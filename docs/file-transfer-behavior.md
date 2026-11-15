# File transfer behavior

How nui-sftp transfers files, and where the protocols make it behave differently.

## Uploads

### Safe mode

The default. A file is written to `<name>.filepart` next to its destination and renamed into place once complete, so
nobody sees a half written file and a failed upload leaves the destination as it was. An interrupted upload continues
its `.filepart` file later.

The destination is checked before the upload and again when the file is moved into place. An existing file is only
replaced when overwriting was allowed.

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
| FTP, server lists `SIZE` but not `MLST` | `SIZE`, then `CWD` for a directory |
| FTP, server lists neither | none, see below |

Each check is one or two commands. FTP never lists the whole directory to check a single file.

FTP has no command that reliably tells whether a path exists:

- On many servers `MLST`, `SIZE` and `CWD` follow symbolic links (seen on proftpd, pyftpdlib and vsftpd; pure-ftpd's
  `MLST` shows links as links). A link then counts as what it points to.
- A dangling link, or a path the server cannot reach, counts as free. A safe upload replaces such a link, an unsafe
  fast upload writes through it.

### FTP servers without `MLST` and `SIZE`

Nothing can check for an existing file there. The server tells this in its `FEAT` reply at login. Uploads to such a
server:

- fail in the safe mode with `CannotCheckDestination`, a folder upload once for the whole folder;
- work in the unsafe fast mode at any size, replacing existing files without asking.

File Tracking uploads are always safe, so they fail on such servers.
