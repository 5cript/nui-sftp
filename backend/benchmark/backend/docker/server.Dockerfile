# OpenSSH with its sftp-server for the transfer benchmarks. Pinned so runs stay comparable.
FROM alpine:3.24
RUN apk add --no-cache openssh-server openssh-sftp-server \
    && ssh-keygen -A \
    && adduser -D -s /bin/sh bench \
    && echo 'bench:bench' | chpasswd \
    && mkdir -p /home/bench/.ssh \
    && chmod 700 /home/bench/.ssh \
    && chown bench:bench /home/bench/.ssh
COPY sshd_config /etc/ssh/sshd_config
EXPOSE 22
CMD ["/usr/sbin/sshd", "-D", "-e"]
