# OpenSSH with its sftp-server and vsftpd for the transfer benchmarks. Pinned so runs stay comparable.
FROM alpine:3.24
RUN apk add --no-cache openssh-server openssh-sftp-server vsftpd openssl \
    && ssh-keygen -A \
    && adduser -D -s /bin/sh bench \
    && echo 'bench:bench' | chpasswd \
    && mkdir -p /home/bench/.ssh \
    && chmod 700 /home/bench/.ssh \
    && chown bench:bench /home/bench/.ssh \
    && mkdir -p /var/run/vsftpd/empty /etc/ssl/private \
    && openssl req -x509 -nodes -newkey rsa:2048 -days 36500 -subj "/CN=nui-sftp-benchmark" \
        -keyout /etc/ssl/private/benchmark.key -out /etc/ssl/certs/benchmark.pem \
    && chmod 600 /etc/ssl/private/benchmark.key
COPY sshd_config /etc/ssh/sshd_config
COPY vsftpd.conf /etc/vsftpd/benchmark.conf
COPY start.sh /usr/local/sbin/start.sh
RUN chmod 755 /usr/local/sbin/start.sh
EXPOSE 21 22
CMD ["/usr/local/sbin/start.sh"]
