#!/bin/sh
# vsftpd on 21 (plain and AUTH TLS) and on 990 (implicit TLS), with the settings of the variant in $1.
# docker run --init reaps both, vsftpd as PID 1 segfaults when sessions end.
variant="${1:-default}"

{
    cat /etc/nui-sftp/base.conf "/etc/nui-sftp/$variant.conf"
    echo listen_port=21
    echo pasv_min_port=30000
    echo pasv_max_port=30049
    echo vsftpd_log_file=/var/log/vsftpd-explicit.log
} > /etc/vsftpd-explicit.conf
{
    cat /etc/nui-sftp/base.conf "/etc/nui-sftp/$variant.conf"
    echo implicit_ssl=YES
    echo listen_port=990
    echo pasv_min_port=30050
    echo pasv_max_port=30099
    echo vsftpd_log_file=/var/log/vsftpd-implicit.log
} > /etc/vsftpd-implicit.conf

vsftpd /etc/vsftpd-explicit.conf &
exec vsftpd /etc/vsftpd-implicit.conf
