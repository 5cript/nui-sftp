# nui-sftp

This project contains a cross platform (Linux & Windows) SSH and SFTP client.

![AppImage](https://s3.g.s4.mega.io/jgemkib4a5fte35rktt5wxrwkw4ejk4ybemkf/nui-scp/images/Screenshots/session.png)

# Installation

First go to the releases section https://github.com/5cript/nui-sftp/releases This is where you can find the latest releases of the app. (Not every release might have packages attached to it, pick the latest one with packages).

## Windows

Download the windows zip and extract it somewhere. If you want an installer, create an issue so I know that there is demand.

## GNU/Linux

### ArchLinux

You can install the flatpak or install via makepkg
```bash
git clone https://github.com/5cript/nui-sftp-deploy.git
cd nui-sftp-deploy
makepkg -si
```
to uninstall:
```bash
sudo pacman -R nui-sftp
```

This project will be submitted to the AUR at a later date.

### All Other Distributions

For other distributions a flatpak is provided. Currently not in the official flathub, but that will come eventually.
Download the flatpak from a release and install it like so:
```bash
# Make sure to pass the correct file name!
flatpak install nui-sftp-flatpak-x86_64_0.1.1.flatpak
```

## Apple Computers

macOS is currently not supported. Maybe in the future if there is demand.

## Other

There will not be support for any other systems (Mobile Devices or BSD).

# Feature List

## Synchronization
You can synchronize folders between your local machine and the server. This will automatically upload/download files that are changed in the source folder to the target folder. You can also choose to only synchronize in one direction (upload or download).
![Synchronize](https://s3.g.s4.mega.io/jgemkib4a5fte35rktt5wxrwkw4ejk4ybemkf/nui-scp/images/Screenshots/synchronize.png)

## Local Shell
Run a PTY on your local machine with a file view.
![LocalShellSession](https://s3.g.s4.mega.io/jgemkib4a5fte35rktt5wxrwkw4ejk4ybemkf/nui-scp/images/Screenshots/local.png)

## Settings
When a user might want to change the behavior of the app, I decided: Lets make it configurable.
![Settings](https://s3.g.s4.mega.io/jgemkib4a5fte35rktt5wxrwkw4ejk4ybemkf/nui-scp/images/Screenshots/settings.png)

## Theming & Customizability
Includes bright theme and more theme colors. Can be fully customized by overwriting CSS (although thats more of a power user feature).
![BrightTheme](https://s3.g.s4.mega.io/jgemkib4a5fte35rktt5wxrwkw4ejk4ybemkf/nui-scp/images/Screenshots/theming.png)

## Download & Upload directly as Archive
You can download and upload files and folders directly as tar archives, no manual packing required.
![BrightTheme](https://s3.g.s4.mega.io/jgemkib4a5fte35rktt5wxrwkw4ejk4ybemkf/nui-scp/images/Screenshots/download_as_archive.png)

## SSH
- Keyboard interactive password auth.
- Key auth with optional passphrase.
- Automatic key auth via SSH directory.
- SSH agent support on Linux.
- Multi channel support (multiple ptys and sftp at the same time).

## SFTP
- Download & Upload, Single files and directories.
- Download & Upload to tar archives streamingly, no manual packing required.
- Folder Synchronization.
- Download & Live observe changes that automatically resync to the server.
- Delete file.
- Rename file.
- Create file.
- Create directory.
- Pause/Continue operations.
- Cancel operations.
- Reorder operations in the queue.
- Show file properties.
- Recursive folder delete.

## General UI
- Flexible Layouting - organize widgets as you like
- Save and Load Layouts.
- Run multiple SSH or Local Shell Session in parallel.

## Settings UI
- If its configurable, its in the settings.

## File View UI
- Grid view.
- Table view.
- Box drag select for icon view.
- Auto refresh after operation complete.
- Resize left vs right.
- Upload drag and drop (within view and into the app (linux caveat: only a single file, bug in webkitgtk)).
- Download drag and drop (within view).
- Searching files.
- Sorting files.
- Asks the user to overwrite files on upload/download.

# No intention to support for now:
- Windows SSH agent support, waiting for official libssh support, before I resort to patches: https://gitlab.com/libssh/libssh-mirror/-/issues/277
- GSSAPI auth: Will make work when a need arises
- KeypassXc database integration: lots of work for tiny use cases. Its a cool idea for later.

# Known Problems

- Cannot use mouse side keys for history navigation (yet) because of a bug in webkit show them as "button 0".
- On WebKit Systems (Linux), the drag and drop from the desktop only supports single files and folders, not a list of items, because WebKit removed the functionality entirely because of an unfixed security issue.