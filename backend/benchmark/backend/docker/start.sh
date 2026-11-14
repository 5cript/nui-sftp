#!/bin/sh
# vsftpd on 21 (plain and AUTH TLS, no rate limit) beside sshd. sshd starts once vsftpd listens, so a server that
# answers SSH answers FTP too.
vsftpd /etc/vsftpd/benchmark.conf &

listening() {
    grep -q ':0015 00000000:0000 0A' /proc/net/tcp
}
attempts=0
while [ "$attempts" -lt 200 ] && ! listening; do
    attempts=$((attempts + 1))
    sleep 0.05
done
if ! listening; then
    echo "vsftpd does not listen on 21" >&2
    exit 1
fi
exec /usr/sbin/sshd -D -e
