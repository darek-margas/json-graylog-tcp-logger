/*
* test_integration.c
*
* Black-box tests of the real GELFsender binary: it is started with a pipe as
* stdin, exactly like httpd does, and talks to loopback listeners acting as
* Graylog servers. Nothing is mocked and no special build is involved.
*
* Usage: test_integration [path/to/GELFsender]
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <unistd.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>

#include "test_util.h"

static const char *sender_bin = "./GELFsender";

typedef struct {
    pid_t pid;
    int in; /* write end of the sender's stdin */
} Proc;

typedef struct {
    char *data;
    size_t len, cap;
} Buf;

static int start_sender(Proc *p, const char *const args[])
{
    char *argv[20];
    int pfd[2], i;

    p->pid = -1;
    p->in = -1;
    if (pipe(pfd) < 0) {
        return -1;
    }
    p->pid = fork();
    if (p->pid < 0) {
        return -1;
    }
    if (p->pid == 0) {
        dup2(pfd[0], STDIN_FILENO);
        close(pfd[0]);
        close(pfd[1]);
        argv[0] = (char *)sender_bin;
        for (i = 0; args[i] != NULL && i < 18; i++) {
            argv[i + 1] = (char *)args[i];
        }
        argv[i + 1] = NULL;
        execv(sender_bin, argv);
        _exit(127);
    }
    close(pfd[0]);
    p->in = pfd[1];
    return 0;
}

/* Returns the exit status, or -1 if it had to be killed. */
static int wait_exit(Proc *p, int timeout_ms)
{
    double deadline = tu_now() + timeout_ms / 1000.0;
    int status;

    if (p->in >= 0) {
        close(p->in);
        p->in = -1;
    }
    while (tu_now() < deadline) {
        if (waitpid(p->pid, &status, WNOHANG) == p->pid) {
            return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        }
        tu_sleep_ms(20);
    }
    kill(p->pid, SIGKILL);
    waitpid(p->pid, &status, 0);
    return -1;
}

static void write_all(int fd, const char *data, size_t n)
{
    ssize_t w;

    while (n > 0) {
        w = write(fd, data, n);
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            CHECK(!"write to sender stdin failed");
            return;
        }
        data += w;
        n -= (size_t)w;
    }
}

static int buf_frames(const Buf *b)
{
    size_t i;
    int n = 0;

    for (i = 0; i < b->len; i++) {
        if (b->data[i] == '\0') {
            n++;
        }
    }
    return n;
}

/* Reads until `want` NUL-delimited frames arrived, EOF, or timeout. */
static int recv_frames(int fd, Buf *b, int want, int timeout_ms)
{
    double deadline = tu_now() + timeout_ms / 1000.0;
    struct pollfd p;
    ssize_t n;
    int left;

    while (buf_frames(b) < want) {
        left = (int)((deadline - tu_now()) * 1000.0);
        if (left <= 0) {
            break;
        }
        p.fd = fd;
        p.events = POLLIN;
        if (poll(&p, 1, left) != 1) {
            break;
        }
        if (b->cap - b->len < 65536) {
            b->cap = b->cap * 2 + 65536;
            b->data = (char *)realloc(b->data, b->cap);
        }
        n = recv(fd, b->data + b->len, b->cap - b->len, 0);
        if (n <= 0) {
            break;
        }
        b->len += (size_t)n;
    }
    return buf_frames(b);
}

static int expect_frames(int fd, const char *expected, size_t len, int line)
{
    Buf b = {NULL, 0, 0};
    int want = 0, ok;
    size_t i;

    for (i = 0; i < len; i++) {
        if (expected[i] == '\0') {
            want++;
        }
    }
    recv_frames(fd, &b, want, 3000);
    ok = b.len == len && memcmp(b.data, expected, len) == 0;
    if (!ok) {
        fprintf(stderr, "%s:%d: expected %lu bytes, got %lu\n",
                __FILE__, line, (unsigned long)len, (unsigned long)b.len);
        test_failures++;
    }
    free(b.data);
    return ok;
}
#define EXPECT_FRAMES(fd, lit) expect_frames((fd), (lit), sizeof(lit) - 1, __LINE__)

