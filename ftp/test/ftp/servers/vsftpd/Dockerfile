FROM debian:trixie-slim

RUN apt-get update \
 && apt-get install -y --no-install-recommends vsftpd iptables iproute2 openssl \
 && rm -rf /var/lib/apt/lists/* \
 && mkdir -p /var/run/vsftpd/empty \
 && openssl req -x509 -nodes -newkey rsa:2048 -days 36500 -subj "/CN=nui-sftp-ftp-test-vsftpd" \
        -keyout /etc/ssl/private/ftp-test.key -out /etc/ssl/certs/ftp-test.pem \
 && chmod 600 /etc/ssl/private/ftp-test.key

COPY common/fixtures.sh /usr/local/sbin/fixtures.sh
RUN sh /usr/local/sbin/fixtures.sh

COPY vsftpd/base.conf vsftpd/default.conf vsftpd/throttled.conf vsftpd/limits.conf /etc/nui-sftp/
COPY vsftpd/start.sh /usr/local/sbin/start.sh
RUN chmod 755 /usr/local/sbin/start.sh

ENTRYPOINT ["/usr/local/sbin/start.sh"]
CMD ["default"]
