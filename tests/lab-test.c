#include "harness/unity.h"
#include "../src/lab.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

int main_exclude(int argc, char **argv);
int main_test_send_packet(int fd, const struct sockaddr *a, socklen_t alen,
                          const lab_packet_t *p);
int main_test_remaining_wait_ms(uint64_t due, uint64_t now);
int main_test_receiver(int fd, struct sockaddr_storage *a, socklen_t alen,
                       const char *file);

/*
 * These tests exercise the pure protocol layer only.  No socket, file, or
 * real clock is involved: a packet is passed directly to the state machine,
 * and advancing the integer "now" value simulates a timer event.
 */
void setUp(void) {}
void tearDown(void) {}

static void relay_process(int fd)
{
    struct sockaddr_storage receiver, sender, from, *target;
    socklen_t receiver_length = sizeof(receiver), sender_length = sizeof(sender);
    socklen_t from_length;
    int have_receiver = 0, have_sender = 0;
    uint8_t buffer[LAB_MAX_PACKET + 128];

    for (;;) {
        ssize_t length;
        from_length = sizeof(from);
        length = recvfrom(fd, buffer, sizeof(buffer), 0,
                          (struct sockaddr *)&from, &from_length);
        if (length < 0) _exit(3);
        if ((size_t)length >= 5 && !memcmp(buffer, "HELLO", 5)) {
            if ((size_t)length >= 10 &&
                !memcmp(buffer + (size_t)length - 5, " recv", 5)) {
                receiver = from;
                receiver_length = from_length;
                have_receiver = 1;
            } else {
                sender = from;
                sender_length = from_length;
                have_sender = 1;
            }
            if (sendto(fd, "OK", 2, 0, (struct sockaddr *)&from, from_length) < 0)
                _exit(4);
            if (have_receiver && have_sender) {
                static const uint8_t malformed[] = {0xff};
                (void)sendto(fd, malformed, sizeof(malformed), 0,
                             (struct sockaddr *)&receiver, receiver_length);
                (void)sendto(fd, "", 0, 0,
                             (struct sockaddr *)&receiver, receiver_length);
            }
            continue;
        }
        if (!have_receiver || !have_sender) continue;
        {
            struct sockaddr_in *from4 = (struct sockaddr_in *)&from;
            struct sockaddr_in *sender4 = (struct sockaddr_in *)&sender;
            target = (from4->sin_port == sender4->sin_port) ? &receiver : &sender;
        }
        (void)sendto(fd, buffer, (size_t)length, 0,
                     (struct sockaddr *)target,
                     target == &receiver ? receiver_length : sender_length);
        if (target == &receiver && length >= (ssize_t)LAB_HEADER_SIZE &&
            buffer[0] == LAB_FIN) {
            static const uint8_t malformed[] = {0xff};
            (void)sendto(fd, malformed, sizeof(malformed), 0,
                         (struct sockaddr *)&receiver, receiver_length);
            (void)sendto(fd, buffer, (size_t)length, 0,
                         (struct sockaddr *)&receiver, receiver_length);
            (void)sendto(fd, "", 0, 0,
                         (struct sockaddr *)&receiver, receiver_length);
        }
    }
}

static void hello_only_relay(int fd, const char *reply, int forward_packets)
{
    struct sockaddr_storage from;
    socklen_t from_length;
    uint8_t buffer[LAB_MAX_PACKET + 128];

    for (;;) {
        ssize_t length;
        from_length = sizeof(from);
        length = recvfrom(fd, buffer, sizeof(buffer), 0,
                          (struct sockaddr *)&from, &from_length);
        if (length < 0) _exit(3);
        if ((size_t)length >= 5 && !memcmp(buffer, "HELLO", 5)) {
            if (sendto(fd, reply, strlen(reply), 0,
                       (struct sockaddr *)&from, from_length) < 0)
                _exit(4);
            if (forward_packets) {
                (void)sendto(fd, "bad", 3, 0,
                             (struct sockaddr *)&from, from_length);
                (void)sendto(fd, "", 0, 0,
                             (struct sockaddr *)&from, from_length);
            }
        } else if (forward_packets) {
            (void)sendto(fd, buffer, (size_t)length, 0,
                         (struct sockaddr *)&from, from_length);
        }
    }
}

static void silent_relay(int fd)
{
    uint8_t buffer[128];
    struct sockaddr_storage from;
    socklen_t length;
    for (;;) {
        length = sizeof(from);
        if (recvfrom(fd, buffer, sizeof(buffer), 0,
                     (struct sockaddr *)&from, &length) < 0) _exit(3);
    }
}

static int make_relay_socket(char *port, size_t port_size)
{
    int fd;
    struct sockaddr_in address;
    socklen_t length = sizeof(address);

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0 ||
        getsockname(fd, (struct sockaddr *)&address, &length) < 0) {
        close(fd);
        return -1;
    }
    snprintf(port, port_size, "%u", (unsigned)ntohs(address.sin_port));
    return fd;
}

