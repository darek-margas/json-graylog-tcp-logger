/*
* GELFsender.c
*
* Copyright (c) 2024, Darek Margas  All rights reserved.
* Copyrights licensed under the GNU LESSER GENERAL PUBLIC LICENSE Version 2.1, February 1999.
* See the accompanying LICENSE file for terms.
* https://github.com/darek-margas/json-graylog-tcp-logger
*
* Changelog:
* - single poll() loop instead of SIGIO/pause(); stdin is read whenever data
*   is available and never waits on the network
* - input split into lines regardless of how read() chunks the pipe
* - byte-limited ring buffer (-b) with per-message limit (-s); when full,
*   oldest messages are dropped until it is at most 3/4 full
* - non-blocking connect with timeout, partial sends resumed, message order
*   kept across failover
* - added unit test
*
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <syslog.h>
#include <signal.h>
#include <libgen.h>

/* extensions from gcc */
#include <getopt.h>

#include "ringbuf.h"
#include "framer.h"
#include "net.h"

#define DEFAULT_BUFFER_SIZE (4UL * 1024 * 1024) /* -b */
#define DEFAULT_MAX_MESSAGE (1UL * 1024 * 1024) /* -s */
#define DEFAULT_RETRY_DELAY 5   /* seconds, -r; also the connect timeout */
#define MAX_SIZE_OPTION (1024UL * 1024 * 1024)
#define DRAIN_TIMEOUT 10        /* seconds to keep sending after stdin EOF */
#define STATS_INTERVAL 60       /* seconds between drop reports */
#define READ_CHUNK 65536

#ifdef MSG_NOSIGNAL
#define SEND_FLAGS MSG_NOSIGNAL
#else
#define SEND_FLAGS 0
#endif

enum { S_DOWN, S_CONNECTING, S_UP };

typedef struct {
    const char *name;
    const char *ip;
    int port;
    int enabled;
    struct sockaddr_in addr;
    int fd;
    int state;
    double deadline;      /* S_DOWN: next attempt; S_CONNECTING: give up */
    double last_fail_log; /* rate limit for repeated failure messages */
} Server;

typedef struct {
    Server srv[2];          /* primary, backup */
    RingBuffer rb;
    Framer fr;
    char *inflight;         /* message currently being sent */
    size_t inflight_len;    /* 0 when nothing is in flight */
    size_t inflight_off;    /* bytes of it already sent */
    Server *target;         /* where the in-flight message goes */
    size_t max_msg;
    double retry_delay;
    int verbose;
    unsigned long rep_dropped_msgs, rep_dropped_bytes, rep_oversized;
} Sender;

static void print_usage(const char* program_name) {
    fprintf(stderr, "Usage: %s -i <ip1> -n <port_number1> [-j <ip2> -m <port_number2>] [-b <size>] [-s <size>] [-r <seconds>] [-l]\n", program_name);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -i <ip1>           Primary server IP address\n");
    fprintf(stderr, "  -n <port_number1>  Primary server port number\n");
    fprintf(stderr, "  -j <ip2>           Backup server IP address (optional)\n");
    fprintf(stderr, "  -m <port_number2>  Backup server port number (optional)\n");
    fprintf(stderr, "  -b <size>          Buffer size in bytes, K or M suffix allowed (default 4M)\n");
    fprintf(stderr, "  -s <size>          Maximum message size, longer lines are dropped (default 1M,\n");
    fprintf(stderr, "                     at most 3/4 of the buffer size)\n");
    fprintf(stderr, "  -r <seconds>       Reconnect delay and connect timeout (default %d)\n", DEFAULT_RETRY_DELAY);
    fprintf(stderr, "  -l                 Enable logging of processed requests\n");
}

static double now_sec(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int parse_size(const char *s, size_t *out)
{
    char *end;
    unsigned long v, mult = 1;

    errno = 0;
    v = strtoul(s, &end, 10);
    if (errno != 0 || end == s) {
        return -1;
    }
    if (*end == 'k' || *end == 'K') {
        mult = 1024UL;
        end++;
    } else if (*end == 'm' || *end == 'M') {
        mult = 1024UL * 1024;
        end++;
    }
    if (*end != '\0' || v > MAX_SIZE_OPTION / mult) {
        return -1;
    }
    *out = (size_t)(v * mult);
    return 0;
}

static int parse_int(const char *s, int min, int max, int *out)
{
    char *end;
    long v;

    errno = 0;
    v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < min || v > max) {
        return -1;
    }
    *out = (int)v;
    return 0;
}

