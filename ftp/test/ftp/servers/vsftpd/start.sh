#!/bin/sh
# vsftpd on 21 (plain and AUTH TLS) and on 990 (implicit TLS), with the settings of the variant in $1.
# docker run --init reaps both, vsftpd as PID 1 segfaults when sessions end.
# vsftpd keeps the PASV port of a transfer until the transfer ends and ends the session with a plaintext
# "500 OOPS: vsf_sysutil_bind" when nine random picks find no free port, so the ranges stay far wider than the
# transfers a parallel test run holds at once.
variant="${1:-default}"

{
    cat /etc/nui-sftp/base.conf "/etc/nui-sftp/$variant.conf"
    echo listen_port=21
    echo pasv_min_port=30000
    echo pasv_max_port=30999
    echo vsftpd_log_file=/var/log/vsftpd-explicit.log
} > /etc/vsftpd-explicit.conf
{
    cat /etc/nui-sftp/base.conf "/etc/nui-sftp/$variant.conf"
    echo implicit_ssl=YES
    echo listen_port=990
    echo pasv_min_port=31000
    echo pasv_max_port=31999
    echo vsftpd_log_file=/var/log/vsftpd-implicit.log
} > /etc/vsftpd-implicit.conf

vsftpd /etc/vsftpd-explicit.conf &
exec vsftpd /etc/vsftpd-implicit.conf
