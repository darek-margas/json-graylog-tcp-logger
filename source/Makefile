#
# Makefile
#
# Copyright (c) 2024, Darek Margas  All rights reserved.
# Copyrights licensed under the GNU LESSER GENERAL PUBLIC LICENSE Version 2.1, February 1999.
# See the accompanying LICENSE file for terms.
# https://github.com/darek-margas/json-graylog-tcp-logger
#
# Changelog:
# - split into modules; tests link the same objects as GELFsender, no
#   separate -DUNIT_TEST build
# - integration test runs the real binary against loopback listeners
# - added unit test
# - migrated compatibility definitions into gcc options
#
CC = gcc
CFLAGS = -O4 -Wall -Wextra -std=c89 -Wmissing-prototypes -Wstrict-prototypes -D_XOPEN_SOURCE=700 -D_POSIX_C_SOURCE=200809L
TARGET = GELFsender
OBJS = ringbuf.o framer.o net.o
UNIT_TEST = test_GELFsender
INTEGRATION_TEST = test_integration

.PHONY: all clean test install

all: $(TARGET)

$(TARGET): GELFsender.o $(OBJS)
	$(CC) $(CFLAGS) -o $@ GELFsender.o $(OBJS)

$(UNIT_TEST): test_GELFsender.o test_util.o $(OBJS)
	$(CC) $(CFLAGS) -o $@ test_GELFsender.o test_util.o $(OBJS)

$(INTEGRATION_TEST): test_integration.o test_util.o
	$(CC) $(CFLAGS) -o $@ test_integration.o test_util.o

GELFsender.o: GELFsender.c ringbuf.h framer.h net.h
ringbuf.o: ringbuf.c ringbuf.h
framer.o: framer.c framer.h ringbuf.h
net.o: net.c net.h
test_util.o: test_util.c test_util.h
test_GELFsender.o: test_GELFsender.c ringbuf.h framer.h net.h test_util.h
test_integration.o: test_integration.c test_util.h

test: $(TARGET) $(UNIT_TEST) $(INTEGRATION_TEST)
	./$(UNIT_TEST)
	./$(INTEGRATION_TEST) ./$(TARGET)

install: $(TARGET)
	install -D -m 0755 $(TARGET) $(DESTDIR)/usr/bin/$(TARGET)

clean:
	rm -f $(TARGET) $(UNIT_TEST) $(INTEGRATION_TEST) *.o
