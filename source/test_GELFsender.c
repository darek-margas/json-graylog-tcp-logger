/*
* test_GELFsender.c
*/

#include "GELFsender.c"

#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <sys/socket.h>
#include <arpa/inet.h>

/* Include your original code here, or in a separate header file */
extern int connect_to_server(const char* server_ip, int port_number, socket_func socket_fn, inet_pton_func inet_pton_fn, connect_func connect_fn);

/* Test function declarations */
void test_circular_buffer(void);
void test_connect_to_server(void);

/* Mock function declarations */
int mock_socket(int domain, int type, int protocol);
int mock_inet_pton(int af, const char *src, void *dst);
int mock_connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen);

int main(void) {
    test_circular_buffer();
    test_connect_to_server();
    printf("All tests passed!\n");
    return 0;
}

void test_circular_buffer(void) {
    CircularBuffer cb;
    char data[BUFFER_SIZE];
    int length;
    int i;

    /* Test initialization */
    init_circular_buffer(&cb);
    assert(cb.head == 0);
    assert(cb.tail == 0);
    assert(cb.count == 0);

    /* Test enqueue and dequeue */
    enqueue_message(&cb, "Test message 1", 15);
    assert(cb.count == 1);
    
    assert(dequeue_message(&cb, data, &length) == 1);
    assert(strcmp(data, "Test message 1") == 0);
    assert(length == 15);
    assert(cb.count == 0);

    /* Test buffer full behavior */
    for (i = 0; i < CIRCULAR_BUFFER_SIZE + 1; i++) {
        char msg[20];
        sprintf(msg, "Message %d", i);
        enqueue_message(&cb, msg, (int)strlen(msg) + 1);
    }
    assert(cb.count == CIRCULAR_BUFFER_SIZE);

    /* Verify that the oldest message was overwritten */
    dequeue_message(&cb, data, &length);
    assert(strcmp(data, "Message 1") == 0);

    printf("Circular buffer tests passed.\n");
}

int mock_socket(int domain, int type, int protocol) {
    (void)domain; /* Cast to void to suppress unused parameter warning */
    (void)type;
    (void)protocol;
    return 3;  /* Return a typical file descriptor value */
}

int mock_inet_pton(int af, const char *src, void *dst) {
    (void)af;
    (void)src;
    (void)dst;
    return 1;  /* Indicate success */
}

int mock_connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    (void)sockfd;
    (void)addr;
    (void)addrlen;
    return 0;  /* Indicate success */
}

void test_connect_to_server(void) {
    int result;
    extern int connect_to_server(const char* server_ip, int port_number,
                                 socket_func socket_fn,
                                 inet_pton_func inet_pton_fn,
                                 connect_func connect_fn);

    result = connect_to_server("192.168.1.1", 12345, mock_socket, mock_inet_pton, mock_connect);
    
    assert(result > 0);
    printf("Connect to server tests passed.\n");
}
