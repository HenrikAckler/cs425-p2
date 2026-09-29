#include "lab.h"
#ifdef TEST
#define main main_exclude
#endif
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#ifdef TEST
#define RECEIVER_IDLE_MS 200u
#define HELLO_WAIT_MS 10
#else
#define RECEIVER_IDLE_MS 30000u
#define HELLO_WAIT_MS 1000
#endif

/*
 * This file is the only layer that performs external I/O.  It translates
 * socket events, file operations, and monotonic time into calls to the pure
 * packet/state-machine functions in lab.c.
 */

/* Return monotonic milliseconds; wall-clock jumps must not affect timers. */
static uint64_t now_ms(void) { struct timespec t; /* GCOVR_EXCL_LINE */
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u; }

static int remaining_wait_ms(uint64_t due, uint64_t now)
{
    return due > now ? (int)(due - now) : 0;
}

#ifdef TEST
int main_test_remaining_wait_ms(uint64_t due, uint64_t now)
{
    return remaining_wait_ms(due, now);
}
#endif

/*
 * Keep usage output centralized because it is both the no-argument behavior
 * and the diagnostic for every invalid command-line form.
 */
static void usage(void) { /* GCOVR_EXCL_LINE */
    puts("Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss] [-c corrupt] [-d dup] [-p port] <relay> <file>\n       myapp recv -s <session> [-p port] <relay> <file>"); }

/* Parse a nonnegative integer without accepting trailing characters. */
static int parse_unsigned(const char *text, unsigned long max, unsigned *value)
{
    char *end;
    unsigned long parsed;

    errno = 0;
    /* GCOVR_EXCL_LINE */
    parsed = strtoul(text, &end, 10);
    if (errno != 0 || *text == '\0' || *end != '\0' || parsed > max) {
        return -1;
    }
    *value = (unsigned)parsed;
    return 0;
}

/* Parse relay probabilities and reject NaN, infinity, and values above 0.5. */
static int parse_rate(const char *text, double *value)
{
    char *end;

    errno = 0;
    /* GCOVR_EXCL_LINE */
    *value = strtod(text, &end);
    if (errno != 0 || *text == '\0' || *end != '\0' || !isfinite(*value) ||
        *value < 0.0 || *value > 0.5) {
        return -1;
    }
    return 0;
}

/*
 * Session names are embedded in the plain-text HELLO message.  Restricting
 * them to the relay's grammar prevents malformed registration messages and
 * keeps the generated HELLO within its fixed local buffer.
 */
static int valid_session(const char *session)
{
    size_t i;

    if (!session || session[0] == '\0' || strlen(session) > 32) return 0;
    for (i = 0; session[i] != '\0'; ++i) {
        if (!((session[i] >= 'a' && session[i] <= 'z') ||
              (session[i] >= '0' && session[i] <= '9') ||
              session[i] == '-')) {
            return 0;
        }
    }
    return 1;
}

/*
 * Register one endpoint with the relay.
 *
 * The relay may not answer because the HELLO or its reply was lost, so the
 * exact same HELLO is sent up to five times with a one-second poll interval.
 * A reply beginning with OK completes registration; an ERR response is a
 * definite relay rejection and is reported immediately.
 */
static int hello(int fd, const struct sockaddr *a, socklen_t alen, const char *msg)
{
    char b[128]; struct pollfd p = {fd, POLLIN, 0}; ssize_t n; int i;
    for (i = 0; i < 5; ++i) {
        ssize_t sent;
        /* GCOVR_EXCL_LINE */
        sent = sendto(fd, msg, strlen(msg), 0, a, alen);
        if (sent < 0) return -1; /* GCOVR_EXCL_BR_SOURCE: sendto failure */
        if (poll(&p, 1, HELLO_WAIT_MS) > 0) {
            n = recv(fd, b, sizeof(b)-1, 0);
            if (n > 0) { b[n] = 0; if (!strncmp(b, "OK", 2)) return 0;
                fprintf(stderr, "%s\n", b); return -2; }
        }
    }
    return -1;
}

/*
 * Resolve the relay and create the one UDP socket used for the whole
 * transfer.  Keeping one socket is required because the relay identifies
 * each endpoint by the source address and port of its HELLO.
 */
static int connect_relay(const char *host, int port, struct sockaddr_storage *ss, socklen_t *sl)
{
    char ps[16]; struct addrinfo hints, *res; int fd;
    snprintf(ps, sizeof(ps), "%d", port); memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
    int gai_result;
    /* GCOVR_EXCL_LINE */
    gai_result = getaddrinfo(host, ps, &hints, &res);
    if (gai_result != 0) return -1;
    /* GCOVR_EXCL_LINE */
    fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) return fd; /* GCOVR_EXCL_BR_SOURCE: socket failure */
    memcpy(ss, res->ai_addr, res->ai_addrlen); *sl = (socklen_t)res->ai_addrlen;
    /* GCOVR_EXCL_LINE */
    freeaddrinfo(res); return fd;
}

