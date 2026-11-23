# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Bounded, read-only JSON snapshots from an explicitly owned private directory."""
import json
import os
from pathlib import Path
import stat


class SnapshotChanged(ValueError):
    """An opened snapshot or its directory changed during the bounded read."""


def unique_object(pairs):
    value = {}
    for key, item in pairs:
        if key in value:
            raise ValueError('Duplicate private-state field')
        value[key] = item
    return value


def read_private(path, owner, maximum=2048):
    path = Path(path)
    parent = path.parent
    before = parent.lstat()
    if (parent.resolve(strict=True) != parent or not stat.S_ISDIR(before.st_mode) or
            before.st_uid != owner[0] or stat.S_IMODE(before.st_mode) != 0o700):
        raise ValueError('Private state parent must be real, owned and 0700')
    directory = os.open(parent, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC | os.O_NOFOLLOW)
    fd = -1
    try:
        opened = os.fstat(directory)
        if (opened.st_dev, opened.st_ino) != (before.st_dev, before.st_ino):
            raise SnapshotChanged('Private state parent changed during open')
        fd = os.open(path.name, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW | os.O_NONBLOCK,
                     dir_fd=directory)
        info = os.fstat(fd)
        if (not stat.S_ISREG(info.st_mode) or info.st_uid != owner[0] or
                stat.S_IMODE(info.st_mode) != 0o600 or
                not 0 < info.st_size <= maximum):
            raise ValueError('Private state must be an owned 0600 single-link bounded file')
        if info.st_nlink == 0:
            raise SnapshotChanged('Private state was unlinked during open')
        if info.st_nlink != 1:
            raise ValueError('Private state must be single-link')
        content = os.read(fd, maximum + 1)
        after, named_parent = os.fstat(fd), parent.lstat()
        if (len(content) != info.st_size or
                (after.st_size, after.st_mtime_ns, after.st_ctime_ns, after.st_mode, after.st_nlink) !=
                (info.st_size, info.st_mtime_ns, info.st_ctime_ns, info.st_mode, info.st_nlink) or
                (named_parent.st_dev, named_parent.st_ino, named_parent.st_uid, named_parent.st_mode) !=
                (before.st_dev, before.st_ino, before.st_uid, before.st_mode)):
            raise SnapshotChanged('Private state changed during read')
        try:
            return json.loads(content, object_pairs_hook=unique_object)
        except RecursionError as error:
            raise ValueError('Private state nesting exceeds its schema') from error
    finally:
        if fd >= 0:
            os.close(fd)
        os.close(directory)
