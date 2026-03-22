# nui-sftp

This project contains a cross platform (Linux & Windows for now) SSH and SFTP client.

# Todo List

## General UI
- [x] Layouting.
- [x] Load Layout (on session start only, not afterwards).
- [x] Fix session being in hanging state when connect fails.
- [x] Implement proper handling of connection loss.
- [x] Multi session support.
- [x] Accessible logs.

## Operation Queue (UI)
- [x] Better header footer borders
- [x] MUCH smaller list items.
- [x] MUCH smaller icons.
- [x] Scroll operations into view when they complete.
- [x] "Remove" to an X button
- [x] Error message in hover.
- [x] Sometimes refreshes on remote side are not added to queue. (Bulk delete for example)

## Settings UI
- [x] Add UI for settings.
- [x] Document settings.
- [x] Save / Delete Current Layout.
- [x] Better number boxes.
- [x] Number box value constraining.

## SSH
- [x] Keyboard interactive password auth.
- [x] Key auth.
- [x] Key auth with passphrase.
- [x] Automatic key auth via SSH directory.
- [x] Password auth.
- [x] SSH agent support linux.

## Shell
- [x] SSH shell.
- [x] Terminal UIs works.
- [x] Terminal resizing.

## SFTP
- [x] Single file downloads.
- [x] Bulk download.
- [x] Forward download options for bulk downloads.
- [x] Single file uploads.
- [x] Delete file.
- [x] Rename file.
- [x] Create file.
- [x] Create directory.
- [x] Pause/Continue operations.
- [x] Cancel operations.
- [x] Bulk uploads.
- [x] Show file properties.
- [x] Recursive folder delete.
- [x] Error message on delete fail.
- [x] Delete confirm focus default "ok".
- [x] Implement handling of symlinks in bulk downloads.
- [x] Implement handling of symlinks in regular downloads.
- [x] Implement handling of symlinks in bulk uploads.
- [x] Implement handling of symlinks in regular uploads.
- [x] Default directory on connect.

## File View UI
- [x] Grid view.
- [x] Table view.
- [x] Split UI into local and remote views.
- [x] Box drag select for icon view.
- [x] Box drag select for table view.
- [x] Auto refresh after operation complete.
- [x] Resize left vs right.
- [x] Upload drag and drop (within view and perhaps into the app).
- [x] Download drag and drop (within view and perhaps outside the app).
- [x] Searching files.
- [x] Sorting files.
- [x] Keyboard input.
- [x] Sorting modes in icon view / sort menu.
- [x] Context menu for file operations.
- [x] Delete remote files (empty dirs and files).
- [x] Rename remote file.
- [x] Ask user to overwrite files on upload/download.
- [x] Drag and drop from system on Windows.
- [x] Drag and drop from system on Linux. (Partially, only 1 file from file managers due to webkit breaking itself for security reasons without providing a proper fix. https://github.com/WebKit/WebKit/commit/89838b9164a1dd3baa7053539cf93414977fb081)
- [x] Persist mode as part of layout.

# No intention to support for now:
- Windows SSH agent support, waiting for official libssh support, before I resort to patches: https://gitlab.com/libssh/libssh-mirror/-/issues/277
- GSSAPI auth: Will make work when a need arises
- KeypassXc database integration: lots of work for tiny use cases. Its a cool idea for later.

# Not Release 1 Features

## General UI
- [ ] Custom top bar with moveability and minimize, maximize, close buttons.
- [ ] More/Custom themes.
- [ ] Split language file into multiple for each language for easier maintenance.

## Synchronization
- [ ] Synchronize directories.

## SFTP
- [ ] Implement bulk download to archives.
- [ ] Optimize download/upload speed of lots of files.
- [ ] Test utf path support and implement if faulty.

## Shell
- [ ] Local shell is broken on windows and linux. There is a bigger fix needed because I need to fork at the very start of main to an intermediate process that then starts the actual app and the shell processes on linux.

## Settings UI
- [ ] Option to disable some warning boxes.

## File View UI
- [ ] Same side drag and drop for moving files locally or remotely.
- [ ] Better indication that on drag drop, all selected items are being dragged.
- [ ] File previews:
  - [ ] Image preview (image as icon?)
  - [ ] Text preview
- [ ] Show link target in properties window.
- [X] Improve arrow navigation with shift and control.
- [X] Scroll into view of recently selected item when using keyboard controls.
- [ ] Mouse side keys
- [ ] Ctrl + C / Ctrl + V for copy paste.

## Open Files & Monitoring
- [ ] Open files with associated application.
- [ ] Monitor changes on open files. Add monitoring tab for that and add auto upload option.