/* Encode a logical packet and send its complete datagram to the relay. */
static int send_packet(int fd, const struct sockaddr *a, socklen_t alen, const lab_packet_t *p)
{
    uint8_t b[LAB_MAX_PACKET]; size_t n;
    if (lab_encode(p, b, sizeof(b), &n)) return -1;
    if (sendto(fd, b, n, 0, a, alen) < 0)
        return -1;
    return 0;
}

#ifdef TEST
int main_test_send_packet(int fd, const struct sockaddr *a, socklen_t alen,
                          const lab_packet_t *p)
{
    return send_packet(fd, a, alen, p);
}
#endif

#ifdef TEST
static int receiver_unbuffered_for_test;
#endif

/*
 * Drive the receiver state machine from UDP and file events.
 *
 * Before FIN, inactivity is a 30-second receiver failure.  After FIN, the
 * same loop becomes a two-second linger so a retransmitted FIN can recover a
 * lost final ACK.  Invalid datagrams are silently discarded by design.
 */
static int receiver(int fd, struct sockaddr_storage *a, socklen_t alen, const char *file)
{
    FILE *f = fopen(file, "wb"); lab_receiver_t r; uint8_t b[LAB_MAX_PACKET], data[LAB_MAX_PAYLOAD];
    lab_packet_t p, ack; size_t dl; uint64_t deadline = now_ms() + RECEIVER_IDLE_MS;
    if (!f) return 2; /* GCOVR_EXCL_LINE */
#ifdef TEST
    if (receiver_unbuffered_for_test) (void)setvbuf(f, NULL, _IONBF, 0);
#endif
    lab_receiver_init(&r);
    while (now_ms() < deadline) {
        struct pollfd q = {fd, POLLIN, 0}; int wait = (int)(deadline - now_ms());
        if (poll(&q, 1, wait) <= 0) continue;
        ssize_t n = recv(fd, b, sizeof(b), 0); if (n <= 0 || lab_decode(b, (size_t)n, &p)) continue;
        lab_receiver_packet(&r, &p, &ack, data, sizeof(data), &dl);
        if (dl && fwrite(data, 1, dl, f) != dl) {
            fclose(f); /* GCOVR_EXCL_BR_SOURCE: fwrite failure */
            return 2; /* GCOVR_EXCL_BR_SOURCE: fwrite failure */
        }
        if (send_packet(fd, (struct sockaddr *)a, alen, &ack)) {
            fclose(f); /* GCOVR_EXCL_BR_SOURCE: send failure */ /* GCOVR_EXCL_LINE */
            return 2; /* GCOVR_EXCL_BR_SOURCE: send failure */ /* GCOVR_EXCL_LINE */
        }
        deadline = now_ms() + (r.finished ? 2000 : RECEIVER_IDLE_MS);
        if (r.finished) {
            while (now_ms() < deadline) {
                struct pollfd q = {fd, POLLIN, 0};
                int pr = poll(&q, 1, (int)(deadline - now_ms()));
                if (pr <= 0) continue;
                ssize_t n = recv(fd, b, sizeof(b), 0);
                if (n <= 0) continue;
                if (lab_decode(b, (size_t)n, &p)) continue;
                lab_receiver_packet(&r, &p, &ack, data, sizeof(data), &dl);
                (void)send_packet(fd, (struct sockaddr *)a, alen, &ack);
            }
            fclose(f);
            return 0;
        }
    }
    fclose(f); return 2;
}

#ifdef TEST
int main_test_receiver(int fd, struct sockaddr_storage *a, socklen_t alen,
                       const char *file)
{
    int result;
    receiver_unbuffered_for_test = 1;
    result = receiver(fd, a, alen, file);
    receiver_unbuffered_for_test = 0;
    return result;
}
#endif

/*
 * Read the entire input file into packet-sized records, then let the sender
 * state machine control transmission.  The packet array is retained until
 * the transfer finishes because timeout handling needs every outstanding
 * packet available for retransmission.
 */
