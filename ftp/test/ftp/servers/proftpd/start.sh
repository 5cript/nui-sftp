#!/bin/sh
# proftpd on 21 (plain and AUTH TLS) and on 990 (implicit TLS), with the settings of the variant in $1.
variant="${1:-default}"
ln -sf "/etc/nui-sftp/$variant.conf" /etc/nui-sftp/variant.conf
exec proftpd --nodaemon --config /etc/nui-sftp/proftpd.conf
