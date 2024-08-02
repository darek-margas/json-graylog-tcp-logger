# json-graylog-tcp-logger
This is Graylog TCP logger written in C

This process takes STDIN as input, assuming one message per line, terminated by 0x0a. Then, replaces termination with 0x00 to comply with Graylog TCP format and sends it to one of (up to) two Graylog servers.

Compile with: gcc -O3 -Wall -Wextra -Wmissing-prototypes -Wstrict-prototypes -Wold-style-definition -o GELFsender GELFsender.c
