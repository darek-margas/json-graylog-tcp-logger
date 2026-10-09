/*
* test_GELFsender.c
*
* Unit tests for the ring buffer, line framing and connect helpers. Linked
* against the same objects as GELFsender; network tests use real loopback
* sockets rather than mocks.
*/

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "ringbuf.h"
#include "framer.h"
#include "net.h"
#include "test_util.h"

/* Pops one entry and checks it is the NUL-terminated string expected. */
static void expect_pop(RingBuffer *rb, const char *expected, int line)
{
    char out[256];
    size_t n;

    memset(out, 0, sizeof(out));
    n = rb_pop(rb, out, sizeof(out));
    if (n != strlen(expected) + 1 || memcmp(out, expected, n) != 0) {
        fprintf(stderr, "%s:%d: expected \"%s\", got %lu bytes \"%.*s\"\n",
                __FILE__, line, expected, (unsigned long)n, (int)n, out);
        test_failures++;
    }
}
#define EXPECT_POP(rb, s) expect_pop((rb), (s), __LINE__)

static void test_rb_fifo(void)
{
    RingBuffer rb;
    char out[16];

    CHECK(rb_init(&rb, 256) == 0);
    CHECK(rb_pop(&rb, out, sizeof(out)) == 0);
    CHECK(rb_push(&rb, "x", 0) == -1);
    CHECK(rb_push(&rb, "one", 4) == 0);
    CHECK(rb_push(&rb, "two", 4) == 0);
    CHECK(rb.count == 2);
    CHECK(rb.used == 2 * (4 + RB_HDR));
    EXPECT_POP(&rb, "one");
    EXPECT_POP(&rb, "two");
    CHECK(rb.count == 0 && rb.used == 0);
    CHECK(rb_pop(&rb, out, sizeof(out)) == 0);
    rb_free(&rb);

    CHECK(rb_init(&rb, RB_MIN_SIZE - 1) == -1);
}

/* Deterministic message i, 1..20 bytes, so headers and payloads land on the
 * wrap point of a 61 byte buffer in every possible way. */
static size_t make_msg(int i, char *buf)
{
    size_t len = 1 + (size_t)(i * 7) % 20;
    size_t j;

    for (j = 0; j < len; j++) {
        buf[j] = (char)(i + (int)j);
    }
    return len;
}

static void test_rb_wraparound(void)
{
    RingBuffer rb;
    char in[32], expect[32], out[32];
    size_t len, elen, n;
    int i;

    CHECK(rb_init(&rb, 61) == 0);
    for (i = 0; i < 2000; i++) {
        len = make_msg(i, in);
        CHECK(rb_push(&rb, in, len) == 0);
        if (i > 0) {
            elen = make_msg(i - 1, expect);
            n = rb_pop(&rb, out, sizeof(out));
            CHECK(n == elen && memcmp(out, expect, n) == 0);
        }
    }
    CHECK(rb.count == 1);
    CHECK(rb.dropped_msgs == 0);
    rb_free(&rb);
}

static void test_rb_eviction_to_three_quarters(void)
{
    RingBuffer rb;
    char msg[16];
    int i;

    /* 100 bytes, 3/4 = 75. Each "msg-NNNNN\0" entry takes 10 + 4 bytes. */
    CHECK(rb_init(&rb, 100) == 0);
    for (i = 0; i < 7; i++) {
        sprintf(msg, "msg-%05d", i);
        CHECK(rb_push(&rb, msg, 10) == 0);
    }
    CHECK(rb.used == 98 && rb.dropped_msgs == 0);

    /* 98 + 14 > 100: evict until used + 14 <= 75, i.e. 3 entries (98 -> 56) */
    CHECK(rb_push(&rb, "msg-00007", 10) == 0);
    CHECK(rb.dropped_msgs == 3);
    CHECK(rb.dropped_bytes == 30);
    CHECK(rb.count == 5);
    CHECK(rb.used == 70);

    /* Room again: the next push evicts nothing */
    CHECK(rb_push(&rb, "msg-00008", 10) == 0);
    CHECK(rb.dropped_msgs == 3 && rb.used == 84);

    for (i = 3; i <= 8; i++) {
        sprintf(msg, "msg-%05d", i);
        EXPECT_POP(&rb, msg);
    }
    CHECK(rb.count == 0);
    rb_free(&rb);
}

static void test_rb_entry_limit(void)
{
    RingBuffer rb;
    char big[80];

    memset(big, 'z', sizeof(big));
    CHECK(rb_max_entry(100) == 71);
    CHECK(rb_init(&rb, 100) == 0);
    CHECK(rb_push(&rb, big, 72) == -1);
    CHECK(rb_push(&rb, big, 71) == 0);
    CHECK(rb_push(&rb, "small-01", 9) == 0); /* 75 + 13 <= 100 */
    CHECK(rb.dropped_msgs == 0);
    CHECK(rb_push(&rb, "small-02", 9) == 0); /* 88 + 13 > 100 */
    CHECK(rb.dropped_msgs == 1 && rb.dropped_bytes == 71);
    EXPECT_POP(&rb, "small-01");
    EXPECT_POP(&rb, "small-02");
    rb_free(&rb);
}