static int wait_for_child(pid_t child)
{
    int status;
    if (waitpid(child, &status, 0) < 0) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

void test_main_cli_and_clean_transfer(void)
{
    (void)setvbuf(stdout, NULL, _IONBF, 0);
    int relay_fd;
    struct sockaddr_in relay_address;
    socklen_t relay_length = sizeof(relay_address);
    char port[16];
    char *bad_no_args[] = {"myapp", NULL};
    char *bad_mode[] = {"myapp", "bad", NULL};
    char *bad_session[] = {"myapp", "recv", "-s", "Bad", "127.0.0.1", "x", NULL};
    char *bad_number[] = {"myapp", "send", "-s", "coverage", "-w", "0",
                          "127.0.0.1", "x", NULL};
    char *bad_text_number[] = {"myapp", "send", "-s", "coverage", "-w", "x",
                               "127.0.0.1", "x", NULL};
    char *empty_number[] = {"myapp", "send", "-s", "coverage", "-w", "",
                            "127.0.0.1", "x", NULL};
    char *large_number[] = {"myapp", "send", "-s", "coverage", "-w", "65",
                            "127.0.0.1", "x", NULL};
    char *overflow_number[] = {"myapp", "send", "-s", "coverage", "-w",
                               "999999999999999999999999999999999999",
                               "127.0.0.1", "x", NULL};
    char *bad_timeout[] = {"myapp", "send", "-s", "coverage", "-T", "x",
                           "127.0.0.1", "x", NULL};
    char *bad_port[] = {"myapp", "recv", "-s", "coverage", "-p", "0",
                        "127.0.0.1", "x", NULL};
    char *bad_port_text[] = {"myapp", "recv", "-s", "coverage", "-p", "bad",
                             "127.0.0.1", "x", NULL};
    char *empty_rate[] = {"myapp", "send", "-s", "coverage", "-l", "",
                          "127.0.0.1", "x", NULL};
    char *overflow_rate[] = {"myapp", "send", "-s", "coverage", "-l",
                             "1e9999", "127.0.0.1", "x", NULL};
    char *infinite_rate[] = {"myapp", "send", "-s", "coverage", "-l", "inf",
                             "127.0.0.1", "x", NULL};
    char *nan_rate[] = {"myapp", "send", "-s", "coverage", "-l", "nan",
                        "127.0.0.1", "x", NULL};
    char *bad_rate[] = {"myapp", "send", "-s", "coverage", "-l", "x",
                        "127.0.0.1", "x", NULL};
    char *bad_corrupt_rate[] = {"myapp", "send", "-s", "coverage", "-c", "x",
                                "127.0.0.1", "x", NULL};
    char *bad_dup_rate[] = {"myapp", "send", "-s", "coverage", "-d", "x",
                            "127.0.0.1", "x", NULL};
    char *bad_option[] = {"myapp", "recv", "-s", "coverage", "-x",
                          "127.0.0.1", "x", NULL};
    char *empty_session[] = {"myapp", "recv", "-s", "", "127.0.0.1", "x", NULL};
    char long_session[40];
    char *long_session_args[] = {"myapp", "recv", "-s", long_session,
                                  "127.0.0.1", "x", NULL};
    char *negative_rate[] = {"myapp", "send", "-s", "coverage", "-l", "-1",
                             "127.0.0.1", "x", NULL};
    char *high_rate[] = {"myapp", "send", "-s", "coverage", "-l", "0.6",
                         "127.0.0.1", "x", NULL};
    char *recv_window[] = {"myapp", "recv", "-s", "coverage", "-w", "1",
                           "127.0.0.1", "x", NULL};
    char *recv_timeout[] = {"myapp", "recv", "-s", "coverage", "-T", "1",
                            "127.0.0.1", "x", NULL};
    char *recv_loss[] = {"myapp", "recv", "-s", "coverage", "-l", "0.1",
                         "127.0.0.1", "x", NULL};
    char *recv_corrupt[] = {"myapp", "recv", "-s", "coverage", "-c", "0.1",
                            "127.0.0.1", "x", NULL};
    char *recv_dup[] = {"myapp", "recv", "-s", "coverage", "-d", "0.1",
                        "127.0.0.1", "x", NULL};
    char *zero_timeout[] = {"myapp", "send", "-s", "coverage", "-T", "0",
                            "127.0.0.1", "x", NULL};
    char *extra_arg[] = {"myapp", "recv", "-s", "coverage",
                         "127.0.0.1", "x", "extra", NULL};
    char *invalid_char[] = {"myapp", "recv", "-s", "coverage_",
                            "127.0.0.1", "x", NULL};
    char *invalid_high_char[] = {"myapp", "recv", "-s", "coverage{",
                                 "127.0.0.1", "x", NULL};
    char *missing_session[] = {"myapp", "recv", "127.0.0.1", "x", NULL};
    char *valid_session_chars[] = {"myapp", "recv", "-s", "a1-z9",
                                   "invalid host", "x", NULL};
    char input[] = "/tmp/lab-input-XXXXXX";
    char output[] = "/tmp/lab-output-XXXXXX";
    pid_t relay_child, receiver_child, sender_child;
    int input_fd, output_fd;
    FILE *input_file;

    TEST_ASSERT_EQUAL_INT(0, main_exclude(1, bad_no_args));
    TEST_ASSERT_EQUAL_INT(1, main_exclude(0, bad_no_args));
    TEST_ASSERT_EQUAL_INT(1, main_exclude(2, bad_mode));
    TEST_ASSERT_EQUAL_INT(1, main_exclude(6, bad_session));
    TEST_ASSERT_EQUAL_INT(1, main_exclude(8, bad_number));
    TEST_ASSERT_EQUAL_INT(1, main_exclude(8, bad_text_number));
    TEST_ASSERT_EQUAL_INT(1, main_exclude(8, empty_number));
    TEST_ASSERT_EQUAL_INT(1, main_exclude(8, large_number));
    TEST_ASSERT_EQUAL_INT(1, main_exclude(8, overflow_number));
    TEST_ASSERT_EQUAL_INT(1, main_exclude(8, bad_timeout));
    TEST_ASSERT_EQUAL_INT(1, main_exclude(8, bad_port));
    TEST_ASSERT_EQUAL_INT(1, main_exclude(8, bad_port_text));
    TEST_ASSERT_EQUAL_INT(1, main_exclude(8, empty_rate));
    TEST_ASSERT_EQUAL_INT(1, main_exclude(8, overflow_rate));
    TEST_ASSERT_EQUAL_INT(1, main_exclude(8, infinite_rate));
    TEST_ASSERT_EQUAL_INT(1, main_exclude(8, nan_rate));
    TEST_ASSERT_EQUAL_INT(1, main_exclude(8, bad_rate));
    TEST_ASSERT_EQUAL_INT(1, main_exclude(8, bad_corrupt_rate));
    TEST_ASSERT_EQUAL_INT(1, main_exclude(8, bad_dup_rate));
    TEST_ASSERT_EQUAL_INT(1, main_exclude(8, bad_option));
    TEST_ASSERT_EQUAL_INT(1, main_exclude(6, empty_session));
    memset(long_session, 'a', sizeof(long_session) - 1);
    long_session[sizeof(long_session) - 1] = '\0';
    TEST_ASSERT_EQUAL_INT(1, main_exclude(6, long_session_args));
    TEST_ASSERT_EQUAL_INT(1, main_exclude(8, negative_rate));
    TEST_ASSERT_EQUAL_INT(1, main_exclude(8, high_rate));
    TEST_ASSERT_EQUAL_INT(1, main_exclude(8, recv_window));
    TEST_ASSERT_EQUAL_INT(1, main_exclude(8, recv_timeout));
    TEST_ASSERT_EQUAL_INT(1, main_exclude(8, recv_loss));
    TEST_ASSERT_EQUAL_INT(1, main_exclude(8, recv_corrupt));
    TEST_ASSERT_EQUAL_INT(1, main_exclude(8, recv_dup));
    TEST_ASSERT_EQUAL_INT(1, main_exclude(8, zero_timeout));
    TEST_ASSERT_EQUAL_INT(1, main_exclude(7, extra_arg));
    TEST_ASSERT_EQUAL_INT(1, main_exclude(6, invalid_char));
    TEST_ASSERT_EQUAL_INT(1, main_exclude(6, invalid_high_char));
    TEST_ASSERT_EQUAL_INT(1, main_exclude(4, missing_session));
    TEST_ASSERT_EQUAL_INT(2, main_exclude(6, valid_session_chars));
    {
        lab_packet_t invalid_packet = {0};
        lab_packet_t valid_packet = {0};
        invalid_packet.type = (lab_type_t)99;
        TEST_ASSERT_EQUAL_INT(-1, main_test_send_packet(-1, NULL, 0,
                                                        &invalid_packet));
        valid_packet.type = LAB_DATA;
        valid_packet.length = 1;
        TEST_ASSERT_EQUAL_INT(-1, main_test_send_packet(-1, NULL, 0,
                                                        &valid_packet));
    }
    TEST_ASSERT_EQUAL_INT(10, main_test_remaining_wait_ms(20, 10));
    TEST_ASSERT_EQUAL_INT(0, main_test_remaining_wait_ms(10, 10));

    relay_fd = socket(AF_INET, SOCK_DGRAM, 0);
    TEST_ASSERT_TRUE(relay_fd >= 0);
    memset(&relay_address, 0, sizeof(relay_address));
    relay_address.sin_family = AF_INET;
    relay_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    relay_address.sin_port = 0;
    TEST_ASSERT_EQUAL_INT(0, bind(relay_fd, (struct sockaddr *)&relay_address,
                                  sizeof(relay_address)));
    TEST_ASSERT_EQUAL_INT(0, getsockname(relay_fd,
                                         (struct sockaddr *)&relay_address,
                                         &relay_length));
    snprintf(port, sizeof(port), "%u", (unsigned)ntohs(relay_address.sin_port));

    input_fd = mkstemp(input);
    output_fd = mkstemp(output);
    TEST_ASSERT_TRUE(input_fd >= 0);
    TEST_ASSERT_TRUE(output_fd >= 0);
    close(output_fd);
    input_file = fdopen(input_fd, "wb");
    TEST_ASSERT_NOT_NULL(input_file);
    TEST_ASSERT_EQUAL_UINT32(14, fwrite("coverage-input", 1, 14, input_file));
    TEST_ASSERT_EQUAL_INT(0, fclose(input_file));

    relay_child = fork();
    TEST_ASSERT_TRUE(relay_child >= 0);
    if (relay_child == 0) relay_process(relay_fd);

    receiver_child = fork();
    TEST_ASSERT_TRUE(receiver_child >= 0);
    if (receiver_child == 0) {
        char *args[] = {"myapp", "recv", "-s", "coverage", "-p", port,
                        "127.0.0.1", output, NULL};
        exit(main_exclude(8, args));
    }
    usleep(10000);
    sender_child = fork();
    TEST_ASSERT_TRUE(sender_child >= 0);
    if (sender_child == 0) {
        char *args[] = {"myapp", "send", "-s", "coverage", "-p", port,
                        "-w", "2", "-T", "100", "-l", "0", "-c", "0",
                        "-d", "0", "127.0.0.1", input, NULL};
        exit(main_exclude(18, args));
    }

    TEST_ASSERT_EQUAL_INT(0, wait_for_child(sender_child));
    TEST_ASSERT_EQUAL_INT(0, wait_for_child(receiver_child));
    kill(relay_child, SIGTERM);
    (void)wait_for_child(relay_child);
    close(relay_fd);

    input_file = fopen(input, "rb");
    TEST_ASSERT_NOT_NULL(input_file);
    FILE *output_file = fopen(output, "rb");
    TEST_ASSERT_NOT_NULL(output_file);
    char input_bytes[32], output_bytes[32];
    size_t input_length = fread(input_bytes, 1, sizeof(input_bytes), input_file);
    size_t output_length = fread(output_bytes, 1, sizeof(output_bytes), output_file);
    TEST_ASSERT_EQUAL_UINT32(input_length, output_length);
    TEST_ASSERT_EQUAL_MEMORY(input_bytes, output_bytes, input_length);
    fclose(input_file);
    fclose(output_file);
    unlink(output);

    {
        int error_fd = make_relay_socket(port, sizeof(port));
        pid_t error_relay, error_sender;
        TEST_ASSERT_TRUE(error_fd >= 0);
        error_relay = fork();
        TEST_ASSERT_TRUE(error_relay >= 0);
        if (error_relay == 0) hello_only_relay(error_fd, "ERR no receiver", 0);
        error_sender = fork();
        TEST_ASSERT_TRUE(error_sender >= 0);
        if (error_sender == 0) {
            char *args[] = {"myapp", "send", "-s", "coverage", "-p", port,
                            "-w", "1", "-T", "10", "127.0.0.1", input, NULL};
            exit(main_exclude(12, args));
        }
        TEST_ASSERT_EQUAL_INT(2, wait_for_child(error_sender));
        kill(error_relay, SIGTERM);
        (void)wait_for_child(error_relay);
        close(error_fd);
    }

    {
        int drop_fd = make_relay_socket(port, sizeof(port));
        pid_t drop_relay, drop_sender;
        TEST_ASSERT_TRUE(drop_fd >= 0);
        drop_relay = fork();
        TEST_ASSERT_TRUE(drop_relay >= 0);
        if (drop_relay == 0) hello_only_relay(drop_fd, "OK", 1);
        drop_sender = fork();
        TEST_ASSERT_TRUE(drop_sender >= 0);
        if (drop_sender == 0) {
            char *args[] = {"myapp", "send", "-s", "coverage", "-p", port,
                            "-w", "1", "-T", "10", "127.0.0.1", input, NULL};
            exit(main_exclude(12, args));
        }
        TEST_ASSERT_EQUAL_INT(2, wait_for_child(drop_sender));
        kill(drop_relay, SIGTERM);
        (void)wait_for_child(drop_relay);
        close(drop_fd);
    }

    {
        int quiet_fd = make_relay_socket(port, sizeof(port));
        pid_t quiet_relay, quiet_sender;
        TEST_ASSERT_TRUE(quiet_fd >= 0);
        quiet_relay = fork();
        TEST_ASSERT_TRUE(quiet_relay >= 0);
        if (quiet_relay == 0) hello_only_relay(quiet_fd, "", 0);
        quiet_sender = fork();
        TEST_ASSERT_TRUE(quiet_sender >= 0);
        if (quiet_sender == 0) {
            char *args[] = {"myapp", "send", "-s", "coverage", "-p", port,
                            "-w", "1", "-T", "10", "127.0.0.1", input, NULL};
            exit(main_exclude(12, args));
        }
        TEST_ASSERT_EQUAL_INT(2, wait_for_child(quiet_sender));
        kill(quiet_relay, SIGTERM);
        (void)wait_for_child(quiet_relay);
        close(quiet_fd);
    }

    {
        int idle_fd = make_relay_socket(port, sizeof(port));
        pid_t idle_relay, idle_receiver;
        char idle_output[] = "/tmp/lab-idle-XXXXXX";
        int idle_file = mkstemp(idle_output);
        TEST_ASSERT_TRUE(idle_fd >= 0);
        TEST_ASSERT_TRUE(idle_file >= 0);
        close(idle_file);
        idle_relay = fork();
        TEST_ASSERT_TRUE(idle_relay >= 0);
        if (idle_relay == 0) hello_only_relay(idle_fd, "OK", 0);
        idle_receiver = fork();
        TEST_ASSERT_TRUE(idle_receiver >= 0);
        if (idle_receiver == 0) {
            char *args[] = {"myapp", "recv", "-s", "coverage", "-p", port,
                            "127.0.0.1", idle_output, NULL};
            exit(main_exclude(8, args));
        }
        TEST_ASSERT_EQUAL_INT(2, wait_for_child(idle_receiver));
        kill(idle_relay, SIGTERM);
        (void)wait_for_child(idle_relay);
        close(idle_fd);
        unlink(idle_output);
    }

    {
        int large_fd = make_relay_socket(port, sizeof(port));
        int large_input = open("/tmp/lab-large-input", O_CREAT | O_TRUNC | O_RDWR, 0600);
        pid_t large_relay, large_sender;
        TEST_ASSERT_TRUE(large_fd >= 0);
        TEST_ASSERT_TRUE(large_input >= 0);
        TEST_ASSERT_EQUAL_INT(0, ftruncate(large_input, 16385 * 1024));
        close(large_input);
        large_relay = fork();
        TEST_ASSERT_TRUE(large_relay >= 0);
        if (large_relay == 0) hello_only_relay(large_fd, "OK", 0);
        large_sender = fork();
        TEST_ASSERT_TRUE(large_sender >= 0);
        if (large_sender == 0) {
            char *args[] = {"myapp", "send", "-s", "coverage", "-p", port,
                            "-w", "1", "-T", "10", "127.0.0.1",
                            "/tmp/lab-large-input", NULL};
            exit(main_exclude(12, args));
        }
        TEST_ASSERT_EQUAL_INT(2, wait_for_child(large_sender));
        kill(large_relay, SIGTERM);
        (void)wait_for_child(large_relay);
        close(large_fd);
        unlink("/tmp/lab-large-input");
    }

    {
        int silent_fd = make_relay_socket(port, sizeof(port));
        pid_t silent_child, silent_sender;
        TEST_ASSERT_TRUE(silent_fd >= 0);
        silent_child = fork();
        TEST_ASSERT_TRUE(silent_child >= 0);
        if (silent_child == 0) silent_relay(silent_fd);
        silent_sender = fork();
        TEST_ASSERT_TRUE(silent_sender >= 0);
        if (silent_sender == 0) {
            char *args[] = {"myapp", "send", "-s", "coverage", "-p", port,
                            "-w", "1", "-T", "10", "127.0.0.1", input, NULL};
            exit(main_exclude(12, args));
        }
        TEST_ASSERT_EQUAL_INT(2, wait_for_child(silent_sender));
        kill(silent_child, SIGTERM);
        (void)wait_for_child(silent_child);
        close(silent_fd);
    }
    unlink(input);
}

void test_receiver_fwrite_failure(void)
{
    struct sockaddr_in receiver_address;
    struct sockaddr_storage destination;
    socklen_t address_length = sizeof(receiver_address);
    int receiver_fd = socket(AF_INET, SOCK_DGRAM, 0);
    int sender_fd = socket(AF_INET, SOCK_DGRAM, 0);
    uint8_t wire[LAB_MAX_PACKET];
    size_t wire_length;
    lab_packet_t packet = {0};
    pid_t child;

    TEST_ASSERT_TRUE(receiver_fd >= 0);
    TEST_ASSERT_TRUE(sender_fd >= 0);
    memset(&receiver_address, 0, sizeof(receiver_address));
    receiver_address.sin_family = AF_INET;
    receiver_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    TEST_ASSERT_EQUAL_INT(0, bind(receiver_fd,
                                  (struct sockaddr *)&receiver_address,
                                  sizeof(receiver_address)));
    TEST_ASSERT_EQUAL_INT(0, getsockname(receiver_fd,
                                         (struct sockaddr *)&receiver_address,
                                         &address_length));
    memset(&destination, 0, sizeof(destination));
    memcpy(&destination, &receiver_address, sizeof(receiver_address));

    child = fork();
    TEST_ASSERT_TRUE(child >= 0);
    if (child == 0)
        exit(main_test_receiver(receiver_fd, &destination,
                                sizeof(receiver_address), "/dev/full"));

    packet.type = LAB_DATA;
    packet.length = 1;
    packet.payload[0] = 0x5a;
    TEST_ASSERT_EQUAL_INT(0, lab_encode(&packet, wire, sizeof(wire),
                                        &wire_length));
    TEST_ASSERT_EQUAL_INT((int)wire_length,
                          (int)sendto(sender_fd, wire, wire_length, 0,
                                      (struct sockaddr *)&receiver_address,
                                      sizeof(receiver_address)));
    TEST_ASSERT_EQUAL_INT(2, wait_for_child(child));
    close(sender_fd);
    close(receiver_fd);
}

/* Build a small DATA packet for tests that focus on sequence behavior. */
static void make_data(lab_packet_t *p, uint32_t seq, const char *text)
{
    memset(p, 0, sizeof(*p));
    p->type = LAB_DATA;
    p->seq = seq;
    p->length = (uint16_t)strlen(text);
    memcpy(p->payload, text, p->length);
}

/*
 * Recompute a wire checksum after deliberately changing header bytes.  This
 * lets a test isolate semantic validation from checksum validation.
 */
static void recalculate_checksum(uint8_t *wire, size_t length)
{
    uint16_t check;

    wire[2] = 0;
    wire[3] = 0;
    check = lab_checksum(wire, length);
    wire[2] = (uint8_t)(check >> 8);
    wire[3] = (uint8_t)check;
}

/*
 * Covers the RFC 1071 worked example, odd-length padding, and the basic
 * damage-detection property that changing one bit changes the checksum.
 */
void test_checksum_examples_and_odd_length(void)
{
    uint8_t even[] = {0, 1, 0xf2, 3, 0xf4, 0xf5, 0xf6, 0xf7};
    uint8_t odd[] = {0x12, 0x34, 0x56};

    TEST_ASSERT_EQUAL_HEX16(0x220d, lab_checksum(even, sizeof(even)));
    TEST_ASSERT_EQUAL_HEX16(0x97cb, lab_checksum(odd, sizeof(odd)));
    even[3] ^= 1;
    TEST_ASSERT_NOT_EQUAL_HEX16(0x220d, lab_checksum(even, sizeof(even)));
}

/*
 * Verify DATA and header-only ACK round trips, then verify that a damaged
 * payload, short datagram, length mismatch, and unknown type are rejected.
 */
void test_packet_round_trip_and_validation(void)
{
    lab_packet_t packet, decoded;
    uint8_t wire[LAB_MAX_PACKET];
    size_t wire_length;

    make_data(&packet, 7, "Hi!");
    TEST_ASSERT_EQUAL_INT(0, lab_encode(&packet, wire, sizeof(wire), &wire_length));
    TEST_ASSERT_EQUAL_UINT32(13, wire_length);
    TEST_ASSERT_EQUAL_INT(0, lab_decode(wire, wire_length, &decoded));
    TEST_ASSERT_EQUAL_INT(LAB_DATA, decoded.type);
    TEST_ASSERT_EQUAL_UINT32(7, decoded.seq);
    TEST_ASSERT_EQUAL_UINT16(3, decoded.length);
    TEST_ASSERT_EQUAL_MEMORY(packet.payload, decoded.payload, 3);

    memset(&packet, 0, sizeof(packet));
    packet.type = LAB_ACK;
    packet.seq = 9;
    TEST_ASSERT_EQUAL_INT(0, lab_encode(&packet, wire, sizeof(wire), &wire_length));
    TEST_ASSERT_EQUAL_INT(0, lab_decode(wire, wire_length, &decoded));
    TEST_ASSERT_EQUAL_INT(LAB_ACK, decoded.type);

    wire[wire_length - 1] ^= 1;
    TEST_ASSERT_NOT_EQUAL(0, lab_decode(wire, wire_length, &decoded));
    TEST_ASSERT_NOT_EQUAL(0, lab_decode(wire, LAB_HEADER_SIZE - 1, &decoded));

    TEST_ASSERT_EQUAL_INT(0, lab_encode(&packet, wire, sizeof(wire), &wire_length));
    wire[9] = 4;
    TEST_ASSERT_NOT_EQUAL(0, lab_decode(wire, wire_length, &decoded));
    TEST_ASSERT_EQUAL_INT(0, lab_encode(&packet, wire, sizeof(wire), &wire_length));
    wire[0] = 9;
    wire[2] = wire[3] = 0;
    wire[2] = (uint8_t)(lab_checksum(wire, wire_length) >> 8);
    wire[3] = (uint8_t)lab_checksum(wire, wire_length);
    TEST_ASSERT_NOT_EQUAL(0, lab_decode(wire, wire_length, &decoded));
}

/*
 * Exercise every public encoder guard: null pointers, illegal packet types,
 * payload-bearing ACK/FIN packets, oversized payloads, and small output
 * buffers.
 */
void test_packet_encode_rejects_bad_arguments(void)
{
    lab_packet_t packet;
    uint8_t wire[LAB_MAX_PACKET];
    size_t length;

    memset(&packet, 0, sizeof(packet));
    packet.type = LAB_ACK;
    packet.length = 1;
    TEST_ASSERT_NOT_EQUAL(0, lab_encode(&packet, wire, sizeof(wire), &length));
    packet.type = LAB_FIN;
    packet.length = 1;
    TEST_ASSERT_NOT_EQUAL(0, lab_encode(&packet, wire, sizeof(wire), &length));
    packet.length = 0;
    TEST_ASSERT_NOT_EQUAL(0, lab_encode(&packet, wire, LAB_HEADER_SIZE - 1, &length));
    packet.type = (lab_type_t)9;
    TEST_ASSERT_NOT_EQUAL(0, lab_encode(&packet, wire, sizeof(wire), &length));
    packet.type = LAB_DATA;
    packet.length = LAB_MAX_PAYLOAD + 1;
    TEST_ASSERT_NOT_EQUAL(0, lab_encode(&packet, wire, sizeof(wire), &length));
    packet.length = 0;
    TEST_ASSERT_NOT_EQUAL(0, lab_encode(NULL, wire, sizeof(wire), &length));
    TEST_ASSERT_NOT_EQUAL(0, lab_encode(&packet, NULL, sizeof(wire), &length));
    TEST_ASSERT_NOT_EQUAL(0, lab_encode(&packet, wire, sizeof(wire), NULL));
}

/*
 * Exercise decoder rejection before payload copying, checksum rejection, and
 * the rule that only DATA packets may have a nonzero payload length.
 */
void test_packet_decode_rejects_each_header_error(void)
{
    lab_packet_t packet, decoded;
    uint8_t wire[LAB_MAX_PACKET], oversized[LAB_HEADER_SIZE + LAB_MAX_PAYLOAD + 1];
    size_t wire_length;

    make_data(&packet, 1, "x");
    TEST_ASSERT_EQUAL_INT(0, lab_encode(&packet, wire, sizeof(wire), &wire_length));
    wire[1] = 1;
    TEST_ASSERT_NOT_EQUAL(0, lab_decode(wire, wire_length, &decoded));
    TEST_ASSERT_NOT_EQUAL(0, lab_decode(NULL, wire_length, &decoded));
    TEST_ASSERT_NOT_EQUAL(0, lab_decode(wire, wire_length, NULL));
    TEST_ASSERT_NOT_EQUAL(0, lab_decode(NULL, wire_length, &decoded));
    TEST_ASSERT_NOT_EQUAL(0, lab_decode(wire, wire_length, NULL));
    TEST_ASSERT_NOT_EQUAL(0, lab_decode(NULL, wire_length, NULL));

    TEST_ASSERT_EQUAL_INT(0, lab_encode(&packet, wire, sizeof(wire), &wire_length));
    wire[8] = 4;
    wire[9] = 1;
    TEST_ASSERT_NOT_EQUAL(0, lab_decode(wire, wire_length, &decoded));

    memset(oversized, 0, sizeof(oversized));
    oversized[0] = LAB_DATA;
    oversized[8] = (uint8_t)((LAB_MAX_PAYLOAD + 1) >> 8);
    oversized[9] = (uint8_t)(LAB_MAX_PAYLOAD + 1);
    recalculate_checksum(oversized, sizeof(oversized));
    TEST_ASSERT_NOT_EQUAL(0, lab_decode(oversized, sizeof(oversized), &decoded));

    TEST_ASSERT_EQUAL_INT(0, lab_encode(&packet, wire, sizeof(wire), &wire_length));
    wire[0] = LAB_FIN;
    wire[8] = 0;
    wire[9] = 1;
    recalculate_checksum(wire, LAB_HEADER_SIZE + 1);
    TEST_ASSERT_NOT_EQUAL(0, lab_decode(wire, LAB_HEADER_SIZE + 1, &decoded));

    TEST_ASSERT_EQUAL_INT(0, lab_encode(&packet, wire, sizeof(wire), &wire_length));
    wire[0] = 9;
    recalculate_checksum(wire, wire_length);
    TEST_ASSERT_NOT_EQUAL(0, lab_decode(wire, wire_length, &decoded));

    TEST_ASSERT_EQUAL_INT(0, lab_encode(&packet, wire, sizeof(wire), &wire_length));
    wire[2] ^= 1;
    TEST_ASSERT_NOT_EQUAL(0, lab_decode(wire, wire_length, &decoded));

    TEST_ASSERT_EQUAL_INT(0, lab_encode(&packet, wire, sizeof(wire), &wire_length));
    wire[0] = LAB_ACK;
    wire[2] = wire[3] = 0;
    {
        uint16_t check = lab_checksum(wire, wire_length);
        wire[2] = (uint8_t)(check >> 8);
        wire[3] = (uint8_t)check;
    }
    TEST_ASSERT_NOT_EQUAL(0, lab_decode(wire, wire_length, &decoded));
}

/*
 * Send a future packet first, then the expected packet, then a duplicate.
 * The receiver must deliver only the expected packet and repeat cumulative
 * ACKs without buffering out-of-order data.
 */
void test_receiver_orders_and_reacknowledges(void)
{
    lab_receiver_t receiver;
    lab_packet_t packet, ack;
    uint8_t delivered[LAB_MAX_PAYLOAD];
    size_t delivered_length;

    lab_receiver_init(&receiver);
    make_data(&packet, 1, "later");
    TEST_ASSERT_EQUAL_INT(0, lab_receiver_packet(&receiver, &packet, &ack,
                                                  delivered, sizeof(delivered),
                                                  &delivered_length));
    TEST_ASSERT_EQUAL_UINT32(0, ack.seq);
    TEST_ASSERT_EQUAL_UINT32(0, delivered_length);

    make_data(&packet, 0, "first");
    lab_receiver_packet(&receiver, &packet, &ack, delivered, sizeof(delivered),
                        &delivered_length);
    TEST_ASSERT_EQUAL_UINT32(1, ack.seq);
    TEST_ASSERT_EQUAL_UINT32(5, delivered_length);
    TEST_ASSERT_EQUAL_MEMORY("first", delivered, 5);

    lab_receiver_packet(&receiver, &packet, &ack, delivered, sizeof(delivered),
                        &delivered_length);
    TEST_ASSERT_EQUAL_UINT32(1, ack.seq);
    TEST_ASSERT_EQUAL_UINT32(0, delivered_length);

    make_data(&packet, 1, "");
    lab_receiver_packet(&receiver, &packet, &ack, delivered, sizeof(delivered),
                        &delivered_length);
    TEST_ASSERT_EQUAL_UINT32(2, ack.seq);
    TEST_ASSERT_EQUAL_UINT32(0, delivered_length);

    make_data(&packet, 2, "too large");
    lab_receiver_packet(&receiver, &packet, &ack, delivered, 1,
                        &delivered_length);
    TEST_ASSERT_EQUAL_UINT32(2, ack.seq);
}

/*
 * FIN is accepted only at the expected sequence.  Repeating it after
 * completion must produce the same final ACK and no additional delivery.
 * Null arguments are also checked because this function is public API.
 */
void test_receiver_fin_and_repeated_fin(void)
{
    lab_receiver_t receiver;
    lab_packet_t packet, ack;
    uint8_t delivered[1];
    size_t delivered_length;

    lab_receiver_init(&receiver);
    memset(&packet, 0, sizeof(packet));
    packet.type = LAB_FIN;
    packet.seq = 0;
    lab_receiver_packet(&receiver, &packet, &ack, delivered, sizeof(delivered),
                        &delivered_length);
    TEST_ASSERT_TRUE(receiver.finished);
    TEST_ASSERT_EQUAL_UINT32(1, ack.seq);
    lab_receiver_packet(&receiver, &packet, &ack, delivered, sizeof(delivered),
                        &delivered_length);
    TEST_ASSERT_EQUAL_UINT32(1, ack.seq);
    TEST_ASSERT_EQUAL_INT(-1, lab_receiver_packet(NULL, &packet, &ack, delivered,
                                                   sizeof(delivered),
                                                   &delivered_length));
    TEST_ASSERT_EQUAL_INT(-1, lab_receiver_packet(&receiver, NULL, &ack,
                                                   delivered, sizeof(delivered),
                                                   &delivered_length));
    TEST_ASSERT_EQUAL_INT(-1, lab_receiver_packet(&receiver, &packet, NULL,
                                                   delivered, sizeof(delivered),
                                                   &delivered_length));
    TEST_ASSERT_EQUAL_INT(-1, lab_receiver_packet(&receiver, &packet, &ack,
                                                   delivered, sizeof(delivered),
                                                   NULL));
}

/*
 * Cover initial window filling, timer-not-yet-expired behavior, whole-window
 * retransmission, cumulative ACK sliding, duplicate/future ACK handling,
 * default normalization, zero-capacity output, and ten-timeout give-up.
 */
void test_sender_window_ack_timeout_and_giveup(void)
{
    lab_packet_t packets[5], *out[LAB_MAX_PAYLOAD];
    lab_sender_t sender;
    size_t count;
    unsigned i;

    for (i = 0; i < 4; ++i) make_data(&packets[i], i, "x");
    memset(&packets[4], 0, sizeof(packets[4]));
    packets[4].type = LAB_FIN;
    packets[4].seq = 4;
    lab_sender_init(&sender, packets, 5, 3, 10);
    count = lab_sender_start(&sender, 0, out, 64);
    TEST_ASSERT_EQUAL_UINT32(3, count);
    TEST_ASSERT_EQUAL_UINT32(3, sender.next);
    count = lab_sender_timeout(&sender, 9, out, 64);
    TEST_ASSERT_EQUAL_UINT32(0, count);
    count = lab_sender_timeout(&sender, 10, out, 64);
    TEST_ASSERT_EQUAL_UINT32(3, count);
    TEST_ASSERT_EQUAL_PTR(&packets[0], out[0]);
    count = lab_sender_ack(&sender, 3, 11, out, 64);
    TEST_ASSERT_EQUAL_UINT32(2, count);
    TEST_ASSERT_EQUAL_PTR(&packets[3], out[0]);
    TEST_ASSERT_EQUAL_PTR(&packets[4], out[1]);
    TEST_ASSERT_EQUAL_UINT32(3, sender.base);
    TEST_ASSERT_EQUAL_UINT32(0, lab_sender_ack(&sender, 3, 12, out, 64));
    TEST_ASSERT_EQUAL_UINT32(0, lab_sender_ack(&sender, 5, 13, out, 64));
    TEST_ASSERT_TRUE(lab_sender_done(&sender) > 0);

    lab_sender_init(&sender, packets, 1, 1, 1);
    lab_sender_start(&sender, 0, out, 64);
    for (i = 0; i < 10; ++i) {
        count = lab_sender_timeout(&sender, (uint64_t)(i + 1), out, 64);
        if (i < 9) TEST_ASSERT_EQUAL_UINT32(1, count);
        else TEST_ASSERT_EQUAL_UINT32(0, count);
    }
    TEST_ASSERT_EQUAL_INT(-1, lab_sender_done(&sender));

    lab_sender_init(&sender, packets, 0, 0, 0);
    TEST_ASSERT_EQUAL_UINT32(0, lab_sender_start(&sender, 0, out, 0));
    TEST_ASSERT_FALSE(sender.timer_running);
    TEST_ASSERT_EQUAL_UINT32(0, lab_sender_timeout(&sender, 0, out, 0));

    lab_sender_init(&sender, packets, 1, 1, 10);
    lab_sender_start(&sender, 0, out, 0);
    TEST_ASSERT_EQUAL_UINT32(0, lab_sender_ack(&sender, 2, 1, out, 0));
    TEST_ASSERT_EQUAL_UINT32(0, lab_sender_ack(&sender, 0, 1, out, 0));
    TEST_ASSERT_EQUAL_UINT32(0, lab_sender_timeout(&sender, 5, out, 0));
    TEST_ASSERT_EQUAL_UINT32(0, lab_sender_timeout(&sender, 10, out, 0));

    lab_sender_init(&sender, packets, 2, 2, 10);
    lab_sender_start(&sender, 0, out, 2);
    TEST_ASSERT_EQUAL_UINT32(0, lab_sender_timeout(&sender, 10, out, 0));
}

/*
 * Run a small transfer through a deterministic in-memory channel that drops
 * selected packets and ACKs.  This verifies recovery without involving the
 * relay or any operating-system I/O.
 */
void test_empty_transfer_and_lossy_state_machine_transfer(void)
{
    lab_packet_t packets[4], *sent[64], ack, *retransmit[64];
    lab_sender_t sender;
    lab_receiver_t receiver;
    uint8_t delivered[LAB_MAX_PAYLOAD];
    char result[2048];
    size_t sent_count, delivered_length, result_length = 0;
    uint64_t now = 0;
    unsigned guard = 0;

    memset(packets, 0, sizeof(packets));
    packets[0].type = LAB_DATA;
    packets[0].seq = 0;
    packets[0].length = 1024;
    memset(packets[0].payload, 'A', sizeof(packets[0].payload));
    make_data(&packets[1], 1, "tail");
    packets[2].type = LAB_FIN;
    packets[2].seq = 2;
    lab_sender_init(&sender, packets, 3, 2, 5);
    lab_receiver_init(&receiver);
    sent_count = lab_sender_start(&sender, now, sent, 64);
    while (!lab_sender_done(&sender) && guard++ < 100) {
        size_t i;
        for (i = 0; i < sent_count; ++i) {
            int drop = ((guard + i) % 5 == 0);
            if (drop) continue;
            lab_receiver_packet(&receiver, sent[i], &ack, delivered,
                                sizeof(delivered), &delivered_length);
            if (delivered_length) {
                memcpy(result + result_length, delivered, delivered_length);
                result_length += delivered_length;
            }

            if ((guard + i) % 7 != 0) {
                lab_sender_ack(&sender, ack.seq, now, retransmit, 64);
            }
        }
        now += 5;
        sent_count = lab_sender_timeout(&sender, now, sent, 64);
        if (sent_count == 0 && !sender.timer_running) break;
    }
    TEST_ASSERT_TRUE(lab_sender_done(&sender) > 0);
    TEST_ASSERT_TRUE(receiver.finished);
    TEST_ASSERT_EQUAL_UINT32(1028, result_length);
    TEST_ASSERT_EQUAL_CHAR('A', result[0]);
    TEST_ASSERT_EQUAL_CHAR('t', result[1024]);
}

static unsigned test_random(unsigned *state)
{
    *state = *state * 1103515245u + 12345u;
    return (*state >> 16) & 0x7fffu;
}

/*
 * Apply an independently seeded channel event in each direction:
 *   [0,20)   drop the packet
 *   [20,40)  corrupt one byte/sequence bit
 *   [40,60)  duplicate the packet
 *   [60,100) deliver once
 *
 * ACKs pass through the same model, so both lost/corrupted DATA and lost/
 * corrupted ACKs must be recovered by the sender timer.
 */
static void deliver_with_channel(lab_packet_t *packet, lab_receiver_t *receiver,
                                 lab_sender_t *sender, unsigned *seed,
                                 char *result, size_t *result_length)
{
    lab_packet_t received = *packet;
    lab_packet_t ack;
    lab_packet_t *new_out[64];
    uint8_t delivered[LAB_MAX_PAYLOAD];
    size_t delivered_length;
    unsigned action = test_random(seed) % 100u;
    unsigned copies = action >= 40u && action < 60u ? 2u : 1u;
    unsigned copy;

    if (action < 20u) return;
    if (action >= 20u && action < 40u) {
        if (received.length != 0) {
            received.payload[test_random(seed) % received.length] ^= 1u;
        } else {
            received.seq ^= 1u;
        }
    }
    for (copy = 0; copy < copies; ++copy) {
        lab_receiver_packet(receiver, &received, &ack, delivered,
                            sizeof(delivered), &delivered_length);
        if (delivered_length != 0) {
            memcpy(result + *result_length, delivered, delivered_length);
            *result_length += delivered_length;
        }
        action = test_random(seed) % 100u;
        if (action < 20u) continue;
        if (action >= 20u && action < 40u) ack.seq ^= 1u;
        lab_sender_ack(sender, ack.seq, *result_length, new_out, 64);
    }
}

/*
 * A fixed seed makes this lossy bidirectional scenario reproducible.  The
 * expected byte string is compared after the receiver has accepted DATA in
 * order and the sender has completed the FIN exchange.
 */
void test_seeded_bidirectional_lossy_transfer(void)
{
    lab_packet_t packets[4], *out[64];
    lab_sender_t sender;
    lab_receiver_t receiver;
    char expected[2048], result[2048];
    size_t sent_count, result_length = 0;
    uint64_t now = 0;
    unsigned seed = 7;
    unsigned guard = 0;

    memset(expected, 'A', 1024);
    memcpy(expected + 1024, "reliable-transfer", 17);
    memset(packets, 0, sizeof(packets));
    packets[0].type = LAB_DATA;
    packets[0].seq = 0;
    packets[0].length = 1024;
    memset(packets[0].payload, 'A', 1024);
    packets[1].type = LAB_DATA;
    packets[1].seq = 1;
    packets[1].length = 17;
    memcpy(packets[1].payload, "reliable-transfer", 17);
    packets[2].type = LAB_FIN;
    packets[2].seq = 2;

    lab_sender_init(&sender, packets, 3, 2, 10);
    lab_receiver_init(&receiver);
    sent_count = lab_sender_start(&sender, now, out, 64);
    while (!lab_sender_done(&sender) && guard++ < 500) {
        size_t i;
        for (i = 0; i < sent_count; ++i) {
            deliver_with_channel(out[i], &receiver, &sender, &seed,
                                 result, &result_length);
        }
        now += 10;
        sent_count = lab_sender_timeout(&sender, now, out, 64);
    }
    TEST_ASSERT_TRUE(lab_sender_done(&sender) > 0);
    TEST_ASSERT_TRUE(receiver.finished);
    TEST_ASSERT_EQUAL_UINT32(1041, result_length);
    TEST_ASSERT_EQUAL_MEMORY(expected, result, result_length);
}

/*
 * Verify the two file-boundary cases called out by the assignment: an empty
 * file is represented by FIN sequence zero, while a file exactly 1024 bytes
 * long is one full DATA packet followed by FIN.
 */
void test_empty_and_exact_multiple_transfers(void)
{
    lab_packet_t packets[2], *out[4], ack;
    lab_sender_t sender;
    lab_receiver_t receiver;
    uint8_t delivered[LAB_MAX_PAYLOAD];
    size_t count, delivered_length;

    memset(packets, 0, sizeof(packets));
    packets[0].type = LAB_FIN;
    packets[0].seq = 0;
    lab_sender_init(&sender, packets, 1, 1, 10);
    lab_receiver_init(&receiver);
    count = lab_sender_start(&sender, 0, out, 4);
    TEST_ASSERT_EQUAL_UINT32(1, count);
    lab_receiver_packet(&receiver, out[0], &ack, delivered, sizeof(delivered),
                        &delivered_length);
    lab_sender_ack(&sender, ack.seq, 1, out, 4);
    TEST_ASSERT_TRUE(receiver.finished);
    TEST_ASSERT_TRUE(lab_sender_done(&sender) > 0);

    memset(&packets[0], 0, sizeof(packets[0]));
    packets[0].type = LAB_DATA;
    packets[0].seq = 0;
    packets[0].length = LAB_MAX_PAYLOAD;
    memset(packets[0].payload, 'Z', LAB_MAX_PAYLOAD);
    packets[1].type = LAB_FIN;
    packets[1].seq = 1;
    lab_sender_init(&sender, packets, 2, 2, 10);
    lab_receiver_init(&receiver);
    count = lab_sender_start(&sender, 0, out, 4);
    TEST_ASSERT_EQUAL_UINT32(2, count);
    lab_receiver_packet(&receiver, out[0], &ack, delivered, sizeof(delivered),
                        &delivered_length);
    TEST_ASSERT_EQUAL_UINT32(LAB_MAX_PAYLOAD, delivered_length);
    lab_sender_ack(&sender, ack.seq, 1, out, 4);
    lab_receiver_packet(&receiver, out[1], &ack, delivered, sizeof(delivered),
                        &delivered_length);
    lab_sender_ack(&sender, ack.seq, 2, out, 4);
    TEST_ASSERT_TRUE(receiver.finished);
    TEST_ASSERT_TRUE(lab_sender_done(&sender) > 0);
}

int main(void)
{
    UNITY_BEGIN();
    /* Packet-format and checksum contract. */
    RUN_TEST(test_checksum_examples_and_odd_length);
    RUN_TEST(test_packet_round_trip_and_validation);
    RUN_TEST(test_packet_encode_rejects_bad_arguments);
    RUN_TEST(test_packet_decode_rejects_each_header_error);
    /* Receiver sequencing and cumulative-ACK contract. */
    RUN_TEST(test_receiver_orders_and_reacknowledges);
    RUN_TEST(test_receiver_fin_and_repeated_fin);
    /* Sender window, timer, retransmission, and file-boundary contracts. */
    RUN_TEST(test_sender_window_ack_timeout_and_giveup);
    RUN_TEST(test_empty_transfer_and_lossy_state_machine_transfer);
    RUN_TEST(test_seeded_bidirectional_lossy_transfer);
    RUN_TEST(test_empty_and_exact_multiple_transfers);
    RUN_TEST(test_main_cli_and_clean_transfer);
    RUN_TEST(test_receiver_fwrite_failure);
    return UNITY_END();
}