static void test_lines_and_long_message(void)
{
    const char *args[8];
    char port[16];
    Proc p;
    Buf b = {NULL, 0, 0};
    char *big;
    size_t i, big_len = 300000;
    int l, c, ok;

    l = tu_listen(0);
    sprintf(port, "%d", tu_port(l));
    args[0] = "-i"; args[1] = "127.0.0.1"; args[2] = "-n"; args[3] = port;
    args[4] = "-r"; args[5] = "1"; args[6] = NULL;
    CHECK(start_sender(&p, args) == 0);

    c = tu_accept(l, 3000);
    CHECK(c >= 0);
    if (c >= 0) {
        /* several lines in one pipe write must become separate messages */
        write_all(p.in, "a\nbb\nccc\n", 9);
        EXPECT_FRAMES(c, "a\0bb\0ccc\0");

        /* a line far longer than one read() or one MTU arrives whole */
        big = (char *)malloc(big_len + 1);
        memset(big, 'x', big_len);
        big[big_len] = '\n';
        write_all(p.in, big, big_len + 1);
        CHECK(recv_frames(c, &b, 1, 5000) == 1);
        ok = b.len == big_len + 1 && b.data[big_len] == '\0';
        for (i = 0; ok && i < big_len; i++) {
            ok = b.data[i] == 'x';
        }
        CHECK(ok);
        free(big);
        free(b.data);
    }

    CHECK(wait_exit(&p, 5000) == 0);
    if (c >= 0) {
        close(c);
    }
    close(l);
}

static void test_oversized_line_dropped(void)
{
    const char *args[12];
    char port[16], line[5002];
    Proc p;
    int l, c;

    l = tu_listen(0);
    sprintf(port, "%d", tu_port(l));
    args[0] = "-i"; args[1] = "127.0.0.1"; args[2] = "-n"; args[3] = port;
    args[4] = "-b"; args[5] = "4096"; args[6] = "-s"; args[7] = "1024";
    args[8] = NULL;
    CHECK(start_sender(&p, args) == 0);

    c = tu_accept(l, 3000);
    CHECK(c >= 0);
    if (c >= 0) {
        memset(line, 'y', sizeof(line));
        line[sizeof(line) - 1] = '\n';
        write_all(p.in, "ok1\n", 4);
        write_all(p.in, line, sizeof(line));
        write_all(p.in, "ok2\n", 4);
        EXPECT_FRAMES(c, "ok1\0ok2\0");
    }

    CHECK(wait_exit(&p, 5000) == 0);
    if (c >= 0) {
        close(c);
    }
    close(l);
}

static void test_failover_and_return(void)
{
    const char *args[12];
    char port1[16], port2[16];
    Proc p;
    int l1, l2, c1, c2, p1;

    l1 = tu_listen(0);
    l2 = tu_listen(0);
    p1 = tu_port(l1);
    sprintf(port1, "%d", p1);
    sprintf(port2, "%d", tu_port(l2));
    args[0] = "-i"; args[1] = "127.0.0.1"; args[2] = "-n"; args[3] = port1;
    args[4] = "-j"; args[5] = "127.0.0.1"; args[6] = "-m"; args[7] = port2;
    args[8] = "-r"; args[9] = "1"; args[10] = NULL;
    CHECK(start_sender(&p, args) == 0);

    c1 = tu_accept(l1, 3000);
    c2 = tu_accept(l2, 3000);
    CHECK(c1 >= 0 && c2 >= 0);
    if (c1 >= 0 && c2 >= 0) {
        write_all(p.in, "one\n", 4);
        EXPECT_FRAMES(c1, "one\0");

        /* primary goes away: next message must reach the backup */
        close(c1);
        close(l1);
        tu_sleep_ms(300);
        write_all(p.in, "two\n", 4);
        EXPECT_FRAMES(c2, "two\0");

        /* primary is back: sender reconnects and prefers it again */
        l1 = tu_listen(p1);
        CHECK(l1 >= 0);
        c1 = tu_accept(l1, 3000);
        CHECK(c1 >= 0);
        tu_sleep_ms(300);
        if (c1 >= 0) {
            write_all(p.in, "three\n", 6);
            EXPECT_FRAMES(c1, "three\0");
            close(c1);
        }
        if (l1 >= 0) {
            close(l1);
        }
    }

    CHECK(wait_exit(&p, 5000) == 0);
    if (c2 >= 0) {
        close(c2);
    }
    close(l2);
}

