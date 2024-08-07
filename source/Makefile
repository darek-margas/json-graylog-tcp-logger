#
# Copyright (c) 2024, Darek Margas  All rights reserved.
# Copyrights licensed under the GNU LESSER GENERAL PUBLIC LICENSE Version 2.1, February 1999.
# See the accompanying LICENSE file for terms.
# https://github.com/darek-margas/json-graylog-tcp-logger
#
CC = gcc
CFLAGS = -O4 -Wall -Wextra -std=c89 -Wmissing-prototypes -Wstrict-prototypes
TARGET = GELFsender
SOURCE = GELFsender.c

$(TARGET): $(SOURCE)
	$(CC) $(CFLAGS) -o $(TARGET) $(SOURCE)

clean:
	rm -f $(TARGET)
