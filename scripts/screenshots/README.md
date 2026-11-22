# Screenshots

Takes the screenshots used by the README, the webpage and the flatpak metainfo, with a demo profile and demo files
instead of real hosts and paths.

```sh
scripts/screenshots/take.sh
```

- Runs `build/clang_debug` (a CEF build) of the main checkout headless, also when started from a worktree, so no
  window opens. It drives the app over the DevTools protocol. `--build-dir` picks another build.
- bubblewrap mounts a fresh home at `/home/demo`, and `HOME` and `XDG_*` point into it. The real profile, command
  history and `~/.ssh` stay untouched.
- The demo tree is copied to the SFTP server (default: the sftp-container `sftpuser@192.168.178.70:5000`) under
  `/home/sftpuser/demo`, which is replaced on every run. A directory there that this script did not create is never
  touched.
- Output goes to `build/screenshots/`: framed PNGs (rounded corners, an outline and a drop shadow on a transparent
  border, like Spectacle), raw captures in `raw/`, and the profile and app logs in `work/`.
- Pick scenes with `--scenes session,settings`. The scenes are `session`, `history`, `snippets`, `synchronize`,
  `download_as_archive`, `live_edit`, `theming`, `settings` and `local`. Use `--scale 2` for HiDPI images.

## Upload

The upload happens only when a token is given, through `NUI_SFTP_S4_TOKEN` or `--token-file <path>`. The token is
`<access key id>:<secret access key>` of the MEGA S4 account; a token file may also hold the two parts on two
lines, or be the key file MEGA S4 offers for download (`aws_access_key_id=...`, `aws_secret_access_key=...`). The upload needs the aws CLI and is skipped when a scene failed. To upload a run you checked first:

```sh
scripts/screenshots/upload.sh --token-file ~/s4-token.txt build/screenshots
```

Every `<name>.png` goes to `nui-sftp-releases/images/Screenshots/<name>.png` (README, webpage). `session.png` also
goes to `nui-scp/images/nui-sftp-session.png`, the flatpak metainfo screenshot. Flathub copies screenshots when it
builds, so the store page changes with the next Flathub build.

Upload with this script only. It stores the files as `image/png` and then checks that the public URLs answer with
that type. GitHub shows README images through a proxy that refuses every other content type ("Non-Image
content-type returned"), so a PNG stored as `binary/octet-stream` is a broken image in the README while the webpage
still shows it. To check the URLs without uploading, no token needed:

```sh
scripts/screenshots/upload.sh --verify build/screenshots
```

## Working on a scene

```sh
scripts/screenshots/take.sh --scenes snippets --keep-running
node scripts/screenshots/drive.mjs --port 9460 --output build/screenshots/raw --scenes snippets
```

`--keep-running` leaves the app up after the run. `drive.mjs` can then run a scene again; the page-side helpers are
in `page_driver.js`. Stop the app with `kill $(cat build/screenshots/work/app.pid)`.