static void server_failed(Sender *st, Server *s, double now, const char *why)
{
    int was_up = s->state == S_UP;

    if (s->fd >= 0) {
        close(s->fd);
    }
    s->fd = -1;
    s->state = S_DOWN;
    s->deadline = now + st->retry_delay;

    if (was_up) {
        syslog(LOG_ERR, "Lost connection to %s server %s:%d: %s",
               s->name, s->ip, s->port, why);
        s->last_fail_log = now;
    } else if (now - s->last_fail_log >= STATS_INTERVAL) {
        syslog(LOG_ERR, "Failed to connect to %s server %s:%d: %s. Retrying every %d seconds.",
               s->name, s->ip, s->port, why, (int)st->retry_delay);
        s->last_fail_log = now;
    }

    if (st->target == s) {
        /* Resend the whole message elsewhere; the partial one died with the connection */
        st->target = NULL;
        st->inflight_off = 0;
    }
}

static void server_up(Server *s, int fd)
{
    s->fd = fd;
    s->state = S_UP;
    s->last_fail_log = -STATS_INTERVAL; /* log the next failure right away */
    syslog(LOG_INFO, "Connected to %s server %s:%d", s->name, s->ip, s->port);
}

static void server_tick(Sender *st, Server *s, double now)
{
    int fd, r;

    if (!s->enabled) {
        return;
    }
    if (s->state == S_DOWN && now >= s->deadline) {
        r = net_connect_start(&s->addr, &fd);
        if (r == 0) {
            server_up(s, fd);
        } else if (r == 1) {
            s->fd = fd;
            s->state = S_CONNECTING;
            s->deadline = now + st->retry_delay;
        } else {
            server_failed(st, s, now, strerror(errno));
        }
    } else if (s->state == S_CONNECTING && now >= s->deadline) {
        server_failed(st, s, now, "connect timed out");
    }
}

static Server *choose_target(Sender *st)
{
    if (st->srv[0].state == S_UP) {
        return &st->srv[0];
    }
    if (st->srv[1].state == S_UP) {
        return &st->srv[1];
    }
    return NULL;
}

static void next_message(Sender *st)
{
    st->inflight_off = 0;
    st->inflight_len = rb_pop(&st->rb, st->inflight, st->max_msg);
    st->target = st->inflight_len > 0 ? choose_target(st) : NULL;
}

/* Sends as much as the socket takes without blocking. */
static void pump(Sender *st, Server *s, double now)
{
    ssize_t n;

    while (st->target == s && st->inflight_len > 0) {
        n = send(s->fd, st->inflight + st->inflight_off,
                 st->inflight_len - st->inflight_off, SEND_FLAGS);
        if (n < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                server_failed(st, s, now, strerror(errno));
            }
            return;
        }
        st->inflight_off += (size_t)n;
        if (st->inflight_off == st->inflight_len) {
            if (st->verbose) {
                syslog(LOG_INFO, "Sent to %s server: %s", s->name, st->inflight);
            }
            next_message(st);
        }
    }
}

static void handle_server(Sender *st, Server *s, short revents, double now)
{
    char junk[512];
    ssize_t n;
    int err;

    if (s->state == S_CONNECTING) {
        err = net_connect_result(s->fd);
        if (err == 0) {
            server_up(s, s->fd);
        } else {
            server_failed(st, s, now, strerror(err));
        }
        return;
    }

    if (revents & (POLLIN | POLLERR | POLLHUP)) {
        /* Graylog never talks back, so this is a close or an error */
        n = recv(s->fd, junk, sizeof(junk), 0);
        if (n == 0) {
            server_failed(st, s, now, "closed by peer");
            return;
        }
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            server_failed(st, s, now, strerror(errno));
            return;
        }
    }

    if (revents & POLLOUT) {
        pump(st, s, now);
    }
}

