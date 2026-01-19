# nui-sftp

This project will contain a cross platform SSH and SFTP client.
Still under construction :construction:

# Todo List

## General UI
- [x] Layouting.
- [x] Load Layout (on session start only, not afterwards).
- [ ] Custom themes?
- [ ] Custom top bar with moveability and minimize, maximize, close buttons.
- [ ] Accessible logs.
- [x] Multi session support.
- [ ] Unify style of operation queue and rest of the ui.
- [ ] Fix session being in hanging state when connect fails.
- [ ] Implement proper handling of connection loss.
- [ ] Move settings over to the right of the top bar and give it a gear icon.
- [ ] Add lumino button to add file explorer (at least back to 1, if not X times).
- [ ] Add lumino button to add operation queue (exactly 1).

## Settings UI
- [x] Add UI for settings.
- [x] Document settings.
- [ ] Save / Delete Current Layout.
- [ ] Better number boxes.
- [ ] Number box value constraining.
- [ ] list & map settings proper inheritance display.
- [ ] Fix list & map settings language keys.
- [ ] Terminal options: When whole thmee group is off, all setting should show as inherit, even if single ones are checked.

## SSH
- [x] Keyboard interactive password auth.
- [ ] Key auth.
- [ ] SSH agent support linux.
- [ ] SSH agent support windows?
- [ ] GSSAPI auth?
- [ ] KeypassXc database integration?

## Shell
- [x] SSH shell.
- [x] Terminal UIs work.
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
- [ ] Implement handling of symlinks in bulk downloads.
- [ ] Implement handling of symlinks in regular downloads.
- [ ] Implement handling of symlinks in bulk uploads.
- [ ] Implement handling of symlinks in regular uploads.
- [ ] Implement bulk download to archives.
- [ ] Bulk uploads.
- [ ] Show file properties.
- [ ] Optimize download speed.
- [ ] Optimize upload speed.
- [ ] Better error reporting, not just logs.

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
- [ ] Persist mode as part of layout.
- [ ] Delete remote files deep (non empty dirs) -> operation queue.
- [ ] Open files with associated application.
- [ ] Monitor changes on open files. Add monitoring tab for that and add auto upload option.
- [ ] File previews:
  - [ ] Image preview (image as icon?)
  - [ ] Text preview
- [ ] Scroll into view of recently selected item when using keyboard controls.