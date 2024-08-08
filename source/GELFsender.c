/*
* GELFsender.c
*
* Copyright (c) 2024, Darek Margas  All rights reserved.
* Copyrights licensed under the GNU LESSER GENERAL PUBLIC LICENSE Version 2.1, February 1999.
* See the accompanying LICENSE file for terms.
* https://github.com/darek-margas/json-graylog-tcp-logger
*
* Changelog:
* - added unit test 
*
*/

/*
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <syslog.h>
#include <signal.h>
#include <stdbool.h>
#include <libgen.h>

/* extensions from gcc */
#include <getopt.h>

/* might be better to have it in Makefile or as an option */
#define BUFFER_SIZE 8192
#define MAX_RETRIES 25
#define RETRY_DELAY 5 /* seconds */
#define CIRCULAR_BUFFER_SIZE 1000 /*  Maximum number of messages to buffer */

typedef struct {
    char data[BUFFER_SIZE];
    int length;
} Message;

typedef struct {
    Message messages[CIRCULAR_BUFFER_SIZE];
    int head;
    int tail;
    int count;
} CircularBuffer;

/* Room for unit test (no other way to replace system calls in sight) */
typedef int (*socket_func)(int domain, int type, int protocol);
typedef int (*inet_pton_func)(int af, const char *src, void *dst);
typedef int (*connect_func)(int sockfd, const struct sockaddr *addr, socklen_t addrlen);

volatile sig_atomic_t data_ready = 0;
bool logging_enabled = false;

/* Function declarations - c89 compatibility */
void print_usage(const char* program_name);
int connect_to_server(const char* server_ip, int port_number, socket_func socket_fn, inet_pton_func inet_pton_fn, connect_func connect_fn);
void init_circular_buffer(CircularBuffer* cb);
void enqueue_message(CircularBuffer* cb, const char* data, int length);
int dequeue_message(CircularBuffer* cb, char* data, int* length);
void sigio_handler(int signo);

/* Function definitions */
void print_usage(const char* program_name) {
    fprintf(stderr, "Usage: %s -i <ip1> -n <port_number1> [-j <ip2> -m <port_number2>] [-l]\n", program_name);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -i <ip1>           Primary server IP address\n");
    fprintf(stderr, "  -n <port_number1>  Primary server port number\n");
    fprintf(stderr, "  -j <ip2>           Backup server IP address (optional)\n");
    fprintf(stderr, "  -m <port_number2>  Backup server port number (optional)\n");
    fprintf(stderr, "  -l                 Enable logging of processed requests\n");
}

