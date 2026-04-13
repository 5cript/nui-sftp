# Sync test fixtures

Small bash scripts that generate paired `local/` + `remote/` directory trees
with known, intentional differences, so you can point nui-sftp at them and
eyeball what the sync dialog reports.

## Run one

```sh
bash scripts/sync/basic.sh                 # default out: ./sync-fixtures/basic
bash scripts/sync/nested.sh /tmp/my-sync   # custom root
bash scripts/sync/random.sh '' 42 80       # seed 42, 80 files
```

Each script prints an inventory of both sides when it finishes so the expected
diff is obvious at a glance.

## Run them all

```sh
bash scripts/sync/all.sh                   # writes to ./sync-fixtures/
bash scripts/sync/all.sh /tmp/sync         # custom root
```

## Which fixture for what

| script          | exercises                                                       |
| --------------- | --------------------------------------------------------------- |
| `basic.sh`      | one-of-each-kind diff: identical, content, size, mtime, one-side |
| `nested.sh`     | multi-level trees, parent-dir-missing case, per-level ignore     |
| `mtime.sh`      | newer-wins direction picking with identical bytes                |
| `gitignore.sh`  | respect-ignore-files toggle incl. negation and anchors           |
| `symlinks.sh`   | same/different link targets, dangling, link-vs-file type mismatch |
| `random.sh`     | seeded stress fixture: many files, mixed categories              |

## How to test a fixture against nui-sftp

1. Run the script to produce the pair, e.g. `bash scripts/sync/basic.sh`.
2. In nui-sftp, open the local side at `<root>/basic/local` and the remote
   side (over SFTP to localhost or elsewhere) at `<root>/basic/remote`.
3. Right-click one of the directories → Synchronize.
4. Compare the rows in the sync dialog against the inventory printed at the
   end of the script, plus the per-fixture expectations documented in each
   script's header comment.
