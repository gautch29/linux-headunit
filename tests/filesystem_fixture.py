# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Bounded real metadata changes for fixtures on coarse-timestamp filesystems."""
import time


def change_metadata(path, change, before=None):
    before = path.stat() if before is None else before
    deadline = time.monotonic() + 1
    while True:
        change()
        after = path.stat()
        if (after.st_mtime_ns, after.st_ctime_ns) != (before.st_mtime_ns, before.st_ctime_ns):
            return
        if time.monotonic() >= deadline:
            raise AssertionError('filesystem metadata dates did not advance')
        time.sleep(.001)
