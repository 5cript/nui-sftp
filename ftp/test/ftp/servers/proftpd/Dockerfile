FROM debian:trixie-slim

RUN apt-get update \
 && apt-get install -y --no-install-recommends proftpd-core proftpd-mod-crypto iptables iproute2 openssl procps \
 && rm -rf /var/lib/apt/lists/* \
 && mkdir -p /var/log/proftpd /run/proftpd \
 && openssl req -x509 -nodes -newkey rsa:2048 -days 36500 -subj "/CN=nui-sftp-ftp-test-proftpd" \
        -keyout /etc/ssl/private/ftp-test.key -out /etc/ssl/certs/ftp-test.pem \
 && chmod 600 /etc/ssl/private/ftp-test.key

COPY common/fixtures.sh /usr/local/sbin/fixtures.sh
RUN sh /usr/local/sbin/fixtures.sh

COPY proftpd/proftpd.conf proftpd/default.conf proftpd/throttled.conf proftpd/limits.conf /etc/nui-sftp/
COPY proftpd/start.sh /usr/local/sbin/start.sh
RUN chmod 755 /usr/local/sbin/start.sh

ENTRYPOINT ["/usr/local/sbin/start.sh"]
CMD ["default"]
