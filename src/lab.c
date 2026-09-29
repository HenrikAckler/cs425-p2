#include "lab.h"
#include <string.h>

/*
 * The protocol layer deliberately has no knowledge of sockets, files, or
 * wall-clock time.  The application supplies packets and timestamps, while
 * this file decides what the protocol state should do next.
 */

/*
 * Calculate the RFC 1071 Internet checksum.
 *
 * The checksum is the one's complement of the one's-complement sum of all
 * 16-bit words.  Packet fields are encoded in network byte order, so the
 * first byte of each word is the high byte.  A caller that wants to checksum
 * a packet must leave its two-byte checksum field as zero first.
 */
uint16_t lab_checksum(const uint8_t *data, size_t length)
{
    uint32_t sum = 0;
    size_t i;

    /* Add complete 16-bit words, folding carries as required by RFC 1071. */
    for (i = 0; i + 1 < length; i += 2) {
        sum += ((uint32_t)data[i] << 8) | data[i + 1];
        sum = (sum & 0xffffu) + (sum >> 16);
    }

    /*
     * An odd-length datagram has one final byte.  RFC 1071 pads that byte on
     * the right with zero bits for the checksum calculation only; the wire
     * buffer itself is not modified.
     */
    if (i < length) {
        sum += (uint32_t)data[i] << 8;
        sum = (sum & 0xffffu) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

/*
 * Serialize one logical packet into the assignment's exact wire format:
 *
 *   byte 0       type
 *   byte 1       reserved, always zero
 *   bytes 2..3  checksum
 *   bytes 4..7  sequence number, network byte order
 *   bytes 8..9  payload length, network byte order
 *   bytes 10..  payload
 *
 * The checksum is calculated only after every other field has been written.
 */
int lab_encode(const lab_packet_t *p, uint8_t *out, size_t cap, size_t *n)
{
    uint16_t check;
    size_t total;

    /*
     * Validate before writing.  In particular, ACK and FIN packets are
     * header-only packets; rejecting their payloads prevents the sender and
     * receiver from disagreeing about packet meaning.
     */
    if (!p || !out || !n || p->length > LAB_MAX_PAYLOAD ||
        p->type > LAB_FIN ||
        ((p->type == LAB_ACK || p->type == LAB_FIN) && p->length != 0) ||
        cap < LAB_HEADER_SIZE + p->length) return -1;

    total = LAB_HEADER_SIZE + p->length;

    /*
     * Clearing the complete output makes the reserved byte and checksum
     * field zero before the checksum is computed.  It also avoids leaving
     * uninitialized padding in a DATA packet's header.
     */
    memset(out, 0, total);
    out[0] = (uint8_t)p->type;
    out[4] = (uint8_t)(p->seq >> 24); out[5] = (uint8_t)(p->seq >> 16);
    out[6] = (uint8_t)(p->seq >> 8); out[7] = (uint8_t)p->seq;
    out[8] = (uint8_t)(p->length >> 8); out[9] = (uint8_t)p->length;

    /* The payload is copied only after the length/capacity checks above. */
    if (p->length) memcpy(out + LAB_HEADER_SIZE, p->payload, p->length);

    check = lab_checksum(out, total);
    out[2] = (uint8_t)(check >> 8); out[3] = (uint8_t)check;
    *n = total;
    return 0;
}

/*
 * Parse and validate an incoming datagram.
 *
 * The length field is untrusted network input.  The datagram size is checked
 * before any payload copy, which prevents malformed packets from causing an
 * out-of-bounds read or write.  A checksum failure is treated exactly like a
 * lost packet by the higher-level protocol: the caller receives no packet.
 */
int lab_decode(const uint8_t *data, size_t n, lab_packet_t *p)
{
    uint16_t length;

    /* A complete header is required before reading bytes 8 and 9. */
    if (!data || !p || n < LAB_HEADER_SIZE) return -1;

    length = (uint16_t)(((uint16_t)data[8] << 8) | data[9]);

    /*
     * These checks intentionally happen before checksum verification.  They
     * reject malformed lengths and types without interpreting any payload.
     */
    if (data[1] != 0 || data[0] > LAB_FIN || length > LAB_MAX_PAYLOAD ||
        n != LAB_HEADER_SIZE + length) return -1;

    /* The checksum includes the checksum field and must sum to all ones. */
    if (lab_checksum(data, n) != 0) return -1;

    memset(p, 0, sizeof(*p));
    p->type = (lab_type_t)data[0];
    p->seq = ((uint32_t)data[4] << 24) | ((uint32_t)data[5] << 16) |
             ((uint32_t)data[6] << 8) | data[7];
    p->length = length;

    if (length) memcpy(p->payload, data + LAB_HEADER_SIZE, length);

    /* Only DATA packets may carry payload bytes. */
    if ((p->type != LAB_DATA && length != 0)) return -1;
    return 0;
}

/*
 * Fill the caller's send list from the current sender window.
 *
 * base is the oldest unacknowledged packet and next is the first packet that
 * has never been sent.  Therefore [base, next) is in flight, while packets
 * [next, base + window) are eligible for first transmission.
 */
static size_t fill(lab_sender_t *s, lab_packet_t **out, size_t cap)
{
    size_t n = 0;

    while (s->next < s->count && s->next < s->base + s->window && n < cap)
        out[n++] = &s->packets[s->next++];
    return n;
}

/*
 * Initialize a sender.
 *
 * A zero window or timeout is normalized to one so that the state machine
 * always has progress-capable settings, even when used directly by a test or
 * by a future caller that has not yet performed CLI validation.
 */
void lab_sender_init(lab_sender_t *s, lab_packet_t *p, size_t count,
                     unsigned window, unsigned timeout)
{
    memset(s, 0, sizeof(*s));
    s->packets = p; s->count = count; s->window = window ? window : 1;
    s->timeout_ms = timeout ? timeout : 1;
}

/*
 * Start a transfer by filling the initial window.  The timer starts when the
 * first packet is sent, not when the sender object is initialized.  An empty
 * file still has one packet: the FIN packet constructed by the I/O layer.
 */
size_t lab_sender_start(lab_sender_t *s, uint64_t now, lab_packet_t **out, size_t cap)
{
    size_t n = fill(s, out, cap);

    if (n) { s->timer_running = 1; s->timer_due = now + s->timeout_ms; }
    return n;
}

/*
 * Process a cumulative ACK.
 *
 * ACK k means every packet with sequence number less than k arrived.  An ACK
 * can therefore advance base by several packets at once.  ACKs at or behind
 * base are duplicates; ACKs beyond next cannot describe data sent by this
 * sender and are ignored as invalid.
 */
size_t lab_sender_ack(lab_sender_t *s, uint32_t ack, uint64_t now,
                      lab_packet_t **out, size_t cap)
{
    size_t n;

    if (ack > s->next) return 0;
    if (ack <= s->base) return 0;

    s->base = ack; s->consecutive_timeouts = 0;

    /*
     * When base reaches count, DATA and FIN have both been acknowledged.
     * There is no outstanding packet, so the timer must stop.
     */
    if (s->base == s->count) { s->finished = 1; s->timer_running = 0; return 0; }

    /* A cumulative ACK opens space for new packets at the right of the window. */
    n = fill(s, out, cap);
    s->timer_running = 1; s->timer_due = now + s->timeout_ms;
    return n;
}

/*
 * Process a timer event.
 *
 * A timeout is meaningful only when the timer is active and its deadline has
 * passed.  Go-Back-N retransmits every packet in [base, next), including
 * packets that may have reached the receiver already.  Ten consecutive
 * timeouts without an ACK that advances base terminate the transfer.
 */
size_t lab_sender_timeout(lab_sender_t *s, uint64_t now,
                          lab_packet_t **out, size_t cap)
{
    size_t i, n = 0;

    if (!s->timer_running || now < s->timer_due) return 0;
    s->consecutive_timeouts++;

    if (s->consecutive_timeouts >= 10) { s->finished = -1; s->timer_running = 0; return 0; }

    for (i = s->base; i < s->next && n < cap; ++i) out[n++] = &s->packets[i];
    s->timer_due = now + s->timeout_ms;
    return n;
}

int lab_sender_done(const lab_sender_t *s) { return s->finished; }

/* Start a receiver with no packet delivered and no FIN observed. */
void lab_receiver_init(lab_receiver_t *r) { memset(r, 0, sizeof(*r)); }

/*
 * Process one validated packet and produce the receiver's cumulative ACK.
 *
 * The receiver intentionally has no out-of-order buffer.  DATA is delivered
 * only when its sequence equals expected.  A duplicate or future DATA packet
 * is discarded and receives the same ACK for expected.  FIN follows the same
 * sequence rule and marks the receiver finished after advancing expected.
 */
int lab_receiver_packet(lab_receiver_t *r, const lab_packet_t *p, lab_packet_t *ack,
                        uint8_t *delivered, size_t cap, size_t *dl)
{
    if (!r || !p || !ack || !dl) return -1;
    *dl = 0; memset(ack, 0, sizeof(*ack)); ack->type = LAB_ACK;

    if (p->type == LAB_DATA && p->seq == r->expected && p->length <= cap) {
        if (p->length) memcpy(delivered, p->payload, p->length);
        *dl = p->length; r->expected++;
    } else if (p->type == LAB_FIN && p->seq == r->expected) {
        r->expected++; r->finished = 1;
    }
    ack->seq = r->expected;
    return 0;
}
