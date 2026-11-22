#!/bin/sh
# Creates the user ftptest and the fixture tree that every server serves. Tests work on copies of it.
set -eu

home=/srv/ftptest
fixtures="$home/fixtures"

useradd -m -d "$home" -s /bin/bash ftptest
echo "ftptest:ftptest" | chpasswd
mkdir -p "$fixtures/directory/nested"

# Files of known sizes, their hashes are in fixtures.sha256.
content() {
    yes nui-sftp | head -c "$1"
}
: > "$fixtures/empty.bin"
content 1 > "$fixtures/one.bin"
content 16383 > "$fixtures/almost-a-chunk.bin"
content 16384 > "$fixtures/chunk.bin"
content 10485760 > "$fixtures/large.bin"

printf 'plain text\n' > "$fixtures/plain.txt"
printf 'nested\n' > "$fixtures/directory/nested/inside.txt"

# Names that break careless parsers.
printf 'spaces\n' > "$fixtures/name with spaces.txt"
printf 'leading\n' > "$fixtures/ leading space.txt"
printf 'arrow\n' > "$fixtures/a -> b.txt"
printf 'utf8\n' > "$fixtures/$(printf '\303\274n\303\257c\303\266d\303\251-\345\220\215\345\211\215.txt')"
printf 'long\n' > "$fixtures/$(printf '%0251d' 0 | tr 0 n).txt"
# Listed only where the server shows hidden entries: MLSD, or LIST -a.
printf 'hidden\n' > "$fixtures/.hidden"

ln -s plain.txt "$fixtures/link-to-plain.txt"
ln -s directory "$fixtures/link-to-directory"
ln -s does-not-exist "$fixtures/dangling-link"

(cd "$fixtures" && find . -type f -exec sha256sum {} +) > "$home/fixtures.sha256"
chown -R ftptest:ftptest "$home"

# Owned by root, so the user cannot read the file or change anything in the directory.
printf 'secret\n' > "$fixtures/unreadable.txt"
chown root:root "$fixtures/unreadable.txt"
chmod 600 "$fixtures/unreadable.txt"
mkdir "$fixtures/unwritable"
printf 'inside\n' > "$fixtures/unwritable/file.txt"
chown -R root:root "$fixtures/unwritable"
chmod 755 "$fixtures/unwritable"
