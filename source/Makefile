#
# Makefile
#
# Copyright (c) 2024, Darek Margas  All rights reserved.
# Copyrights licensed under the GNU LESSER GENERAL PUBLIC LICENSE Version 2.1, February 1999.
# See the accompanying LICENSE file for terms.
# https://github.com/darek-margas/json-graylog-tcp-logger
#
# Changelog:
# - added unit test 
# - migrated compatibility definitions into gcc options
#
CC = gcc
CFLAGS = -O4 -Wall -Wextra -std=c89 -Wmissing-prototypes -Wstrict-prototypes -D_XOPEN_SOURCE=700 -D_POSIX_C_SOURCE=200809L
TARGET = GELFsender
SOURCE = GELFsender.c
TEST_TARGET = test_GELFsender
TEST_SOURCE = test_GELFsender.c 

.PHONY: all clean test

all: $(TARGET) $(TEST_TARGET)

$(TARGET): $(SOURCE)
	$(CC) $(CFLAGS) -o $(TARGET) $(SOURCE)

$(TEST_TARGET): $(TEST_SOURCE) $(SOURCE)
	$(CC) $(CFLAGS) -o $(TEST_TARGET) $(TEST_SOURCE) -DUNIT_TEST

test: $(TEST_TARGET)
	./$(TEST_TARGET)

clean:
	rm -f $(TARGET) $(TEST_TARGET)

