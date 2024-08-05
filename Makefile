CC = gcc
CFLAGS = -O4 -Wall -Wextra -std=c89 -Wmissing-prototypes -Wstrict-prototypes
TARGET = GELFsender
SOURCE = GELFsender-8.c

$(TARGET): $(SOURCE)
        $(CC) $(CFLAGS) -o $(TARGET) $(SOURCE)

clean:
        rm -f $(TARGET)