static void test_buffering_while_down(void)
{
    const char *args[12];
    char port[16], *input, *s;
    Proc p;
    Buf b = {NULL, 0, 0};
    int portnum, l, c, n, i, k, ok, seq[1000];
    int total = 1000;

    portnum = tu_free_port();
    sprintf(port, "%d", portnum);
    args[0] = "-i"; args[1] = "127.0.0.1"; args[2] = "-n"; args[3] = port;
    args[4] = "-b"; args[5] = "4096"; args[6] = "-s"; args[7] = "1024";
    args[8] = "-r"; args[9] = "1"; args[10] = NULL;
    CHECK(start_sender(&p, args) == 0);

    /* 1000 x 14 bytes stored does not fit into 4096: oldest get dropped */
    input = (char *)malloc((size_t)total * 9 + 1);
    for (i = 0; i < total; i++) {
        sprintf(input + i * 9, "msg-%04d\n", i);
    }
    write_all(p.in, input, (size_t)total * 9);
    free(input);
    tu_sleep_ms(300);
    close(p.in); /* EOF: sender keeps trying to flush for a while */
    p.in = -1;

    l = tu_listen(portnum);
    CHECK(l >= 0);
    c = tu_accept(l, 3000);
    CHECK(c >= 0);
    if (c >= 0) {
        recv_frames(c, &b, INT_MAX, 5000); /* until sender exits */
        k = 0;
        ok = 1;
        for (s = b.data; s != NULL && s < b.data + b.len && k < total; s += strlen(s) + 1) {
            if (sscanf(s, "msg-%d", &n) != 1) {
                ok = 0;
                break;
            }
            seq[k++] = n;
        }
        CHECK(ok);
        CHECK(k > 1 && k < total);
        if (ok && k > 1) {
            /* the in-flight message (possibly followed by a gap), then the
             * newest ones without gaps, all in order */
            CHECK(seq[1] > seq[0]);
            CHECK(seq[k - 1] == total - 1);
            for (i = 2; i < k; i++) {
                CHECK(seq[i] == seq[i - 1] + 1);
            }
            CHECK(k - 1 <= 4096 / 14);
        }
        free(b.data);
        close(c);
    }

    CHECK(wait_exit(&p, 5000) == 0);
    if (l >= 0) {
        close(l);
    }
}

static void test_bad_options(void)
{
    const char *too_big[] = {"-i", "127.0.0.1", "-n", "1", "-b", "4096", "-s", "4000", NULL};
    const char *bad_ip[] = {"-i", "graylog", "-n", "1", NULL};
    const char *no_port2[] = {"-i", "127.0.0.1", "-n", "1", "-j", "127.0.0.1", NULL};
    Proc p;

    CHECK(start_sender(&p, too_big) == 0);
    CHECK(wait_exit(&p, 3000) == 1);
    CHECK(start_sender(&p, bad_ip) == 0);
    CHECK(wait_exit(&p, 3000) == 1);
    CHECK(start_sender(&p, no_port2) == 0);
    CHECK(wait_exit(&p, 3000) == 1);
}

int main(int argc, char *argv[])
{
    if (argc > 1) {
        sender_bin = argv[1];
    }
    signal(SIGPIPE, SIG_IGN);

    test_bad_options();
    test_lines_and_long_message();
    test_oversized_line_dropped();
    test_failover_and_return();
    test_buffering_while_down();

    if (test_failures > 0) {
        fprintf(stderr, "%d integration test check(s) failed.\n", test_failures);
        return 1;
    }
    printf("All integration tests passed.\n");
    return 0;
}
