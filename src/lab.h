#ifndef LAB_H
#define LAB_H

#include <stddef.h>
#include <stdint.h>

#define LAB_HEADER_SIZE 10u
#define LAB_MAX_PAYLOAD 1024u
#define LAB_MAX_PACKET (LAB_HEADER_SIZE + LAB_MAX_PAYLOAD)

typedef enum { LAB_DATA = 0, LAB_ACK = 1, LAB_FIN = 2 } lab_type_t;
typedef struct {
    lab_type_t type;
    uint32_t seq;
    uint16_t length;
    uint8_t payload[LAB_MAX_PAYLOAD];
} lab_packet_t;

uint16_t lab_checksum(const uint8_t *data, size_t length);
int lab_encode(const lab_packet_t *packet, uint8_t *out, size_t capacity,
               size_t *length);
int lab_decode(const uint8_t *data, size_t length, lab_packet_t *packet);

typedef struct {
    lab_packet_t *packets;
    size_t count;
    uint32_t base, next;
    unsigned window, timeout_ms, consecutive_timeouts;
    uint64_t timer_due;
    int timer_running, finished;
} lab_sender_t;

void lab_sender_init(lab_sender_t *sender, lab_packet_t *packets, size_t count,
                     unsigned window, unsigned timeout_ms);
size_t lab_sender_start(lab_sender_t *sender, uint64_t now,
                        lab_packet_t **out, size_t capacity);
size_t lab_sender_ack(lab_sender_t *sender, uint32_t ack, uint64_t now,
                      lab_packet_t **out, size_t capacity);
size_t lab_sender_timeout(lab_sender_t *sender, uint64_t now,
                          lab_packet_t **out, size_t capacity);
int lab_sender_done(const lab_sender_t *sender);

typedef struct {
    uint32_t expected;
    int finished;
} lab_receiver_t;

void lab_receiver_init(lab_receiver_t *receiver);
int lab_receiver_packet(lab_receiver_t *receiver, const lab_packet_t *packet,
                        lab_packet_t *ack, uint8_t *delivered, size_t capacity,
                        size_t *delivered_length);

#endif