static void report_drops(Sender *st)
{
    unsigned long msgs = st->rb.dropped_msgs - st->rep_dropped_msgs;
    unsigned long bytes = st->rb.dropped_bytes - st->rep_dropped_bytes;
    unsigned long oversized = st->fr.oversized - st->rep_oversized;

    if (msgs > 0) {
        syslog(LOG_WARNING, "Buffer full: dropped %lu oldest messages (%lu bytes)", msgs, bytes);
    }
    if (oversized > 0) {
        syslog(LOG_WARNING, "Dropped %lu lines longer than %lu bytes",
               oversized, (unsigned long)(st->max_msg - 1));
    }
    st->rep_dropped_msgs = st->rb.dropped_msgs;
    st->rep_dropped_bytes = st->rb.dropped_bytes;
    st->rep_oversized = st->fr.oversized;
}

static int run(Sender *st)
{
    static char readbuf[READ_CHUNK];
    struct pollfd pfd[3];
    int sidx[2];
    int nfds, stdin_idx, i, timeout_ms;
    int stdin_eof = 0;
    double now, wake, drain_deadline = 0, next_report;
    ssize_t n;
    Server *s;

    next_report = now_sec() + STATS_INTERVAL;
    syslog(LOG_INFO, "Waiting for input on stdin");

    for (;;) {
        now = now_sec();
        server_tick(st, &st->srv[0], now);
        server_tick(st, &st->srv[1], now);

        if (st->inflight_len == 0) {
            next_message(st);
        } else if (st->inflight_off == 0) {
            st->target = choose_target(st); /* prefer primary at message boundaries */
        }

        if (stdin_eof) {
            if (st->inflight_len == 0) {
                return 0;
            }
            if (now >= drain_deadline) {
                syslog(LOG_WARNING, "Exiting with %lu undelivered messages",
                       (unsigned long)st->rb.count + 1);
                return 0;
            }
        }

        if (now >= next_report) {
            report_drops(st);
            next_report = now + STATS_INTERVAL;
        }

        /* Build the poll set */
        nfds = 0;
        stdin_idx = -1;
        if (!stdin_eof) {
            pfd[nfds].fd = STDIN_FILENO;
            pfd[nfds].events = POLLIN;
            stdin_idx = nfds++;
        }
        wake = next_report;
        if (stdin_eof && drain_deadline < wake) {
            wake = drain_deadline;
        }
        for (i = 0; i < 2; i++) {
            s = &st->srv[i];
            sidx[i] = -1;
            if (!s->enabled) {
                continue;
            }
            if (s->state == S_DOWN || s->state == S_CONNECTING) {
                if (s->deadline < wake) {
                    wake = s->deadline;
                }
            }
            if (s->state == S_DOWN) {
                continue;
            }
            pfd[nfds].fd = s->fd;
            pfd[nfds].events = s->state == S_CONNECTING ? POLLOUT : POLLIN;
            if (s->state == S_UP && st->target == s) {
                pfd[nfds].events |= POLLOUT;
            }
            sidx[i] = nfds++;
        }
        timeout_ms = wake > now ? (int)((wake - now) * 1000.0) + 1 : 0;

        if (poll(pfd, (nfds_t)nfds, timeout_ms) < 0) {
            if (errno == EINTR) {
                continue;
            }
            syslog(LOG_ERR, "poll failed: %m");
            return 1;
        }
        now = now_sec();

        if (stdin_idx >= 0 && pfd[stdin_idx].revents) {
            n = read(STDIN_FILENO, readbuf, sizeof(readbuf));
            if (n > 0) {
                framer_feed(&st->fr, &st->rb, readbuf, (size_t)n);
            } else if (n == 0 || (errno != EINTR && errno != EAGAIN)) {
                if (n < 0) {
                    syslog(LOG_ERR, "Error reading from stdin: %m");
                }
                framer_flush(&st->fr, &st->rb);
                stdin_eof = 1;
                drain_deadline = now + DRAIN_TIMEOUT;
                syslog(LOG_INFO, "End of input, flushing %lu buffered messages",
                       (unsigned long)st->rb.count + (st->inflight_len > 0));
            }
        }

        for (i = 0; i < 2; i++) {
            s = &st->srv[i];
            if (sidx[i] >= 0 && pfd[sidx[i]].revents && s->fd == pfd[sidx[i]].fd) {
                handle_server(st, s, pfd[sidx[i]].revents, now);
            }
        }
    }
}