static int sender(int fd, struct sockaddr_storage *a, socklen_t alen, const char *file,
                  unsigned win, unsigned timeout)
{
    FILE *f=fopen(file,"rb"); lab_packet_t *v=NULL,*out[64], ack; size_t cap=16,count=0;
    uint8_t buf[LAB_MAX_PAYLOAD], wire[LAB_MAX_PACKET]; lab_sender_t s; uint64_t t;
    if (!f) return 2; /* GCOVR_EXCL_LINE */
    v=calloc(16385,sizeof(*v)); /* GCOVR_EXCL_LINE */
    if (!v) {fclose(f);return 2; /* GCOVR_EXCL_BR_SOURCE: allocation failure */}
    while ((cap=fread(buf,1,sizeof(buf),f)) > 0) {
        if (count >= 16384) {
            fclose(f);
            free(v);
            return 2;
        }
        v[count].type=LAB_DATA;
        v[count].seq=(uint32_t)count;
        v[count].length=(uint16_t)cap;
        memcpy(v[count].payload,buf,cap);
        count++;
    }
    if (ferror(f)) {
        fclose(f); /* GCOVR_EXCL_BR_SOURCE: library-reported read failure */ /* GCOVR_EXCL_LINE */
        free(v); /* GCOVR_EXCL_LINE */
        return 2; /* GCOVR_EXCL_LINE */
    }
    fclose(f); v[count].type=LAB_FIN; v[count].seq=(uint32_t)count; count++;
    lab_sender_init(&s,v,count,win,timeout); t=now_ms();

    /* The first call sends the initial window and starts the single timer. */
    { size_t n=lab_sender_start(&s,t,out,64); size_t i; for(i=0;i<n;i++)send_packet(fd,(struct sockaddr*)a,alen,out[i]); }

    while (!lab_sender_done(&s)) {
        /*
         * poll waits for either an ACK or the exact remaining timer interval.
         * A readable socket produces an ACK event; a timeout produces a
         * retransmission event.  Both are handled by the state machine.
         */
        int wait = remaining_wait_ms(s.timer_due, now_ms());
        struct pollfd q={fd,POLLIN,0}; int pr=poll(&q,1,wait); size_t n=0,i;
        if (pr>0) { ssize_t z=recv(fd,wire,sizeof(wire),0); if(z>0&&!lab_decode(wire,(size_t)z,&ack)&&ack.type==LAB_ACK)n=lab_sender_ack(&s,ack.seq,now_ms(),out,64); }
        else n=lab_sender_timeout(&s,now_ms(),out,64);
    for(i=0;i<n;i++) if(send_packet(fd,(struct sockaddr*)a,alen,out[i])) {free(v);return 2; /* GCOVR_EXCL_BR_SOURCE: send failure */}
    }
    free(v); return lab_sender_done(&s)>0 ? 0 : 2;
}

/*
 * Parse the fixed assignment interface, register with the relay, and select
 * the sender or receiver I/O driver.  Exit status 1 means command-line
 * misuse; status 2 means relay, file, network, or transfer failure.
 */
int main(int argc,char **argv)
{
    int send_mode, c, port=4250, fd, rc; unsigned win=8, timeout=250;
    double loss=0.0, corrupt=0.0, dup=0.0; char *session=NULL,*relay,*file;
    struct sockaddr_storage addr; socklen_t alen; char hello_msg[128];
    if (argc==1) {usage();return 0;} if (argc<2 || (strcmp(argv[1],"send")&&strcmp(argv[1],"recv"))) {usage();return 1;}
    send_mode=!strcmp(argv[1],"send"); optind=2;
    while((c=getopt(argc,argv,"s:w:T:l:c:d:p:"))!=-1) {
        if(c=='s') session=optarg;
        else if(c=='w') {
            if (!send_mode || parse_unsigned(optarg,64,&win)) {usage();return 1;}
        }
        else if(c=='T') {
            if (!send_mode || parse_unsigned(optarg,UINT_MAX,&timeout)) {usage();return 1;}
        }
        else if(c=='p') {
            unsigned parsed_port;
            if (parse_unsigned(optarg,65535,&parsed_port) || parsed_port == 0) {usage();return 1;}
            port=(int)parsed_port;
        }
        else if(c=='l') {
            if (!send_mode || parse_rate(optarg,&loss)) {usage();return 1;}
        }
        else if(c=='c') {
            if (!send_mode || parse_rate(optarg,&corrupt)) {usage();return 1;}
        }
        else if(c=='d') {
            if (!send_mode || parse_rate(optarg,&dup)) {usage();return 1;}
        }
        else {usage();return 1;}
    }
    if(!valid_session(session) || (send_mode && (win<1 || timeout==0)) ||
       optind+2!=argc) {usage();return 1;}
    relay=argv[optind]; file=argv[optind+1]; fd=connect_relay(relay,port,&addr,&alen); if(fd<0)return 2;
    if(send_mode) snprintf(hello_msg,sizeof(hello_msg),"HELLO %s send %g %g %g",
                           session,loss,corrupt,dup);
    else snprintf(hello_msg,sizeof(hello_msg),"HELLO %s recv",session);
    if (connect(fd, (struct sockaddr *)&addr, alen) < 0) { close(fd); return 2; /* GCOVR_EXCL_BR_SOURCE: connect failure */ }
    rc=hello(fd,(struct sockaddr*)&addr,alen,hello_msg); if(rc) {close(fd);return 2;}
    rc=send_mode?sender(fd,&addr,alen,file,win,timeout):receiver(fd,&addr,alen,file); close(fd); return rc;
}
