# json-graylog-tcp-logger
This is Graylog TCP logger written in C

This process takes STDIN as input, assuming one message per line, terminated by 0x0a. Then, replaces termination with 0x00 to comply with Graylog TCP format and sends it to one of (up to) two Graylog servers.

Compile with make

# So, what does it really do and why is it needed?
Apache has important feature - according to the Apache Team, it never lose any log content. In turn, it does not serve when is unable to log. Yes, it does not.
Transferring logs from Apache to Graylog brings some challenges.
- https has to be able to write log content no matter what, hence log receiver must be robust
- Options are: named-pipe, pipe to spawned process or a file
- Normal file needs to be rotated before space fills up and httpd stops serving, it is also a bit challenging for another process to monitor inode.
- named-pipe will reject writes when buffer overfills and nothing is reading it, causing httpd to stop serving, hence reading process has to be monitored and restarted. It is also more difficult to read, compared to standard file
- Pipe to process spawned by httpd seems best, you probably noticed nc used in this role, however, when process dies, httpd will respawn it until it works and will stop serving in the menatime. Say, if nc is unable to connect to its target, whole setup will loop in respawing
- While it is possible to use UDP or TCP on Graylog listener, both have limitations. Using TCP requires message to be terminated by 0x00 and that is impossible to do by httpd, using custom log definition (or at least I haven't found such option), using UDP limits message size to MTU minus header size, truncating anything longer.

- I used nc and nxlog and wasn't happy with neither.

# How is it solved here?
- This is logging into STDIN hence httpd will maintain the process
- Taking input and sending output is independent: a single poll() loop reads stdin whenever data is available and never waits on the network (connects and sends are non-blocking)
- Input is always accepted and buffered
- Input is split into lines no matter how the pipe delivers it (several lines per read, or one long line over many reads). The 0x0a termination is replaced by 0x00 to comply with Graylog spec
- Output is being send if possible, with active/passive failover if two Graylogs are available. Partial sends are resumed and message order is kept across failover
- If output is impossible to send, buffer accumulates messages up to its size in bytes. When full, oldest messages are discarded until the buffer is at most 3/4 full. In that case some logs will be missing but httpd keeps serving. Drops are reported to syslog at most once a minute
- Lines longer than the maximum message size are discarded and reported
- On end of input, buffered messages are flushed for up to 10 seconds before exiting

# Options
```
GELFsender -i <ip1> -n <port1> [-j <ip2> -m <port2>] [-b <size>] [-s <size>] [-r <seconds>] [-l]
```
- `-i`, `-n` primary server IPv4 address and port
- `-j`, `-m` backup server IPv4 address and port (optional)
- `-b` buffer size in bytes, K or M suffix allowed (default 4M)
- `-s` maximum message size (default 1M), at most 3/4 of the buffer size
- `-r` reconnect delay and connect timeout in seconds (default 5)
- `-l` log every processed message to syslog

# Tests
`make test` runs unit tests (ring buffer, line splitting, connect helpers on loopback) and an integration test that starts the real `GELFsender` binary with a pipe as stdin and loopback listeners as Graylog servers, covering line splitting, long and oversized lines, failover and return to primary, and buffering with drops while servers are down.

# Why is it in C?
- As httpd depends on it, the process must be stable with minimum dependency. Having interpreter involved increases dependency and creates possible points of failure, when interpreter gets modified or damaged (think an update).

# Known problem
- Due to buffering, the time message arrives in Graylog isn't exactly when https sends it. It might be worth to take timestamp from http via custom logging directive. 

I believe it would be better design if it spawns two forks, one to handle input, one to handle output, with shared memory buffer and output fork more focused on reconnecting and rolling buffer. It is just not someting I can write in spare time. It might be beneficial to use resolver and hostnames too. I would be happy to take PRs for improvements if anyone is keen to extend this work.