static void test_framer_lines(void)
{
    RingBuffer rb;
    Framer f;
    const char *s = "hello\nworld\n";
    size_t i;

    CHECK(rb_init(&rb, 4096) == 0);
    CHECK(framer_init(&f, 16) == 0);

    /* several lines in one read */
    framer_feed(&f, &rb, "a\nbb\nccc\n", 9);
    CHECK(rb.count == 3);
    EXPECT_POP(&rb, "a");
    EXPECT_POP(&rb, "bb");
    EXPECT_POP(&rb, "ccc");

    /* one byte per read */
    for (i = 0; i < strlen(s); i++) {
        framer_feed(&f, &rb, s + i, 1);
    }
    CHECK(rb.count == 2);
    EXPECT_POP(&rb, "hello");
    EXPECT_POP(&rb, "world");

    /* empty lines are skipped */
    framer_feed(&f, &rb, "\n\n\nx\n", 5);
    CHECK(rb.count == 1);
    EXPECT_POP(&rb, "x");

    /* unterminated tail is kept until flush */
    framer_feed(&f, &rb, "tail", 4);
    CHECK(rb.count == 0);
    framer_flush(&f, &rb);
    EXPECT_POP(&rb, "tail");
    framer_flush(&f, &rb);
    CHECK(rb.count == 0);

    CHECK(f.oversized == 0);
    framer_free(&f);
    rb_free(&rb);
}

static void test_framer_oversized(void)
{
    RingBuffer rb;
    Framer f;

    CHECK(rb_init(&rb, 4096) == 0);
    CHECK(framer_init(&f, 16) == 0); /* 15 characters + NUL */

    framer_feed(&f, &rb, "0123456789abcde\n", 16);
    EXPECT_POP(&rb, "0123456789abcde");

    framer_feed(&f, &rb, "0123456789abcdef\nok\n", 20);
    CHECK(f.oversized == 1);
    CHECK(rb.count == 1);
    EXPECT_POP(&rb, "ok");

    /* too long only after several reads; counted once, rest skipped */
    framer_feed(&f, &rb, "0123456789", 10);
    framer_feed(&f, &rb, "0123456789", 10);
    framer_feed(&f, &rb, "0123456789", 10);
    framer_feed(&f, &rb, "\nnext\n", 6);
    CHECK(f.oversized == 2);
    CHECK(rb.count == 1);
    EXPECT_POP(&rb, "next");

    /* unterminated oversized tail is not flushed */
    framer_feed(&f, &rb, "0123456789abcdefgh", 18);
    framer_flush(&f, &rb);
    CHECK(rb.count == 0);
    CHECK(f.oversized == 3);

    framer_free(&f);
    rb_free(&rb);
}

static void test_net_parse(void)
{
    struct sockaddr_in a;

    CHECK(net_parse_addr("127.0.0.1", 12201, &a) == 0);
    CHECK(a.sin_family == AF_INET);
    CHECK(ntohs(a.sin_port) == 12201);
    CHECK(ntohl(a.sin_addr.s_addr) == INADDR_LOOPBACK);
    CHECK(net_parse_addr("999.1.1.1", 1, &a) == -1);
    CHECK(net_parse_addr("graylog.local", 1, &a) == -1);
    CHECK(net_parse_addr(NULL, 1, &a) == -1);
    CHECK(net_parse_addr("127.0.0.1", 0, &a) == -1);
    CHECK(net_parse_addr("127.0.0.1", 65536, &a) == -1);
}

static int wait_connected(int fd)
{
    struct pollfd p;

    p.fd = fd;
    p.events = POLLOUT;
    if (poll(&p, 1, 3000) != 1) {
        return ETIMEDOUT;
    }
    return net_connect_result(fd);
}

static void test_net_connect(void)
{
    struct sockaddr_in a;
    char got[8];
    int lfd, port, fd, c, r;

    /* success against a real listener */
    lfd = tu_listen(0);
    CHECK(lfd >= 0);
    port = tu_port(lfd);
    CHECK(net_parse_addr("127.0.0.1", port, &a) == 0);
    r = net_connect_start(&a, &fd);
    CHECK(r == 0 || r == 1);
    if (r >= 0) {
        CHECK(wait_connected(fd) == 0);
        c = tu_accept(lfd, 3000);
        CHECK(c >= 0);
        if (c >= 0) {
            CHECK(send(fd, "ping", 5, 0) == 5);
            CHECK(recv(c, got, 5, MSG_WAITALL) == 5);
            CHECK(memcmp(got, "ping", 5) == 0);
            close(c);
        }
        close(fd);
    }
    close(lfd);

    /* refused when nothing listens */
    CHECK(net_parse_addr("127.0.0.1", tu_free_port(), &a) == 0);
    r = net_connect_start(&a, &fd);
    if (r == -1) {
        CHECK(errno == ECONNREFUSED);
    } else {
        CHECK(wait_connected(fd) == ECONNREFUSED);
        close(fd);
    }
}

int main(void)
{
    alarm(120); /* a hang fails the test instead of blocking the build */

    test_rb_fifo();
    test_rb_wraparound();
    test_rb_eviction_to_three_quarters();
    test_rb_entry_limit();
    test_framer_lines();
    test_framer_oversized();
    test_net_parse();
    test_net_connect();

    if (test_failures > 0) {
        fprintf(stderr, "%d unit test check(s) failed.\n", test_failures);
        return 1;
    }
    printf("All unit tests passed.\n");
    return 0;
}
