# nui-sftp

This project contains a cross platform (Linux, Windows & macOS) SSH, SFTP and FTP(S) workbench.

![Session](https://s3.g.s4.mega.io/jgemkib4a5fte35rktt5wxrwkw4ejk4ybemkf/nui-sftp-releases/images/Screenshots/session.png)

## Website
[https://nui-sftp.nuicpp.org](https://nui-sftp.nuicpp.org)

# Installation

First go to the releases section https://github.com/5cript/nui-sftp/releases. This is where you can find the latest releases of the app. (Not every release might have packages attached to it; pick the latest one with packages.)

## Windows

Download the Windows installer/setup.exe and run it. Or download the Windows zip and extract the contents in a place of your choosing.

## GNU/Linux

### ArchLinux

nui-sftp is published on the AUR: [nui-sftp](https://aur.archlinux.org/packages/nui-sftp)
With your favorite AUR helper (yay as an example):
```
yay -S nui-sftp
```

Alternatively you can install directly from the PKGBUILD, which is primarily maintained in [this repository](https://github.com/5cript/nui-sftp-deploy):
```bash
git clone https://github.com/5cript/nui-sftp-deploy.git
cd nui-sftp-deploy
makepkg -si
```
to uninstall:
```bash
sudo pacman -R nui-sftp
```

### All Other Distributions

#### AppImage

An AppImage is provided for most distributions. It requires at least glibc 2.35 (the version shipped with Ubuntu 22.04). Download it from the releases section and run it directly:
```bash
chmod +x nui-sftp-*.AppImage
./nui-sftp-*.AppImage
```

#### Flatpak

A flatpak is published on [Flathub](https://flathub.org/apps/org.nuicpp.nui_sftp) as `org.nuicpp.nui_sftp`:
```bash
flatpak install flathub org.nuicpp.nui_sftp
flatpak run org.nuicpp.nui_sftp
```

Flatpak bundles are also attached to each release if you prefer to install one directly:
```bash
# Make sure to pass the correct file name!
flatpak install nui-sftp-flatpak-x86_64_0.1.1.flatpak
```

> **Note:** Local shells do not work inside the flatpak due to sandbox restrictions. If you need the local shell feature, use the AppImage or the AUR package instead.

## macOS

nui-sftp runs on Apple Silicon Macs (arm64) with macOS 15.2 or newer.

Download `nui-sftp-macos-arm64_<version>.dmg` from the releases section, open it and drag nui-sftp into the Applications folder.

The app is not signed with an Apple Developer ID, so Gatekeeper blocks the first start:
1. Start nui-sftp from Applications. macOS reports that it cannot verify the app; close that dialog.
2. Open System Settings > Privacy & Security, scroll down to Security and click "Open Anyway" next to the message about nui-sftp, then confirm.

Alternatively remove the quarantine flag the download received, then nui-sftp starts like any other app:
```bash
xattr -dr com.apple.quarantine /Applications/nui-sftp.app
```

Settings, logs and the command history live in `~/Library/Application Support/nui-sftp`.

## Other

There will not be support for any other systems (Mobile Devices or BSD).

# Feature List

## Synchronization
Compare a local and a remote folder and review the differences before anything is transferred. Choose the files to upload, download or delete, synchronize in both directions or only one, and leave out hidden files or whatever `.gitignore` lists.
![Synchronize](https://s3.g.s4.mega.io/jgemkib4a5fte35rktt5wxrwkw4ejk4ybemkf/nui-sftp-releases/images/Screenshots/synchronize.png)

## File Tracking
Open a remote file in its default application or in one you pick. nui-sftp watches the local copy and uploads it again every time you save. A local file or folder can be uploaded and watched the same way. The File Tracking panel lists every watch with its state, its failed uploads and the conflicts with changes made on the server. For a conflict you choose to replace the server's version or to keep it.
![FileTracking](https://s3.g.s4.mega.io/jgemkib4a5fte35rktt5wxrwkw4ejk4ybemkf/nui-sftp-releases/images/Screenshots/live_edit.png)

## FTP Servers
Connect to FTP servers, unencrypted or over explicit or implicit TLS. Transfers, synchronization, File Tracking and archives work in an FTP session as they do over SFTP, in the same file view. Certificates are checked against the certificate authorities trusted by your system. A certificate they do not vouch for, a self-signed one for example, can be reviewed and trusted manually. An FTP session has no terminals, as the FTP protocol has no shell.

## Local Shell
Run the shells of your own machine next to your remote sessions, with a file view. The shell command is configurable, and a local shell can also be opened as a tab inside a remote session. The flatpak is the exception: it runs in an isolated environment, where a shell does not see the programs of your system. Use the AppImage or the AUR package if you need local shells.
![LocalShellSession](https://s3.g.s4.mega.io/jgemkib4a5fte35rktt5wxrwkw4ejk4ybemkf/nui-sftp-releases/images/Screenshots/local.png)

## Command History & Snippets
Commands you run in a terminal are recorded per host. The history can be searched, sorted by recency or use count, and entries can be pinned, marked as favorites, run again or inserted into the terminal for editing.
Commands you need often can be saved as snippets, sorted into folders and tagged. A snippet can contain `{{placeholders}}` that are asked for when it runs. 27 ready-made folders (Docker, Git, systemd, Kubernetes, package managers and more) can be added from the snippets panel, and snippets can be copied and imported as JSON.
![Snippets](https://s3.g.s4.mega.io/jgemkib4a5fte35rktt5wxrwkw4ejk4ybemkf/nui-sftp-releases/images/Screenshots/snippets.png)

## Settings
When a user might want to change the behavior of the app, I decided: Let's make it configurable.
Servers inherit their settings from named defaults and override only what differs. The search box in the settings header (Ctrl+F) finds any setting in the defaults and in every server.
![Settings](https://s3.g.s4.mega.io/jgemkib4a5fte35rktt5wxrwkw4ejk4ybemkf/nui-sftp-releases/images/Screenshots/settings.png)

## Theming & Customizability
Three themes (Blue, Nebula and Plain), each with a dark and a light mode that can follow the system. Can be fully customized by overwriting CSS (although that's more of a power user feature).
![BrightTheme](https://s3.g.s4.mega.io/jgemkib4a5fte35rktt5wxrwkw4ejk4ybemkf/nui-sftp-releases/images/Screenshots/theming.png)

## Download & Upload directly as Archive
You can download and upload files and folders directly as tar archives, no manual packing required. The archive is packed while it is transferred, as `.tar`, `.tar.gz`, `.tar.bz2`, `.tar.zst` or `.tar.xz` with a compression level of your choice.
![DownloadAsArchive](https://s3.g.s4.mega.io/jgemkib4a5fte35rktt5wxrwkw4ejk4ybemkf/nui-sftp-releases/images/Screenshots/download_as_archive.png)

## SSH
- Password auth, the password is asked for when connecting.
- Key auth with optional passphrase.
- Automatic key auth via SSH directory.
- SSH agent support on Linux and macOS.
- Several users per server, each with their own keys.
- ProxyJump and ProxyCommand.
- Reads your SSH configuration file (can be turned off).
- Multi channel support (multiple ptys and sftp at the same time).
- Direct Connect: open a session to a host without adding it as a server.
- Reconnects when the connection is lost and restores the session.

## SFTP
- Download & Upload, Single files and directories.
- Several transfers at the same time, and several files at once within a directory transfer.
- Download & Upload to tar archives streamingly, no manual packing required.
- Folder Synchronization.
- File Tracking: Open a remote file or upload a local one, and every change you save is uploaded again.
- Continue interrupted downloads and uploads.
- Delete file.
- Rename file.
- Create file.
- Create directory.
- Recursive folder delete.
- Show file properties.
- Asks the user to overwrite files on upload/download.
- Unsafe fast mode that speeds up many small files, see [File transfer behavior](docs/file-transfer-behavior.md).

## Operation Queue
- Pause/Continue the queue.
- Cancel operations and delete or keep their partial files. Kept partial files can be continued or deleted later.
- Reorder operations while the queue is paused, or move one to the top.
- Transfer speed and progress per operation.
- Failed operations are kept in a list of their own.

## FTP & FTPS
- Plain FTP, explicit FTP over TLS and implicit FTP over TLS.
- Everything of the SFTP and Operation Queue lists above, with the same file view, queue and synchronization.
- Certificates are verified with the certificate authorities of the system, on Linux, Windows and macOS.
- An unverified certificate (self-signed, expired, issued for another server) is shown with its details and fingerprints. You can reject it, trust it once or trust it always.
- Anonymous login and several users per server.
- Passwords are asked for when connecting and never stored.
- Several connections per session: one for browsing, the others for parallel transfers.
- Keeps idle sessions alive and logs in again when the server closed the connection.
- Passive mode (EPSV and PASV).

## General UI
- Flexible Layout - organize widgets as you like
- Save and Load Layouts.
- Run multiple SSH, FTP or Local Shell Session in parallel.
- Notification log.
- English and German.

## Settings UI
- If it's configurable, it's in the settings.

## File View UI
- Local and remote files side by side.
- Grid view.
- Table view with sortable and resizable columns.
- Box drag select for icon view.
- Places, favorites and devices. Remote favorites are kept per server.
- Path bar with suggestions.
- Auto refresh after operation complete.
- Resize left vs right.
- Upload drag and drop (within view and into the app).
- Download drag and drop (within view).
- Local files: copy and move (drag and drop or cut/copy/paste), rename, delete, open, open with and show in the file manager.
- Searching files.
- Sorting files.
- Show or hide hidden files.
- Keyboard: F2 rename, F5 refresh, Delete, Enter, Backspace, Ctrl+C/X/V and selecting by typing a name.

# Documentation
- [File transfer behavior](docs/file-transfer-behavior.md): how uploads check, replace and continue files, per protocol.

# No intention to support for now:
- FTP active mode and ASCII transfers: FTP uses passive mode and binary transfers only.
- FTP servers without Unix-like paths (VMS, MVS).
- Windows SSH agent support, waiting for official libssh support, before I resort to patches: https://gitlab.com/libssh/libssh-mirror/-/issues/277
- GSSAPI auth: Will make work when a need arises
- KeePassXC database integration: lots of work for tiny use cases. It's a cool idea for later.

# Known Problems

- Cannot use mouse side keys for history navigation (yet).
