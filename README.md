# nui-sftp

This project will contain a cross platform SSH and SFTP client.
Still under construction :construction:

# Todo List

## General UI
- [x] Layouting.
- [x] Load Layout (on session start only, not afterwards).
- [x] Fix session being in hanging state when connect fails.
- [x] Implement proper handling of connection loss.
- [x] Multi session support.
- [ ] Custom top bar with moveability and minimize, maximize, close buttons.
- [ ] More/Custom themes.
- [ ] Accessible logs.
- [ ] Unify style of operation queue and rest of the ui.
- [ ] Scroll operations into view when they complete.

## Settings UI
- [x] Add UI for settings.
- [x] Document settings.
- [x] Save / Delete Current Layout.
- [x] Better number boxes.
- [x] Number box value constraining.
- [ ] Option to disable some warning boxes.

## SSH
- [x] Keyboard interactive password auth.
- [x] Key auth.
- [x] Key auth with passphrase.
- [x] Automatic key auth via SSH directory.
- [x] Password auth.
- [x] SSH agent support linux.

### No intention to support for now:
- Windows SSH agent support, waiting for official libssh support, before I resort to patches: https://gitlab.com/libssh/libssh-mirror/-/issues/277
- GSSAPI auth: Will make work when a need arises
- KeypassXc database integration: lots of work for tiny use cases. Its a cool idea for later.

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
- [ ] Implement handling of symlinks in bulk downloads.
- [ ] Implement handling of symlinks in regular downloads.
- [ ] Implement handling of symlinks in bulk uploads.
- [ ] Implement handling of symlinks in regular uploads.
- [ ] Optimize download speed!!!
- [ ] Optimize upload speed!!!
- [ ] Implement bulk download to archives.
- [ ] Synchronize directories.

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
- [ ] Better indication that on drag drop, all selected items are being dragged.
- [ ] Improve arrow navigation with shift and control.
- [ ] Same side drag and drop for moving files locally or remotely.
- [x] Drag and drop from system on Windows.
- [ ] Drag and drop from system on Linux.
- [x] Persist mode as part of layout.
- [ ] File previews:
  - [ ] Image preview (image as icon?)
  - [ ] Text preview
- [ ] Scroll into view of recently selected item when using keyboard controls.

## Open Files & Monitoring
- [ ] Open files with associated application.
- [ ] Monitor changes on open files. Add monitoring tab for that and add auto upload option.
