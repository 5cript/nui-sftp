# File transfer behavior

How nui-sftp transfers files, and where the protocols make it behave differently.

## Uploads

### Safe mode

The default. A file is written to `<name>.filepart` next to its destination and renamed into place once complete, so
nobody sees a half written file and a failed upload leaves the destination as it was. An interrupted upload continues
its `.filepart` file later.

The destination is checked when the file is moved into place: an existing file is only replaced when overwriting
was allowed. A file larger than 4 KiB that may not overwrite is checked before the upload too, so it is not sent for
nothing. A smaller one is not: sending it costs less than checking every file, as long as existing destinations are
rare. Then the rename refuses it and its `.filepart` file is removed.

### Unsafe fast mode

Files below 1 MiB go straight to their destination: no `.filepart` file, no rename, no checks before or after. A file
that fails or is stopped is removed, a file it overwrote is lost. A destination that may not be overwritten is still
refused: SFTP refuses it when creating the file, FTP checks once before. File Tracking always uploads in the safe
mode.

### How the destination is checked

| Protocol | Check |
|---|---|
| SFTP | `lstat` of the path |
| FTP, server lists `MLST` in `FEAT` | `MLST` of the path |
| FTP, server lists `SIZE` but not `MLST` | `SIZE` and `MDTM` for a file, else `CWD` for a directory |
| FTP, server lists neither | none, see below |

Each check is one to three commands, none of them lists the directory. The `.filepart` file and the destination
right before the rename are looked up as files only, which is `SIZE` alone: only a file can be continued, and a file
renamed onto a directory fails by itself.

FTP has no command that reliably tells whether a path exists:

- On many servers `MLST`, `SIZE` and `CWD` follow symbolic links (seen on proftpd, pyftpdlib and vsftpd; pure-ftpd's
  `MLST` shows links as links). A link then counts as what it points to.
- A dangling link, or a path the server cannot reach, counts as free. A safe upload replaces such a link, an unsafe
  fast upload writes through it.
- Without `MLST` a special file (FIFO, socket, device) counts as free too: `SIZE` and `CWD` both refuse it.
- Without `MLST` a symbolic link to a directory that appears at the destination during an upload is replaced by the
  rename: the lookup right before it asks for a file only.

A command that fails for now (a `4yz` reply) tells nothing of the path: the upload fails, the path does not count as
free.

### FTP servers without `MLST` and `SIZE`

No command checks for a single existing file there, only the listing of its whole directory would, once per
uploaded file. nui-sftp does not do that. The server tells this in its `FEAT` reply at login. Uploads to such a
server:

- fail in the safe mode with `CannotCheckDestination`, a folder upload once for the whole folder;
- work in the unsafe fast mode at any size, replacing existing files without asking.

File Tracking uploads are always safe, so they fail on such servers. Directories an upload creates are still looked
up there, in the listing of their parent.

## Downloads

A downloaded file gets the modification time of its source. FTP's `LIST` and `STAT` round it to the minute or the
day, so a download takes the exact time the server tells after the transfer (`MDTM`, `MLST`, or `fstat` over SFTP),
which it asks for the permissions anyway; a download that keeps no permissions asks for the time alone when its listed
time is rounded. A sync plan and a download of several files carry how exact the scan or the listing told each time,
so their downloads ask the same way. The unsafe fast mode skips that round trip where the listing showed the
permissions and keeps the listed time. File Tracking relies on the exact time: it is what the server had when the copy
was made, unless the file changed while it was read.

## Sync

A sync compares the files of both sides by size and modification time. Times compare at the coarser precision of
both sides: FTP's `LIST` shows minutes, or only the day for files older than about six months, so a file differs only
when its minute or day does. `LIST` names no time zone and is read as UTC, as vsftpd and proftpd print it; on servers
that list local time (pure-ftpd, IIS) the remote times are off by the time zone offset.

### Unchanged since the compare

A sync, and File Tracking, only replace a destination that is still what they last saw: the same type, size and
modification time. They check this before the upload and again before the rename, with the same commands as
above.

- Times are compared at the coarser precision of both sides, as above.
- Over FTP the whole directory is listed only when the commands disagree with the compare in a way they may cause
  themselves: only the time differs and one side came from a listing (some servers list local time, `MLST` and `MDTM`
  are UTC), or the compare saw a symbolic link that the commands followed.
- Where the commands show a symbolic link as one (pure-ftpd), the directory is listed too: only a listing tells where
  a link points to.