int main(int argc, char *argv[]) {
    Sender st;
    const char *prog;
    const char *server_ip1 = NULL, *server_ip2 = NULL;
    int port_number1 = 0, port_number2 = 0;
    int retry = DEFAULT_RETRY_DELAY;
    size_t buffer_size = DEFAULT_BUFFER_SIZE;
    size_t max_message = DEFAULT_MAX_MESSAGE;
    int opt, i, rc;

    memset(&st, 0, sizeof(st));
    prog = basename(argv[0]);
    openlog(prog, LOG_PID | LOG_CONS, LOG_USER);

#ifdef SIGPIPE
    /*
     * A write to a TCP socket closed by the peer may raise SIGPIPE before
     * send() can return EPIPE.  Ignore it so the normal reconnect/failover
     * path handles the failed send instead of terminating the process.
     *
     * SIGPIPE is not available on all platforms (notably Windows), so keep
     * this conditional for portability.
     */
    signal(SIGPIPE, SIG_IGN);
#endif

    while ((opt = getopt(argc, argv, "i:n:j:m:b:s:r:l")) != -1) {
        switch (opt) {
            case 'i': server_ip1 = optarg; break;
            case 'n': if (parse_int(optarg, 1, 65535, &port_number1)) goto usage; break;
            case 'j': server_ip2 = optarg; break;
            case 'm': if (parse_int(optarg, 1, 65535, &port_number2)) goto usage; break;
            case 'b': if (parse_size(optarg, &buffer_size)) goto usage; break;
            case 's': if (parse_size(optarg, &max_message)) goto usage; break;
            case 'r': if (parse_int(optarg, 1, 3600, &retry)) goto usage; break;
            case 'l': st.verbose = 1; break;
            default: goto usage;
        }
    }

    if (server_ip1 == NULL || port_number1 == 0) {
        goto usage;
    }
    if ((server_ip2 && !port_number2) || (!server_ip2 && port_number2)) {
        fprintf(stderr, "Error: Both IP and port must be specified for the second server.\n");
        goto usage;
    }
    if (buffer_size < RB_MIN_SIZE) {
        fprintf(stderr, "Error: Buffer size must be at least %d bytes.\n", RB_MIN_SIZE);
        goto usage;
    }
    if (max_message < 2 || max_message > rb_max_entry(buffer_size)) {
        fprintf(stderr, "Error: Maximum message size must be between 2 and %lu bytes (3/4 of buffer size).\n",
                (unsigned long)rb_max_entry(buffer_size));
        goto usage;
    }

    st.srv[0].name = "primary";
    st.srv[0].ip = server_ip1;
    st.srv[0].port = port_number1;
    st.srv[0].enabled = 1;
    st.srv[1].name = "backup";
    st.srv[1].ip = server_ip2;
    st.srv[1].port = port_number2;
    st.srv[1].enabled = server_ip2 != NULL;
    for (i = 0; i < 2; i++) {
        st.srv[i].fd = -1;
        st.srv[i].state = S_DOWN;
        st.srv[i].last_fail_log = -STATS_INTERVAL;
        if (st.srv[i].enabled &&
            net_parse_addr(st.srv[i].ip, st.srv[i].port, &st.srv[i].addr) != 0) {
            fprintf(stderr, "Error: Invalid IPv4 address: %s\n", st.srv[i].ip);
            goto usage;
        }
    }

    st.max_msg = max_message;
    st.retry_delay = retry;
    st.inflight = (char *)malloc(max_message);
    if (st.inflight == NULL || rb_init(&st.rb, buffer_size) != 0 ||
        framer_init(&st.fr, max_message) != 0) {
        fprintf(stderr, "Error: Cannot allocate %lu bytes of buffer.\n", (unsigned long)buffer_size);
        syslog(LOG_ERR, "Cannot allocate buffers");
        return EXIT_FAILURE;
    }
    st.fr.verbose = st.verbose;

    rc = run(&st);

    report_drops(&st);
    for (i = 0; i < 2; i++) {
        if (st.srv[i].fd >= 0) {
            close(st.srv[i].fd);
        }
    }
    framer_free(&st.fr);
    rb_free(&st.rb);
    free(st.inflight);
    closelog();
    return rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE;

usage:
    print_usage(prog);
    return EXIT_FAILURE;
}