int connect_to_server(const char* server_ip, int port_number,
                      socket_func socket_fn,
                      inet_pton_func inet_pton_fn,
                      connect_func connect_fn) {
    int sock_fd;
    struct sockaddr_in server_addr;

    sock_fd = socket_fn(AF_INET, SOCK_STREAM, 0);
    if (sock_fd == -1) {
        syslog(LOG_ERR, "Failed to create socket: %m");
        return -1;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port_number);
    if (inet_pton_fn(AF_INET, server_ip, &server_addr.sin_addr) <= 0) {
        syslog(LOG_ERR, "Invalid address: %m");
        close(sock_fd);
        return -1;
    }

    if (connect_fn(sock_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        syslog(LOG_ERR, "Connection failed: %m");
        close(sock_fd);
        return -1;
    }

    return sock_fd;
}

void init_circular_buffer(CircularBuffer* cb) {
    cb->head = 0;
    cb->tail = 0;
    cb->count = 0;
}

void enqueue_message(CircularBuffer* cb, const char* data, int length) {
    if (cb->count == CIRCULAR_BUFFER_SIZE) {
        /* Buffer is full, discard oldest message */
        cb->tail = (cb->tail + 1) % CIRCULAR_BUFFER_SIZE;
        cb->count--;
    }

    strncpy(cb->messages[cb->head].data, data, length);
    cb->messages[cb->head].length = length;
    cb->head = (cb->head + 1) % CIRCULAR_BUFFER_SIZE;
    cb->count++;
}

int dequeue_message(CircularBuffer* cb, char* data, int* length) {
    if (cb->count == 0) {
        return 0; /* Buffer is empty */
    }

    strncpy(data, cb->messages[cb->tail].data, cb->messages[cb->tail].length);
    *length = cb->messages[cb->tail].length;
    cb->tail = (cb->tail + 1) % CIRCULAR_BUFFER_SIZE;
    cb->count--;
    return 1;
}

void sigio_handler(int signo)
{
    (void)signo;  /* Cast to void to suppress unused parameter warning - c89 compatibility */
    data_ready = 1;
}

#ifndef UNIT_TEST
int main(int argc, char *argv[]) {
    int sock_fd1 = -1, sock_fd2 = -1;
    char buffer[BUFFER_SIZE];
    char *server_ip1 = NULL, *server_ip2 = NULL;
    int port_number1 = 0, port_number2 = 0;
    int opt;
    CircularBuffer cb;

    openlog(basename(argv[0]), LOG_PID | LOG_CONS, LOG_USER);

    while ((opt = getopt(argc, argv, "i:n:j:m:l")) != -1) {
        switch (opt) {
            case 'i': server_ip1 = optarg; break;
            case 'n': port_number1 = atoi(optarg); break;
            case 'j': server_ip2 = optarg; break;
            case 'm': port_number2 = atoi(optarg); break;
            case 'l': logging_enabled = true; break;
            default: print_usage(basename(argv[0])); exit(EXIT_FAILURE);
        }
    }

    if (server_ip1 == NULL || port_number1 == 0) {
        print_usage(basename(argv[0]));
        exit(EXIT_FAILURE);
    }

    if ((server_ip2 && !port_number2) || (!server_ip2 && port_number2)) {
        fprintf(stderr, "Error: Both IP and port must be specified for the second server.\n");
        print_usage(argv[0]);
        exit(EXIT_FAILURE);
    }

    init_circular_buffer(&cb);

    /* Set up signal handling */
    struct sigaction sa;
    sa.sa_handler = sigio_handler;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGIO, &sa, NULL) == -1) {
        syslog(LOG_ERR, "Failed to set up signal handler: %m");
        exit(EXIT_FAILURE);
    }

    /* Set up asynchronous I/O */
    if (fcntl(STDIN_FILENO, F_SETOWN, getpid()) == -1) {
        syslog(LOG_ERR, "Failed to set process owner: %m");
        exit(EXIT_FAILURE);
    }
    int flags = fcntl(STDIN_FILENO, F_GETFL);
    if (fcntl(STDIN_FILENO, F_SETFL, flags | O_ASYNC) == -1) {
        syslog(LOG_ERR, "Failed to set O_ASYNC flag: %m");
        exit(EXIT_FAILURE);
    }

    syslog(LOG_INFO, "Waiting for input on stdin");

    while (1) {
        /* Wait for signal */
        while (!data_ready) {
            pause();
        }
        data_ready = 0;

        ssize_t bytes_read = read(STDIN_FILENO, buffer, BUFFER_SIZE - 1);
        if (bytes_read > 0) {
            buffer[bytes_read - 1] = '\0';  /* Null-terminate the string by replacing last character 0x0a by 0x00*/
            enqueue_message(&cb, buffer, bytes_read);
            if (logging_enabled) {
                syslog(LOG_INFO, "Read and buffered: %s", buffer);
            }
        } else if (bytes_read == 0) {
            syslog(LOG_INFO, "End of input. Exiting...");
            break;
        } else {
            syslog(LOG_ERR, "Error reading from stdin: %m");
        }

        /* Try to send buffered data */
        while (cb.count > 0) {
            char msg[BUFFER_SIZE];
            int msg_length;
            if (dequeue_message(&cb, msg, &msg_length)) {
                if (sock_fd1 != -1) {
                    if (send(sock_fd1, msg, msg_length, 0) < 0) {
                        syslog(LOG_ERR, "Failed to send data to primary server: %m");
                        close(sock_fd1);
                        sock_fd1 = -1;
                    } else {
                        if (logging_enabled) {
                            syslog(LOG_INFO, "Sent to primary server: %s", msg);
                        }
                        continue;  /* Message sent successfully, move to next message */
                    }
                }

                if (sock_fd2 != -1) {
                    if (send(sock_fd2, msg, msg_length, 0) < 0) {
                        syslog(LOG_ERR, "Failed to send data to backup server: %m");
                        close(sock_fd2);
                        sock_fd2 = -1;
                    } else {
                        if (logging_enabled) {
                            syslog(LOG_INFO, "Sent to backup server: %s", msg);
                        }
                        continue;  /* Message sent successfully, move to next message */
                    }
                }

                /* If we reach here, both connections failed, re-enqueue the message */
                enqueue_message(&cb, msg, msg_length);
                break;  /* Exit the sending loop to attempt reconnection */
            }
        }

        /* Attempt to reconnect if necessary */
        if (sock_fd1 == -1) {
	    sock_fd1 = connect_to_server(server_ip1, port_number1, socket, inet_pton, connect);
            if (sock_fd1 == -1) {
                syslog(LOG_ERR, "Failed to reconnect to primary server. Retrying in %d seconds.", RETRY_DELAY);
            } else {
                syslog(LOG_INFO, "Reconnected to primary server.");
            }
        }

        if (server_ip2 && port_number2 && sock_fd2 == -1) {
	    sock_fd2 = connect_to_server(server_ip2, port_number2, socket, inet_pton, connect);
            if (sock_fd2 == -1) {
                syslog(LOG_ERR, "Failed to reconnect to backup server. Retrying in %d seconds.", RETRY_DELAY);
            } else {
                syslog(LOG_INFO, "Reconnected to backup server.");
            }
        }

        if (sock_fd1 == -1 && sock_fd2 == -1) {
            sleep(RETRY_DELAY);
        }
    }

    if (sock_fd1 != -1) close(sock_fd1);
    if (sock_fd2 != -1) close(sock_fd2);
    closelog();
    return 0;
}
#endif
