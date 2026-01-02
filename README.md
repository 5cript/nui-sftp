# nui-sftp

This project will contain a cross platform SSH and SFTP client.
Still under construction :construction:

# Todo List

## General UI
- [x] Layouting.
- [ ] Save Layout (done partially).
- [x] Load Layout (on session start only, not afterwards).
- [ ] Custom themes?
- [ ] Custom top bar with moveability and minimize, maximize, close buttons.
- [ ] Accessible logs.
- [x] Multi session support.
- [ ] Unify style of operation queue and rest of the ui.

## Settings UI
- [ ] Add UI for settings.
- [ ] Document settings.

## SSH
- [ ] Key auth.
- [ ] SSH agent support linux.
- [ ] SSH agent support windows?
- [x] Keyboard interactive password auth.
- [ ] GSSAPI auth?
- [ ] KeypassXc database integration?

## Shell
- [x] SSH shell.
- [x] Terminal UIs work.
- [x] Terminal resizing.

## SFTP
- [x] Single file downloads.
- [x] Bulk download.
- [ ] Implement handling of symlinks in bulk downloads.
- [ ] Implement handling of symlinks in regular downloads.
- [ ] Implement handling of symlinks in bulk uploads.
- [ ] Implement handling of symlinks in regular uploads.
- [ ] Implement bulk download to archives.
- [x] Forward download options for bulk downloads.
- [x] Single file uploads.
- [ ] Bulk uploads.
- [x] Delete file.
- [x] Rename file.
- [x] Create file.
- [x] Create directory.
- [ ] Show file properties.
- [ ] Optimize download speed.
- [ ] Optimize upload speed.
- [ ] Better error reporting, not just logs.
- [x] Pause/Continue operations.
- [x] Cancel operations.

## File View UI
- [x] Grid view.
- [x] Table view.
- [x] Split UI into local and remote views.
- [x] Box drag select.
- [ ] Better indication that on drag drop, all selected items are being dragged.
- [ ] Auto refresh after operation idle.
- [ ] Improve arrow navigation with shift and control.
- [x] Resize left vs right.
- [x] Upload drag and drop (within view and perhaps into the app).
- [x] Download drag and drop (within view and perhaps outside the app).
- [ ] Same side drag and drop for moving files locally or remotely.
- [x] Searching files.
- [x] Sorting files.
- [x] Keyboard input.
- [ ] Persist mode as part of layout.
- [x] Sorting modes in icon view / sort menu.
- [x] Context menu for file operations.
- [x] Delete remote files (empty dirs and files).
- [ ] Delete remote files deep (non empty dirs) -> operation queue.
- [x] Rename remote file.
- [ ] Make Context menu pretty
- [ ] ...Miscellaneous menu items.
- [ ] Open files with associated application.
- [ ] Monitor changes on open files. Add monitoring tab for that and add auto upload option.
- [ ] File previews:
  - [ ] Image preview (image as icon?)
  - [ ] Text preview