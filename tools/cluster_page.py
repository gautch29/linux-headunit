#!/usr/bin/env python3
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Select the native cluster's center page; suitable for local UI or button bindings."""
import argparse
import os
import stat
from pathlib import Path
import tempfile


def available(map_available):
    pages = ('media', 'vehicle', 'trip', 'minimal', 'sport', 'gauges')
    return ('map', *pages) if map_available else pages


def resolved(page, map_available):
    return page if page in available(map_available) else 'media'


def read(path, map_available):
    try:
        fd = os.open(path, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW | os.O_NONBLOCK)
        try:
            info = os.fstat(fd)
            if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1 or not 0 < info.st_size <= 17:
                raise ValueError('Invalid cluster page file')
            content = os.read(fd, 18)
            after = os.fstat(fd)
            if (len(content) != info.st_size or
                    (info.st_size, info.st_mtime_ns, info.st_ctime_ns, info.st_mode, info.st_nlink) !=
                    (after.st_size, after.st_mtime_ns, after.st_ctime_ns, after.st_mode, after.st_nlink)):
                raise ValueError('Cluster page changed during read')
            return resolved(content.decode('ascii').strip(), map_available)
        finally:
            os.close(fd)
    except (OSError, UnicodeError, ValueError):
        return 'map' if map_available else 'media'


def write(path, page, owner=None):
    """Atomically replace the page; `owner` (uid, gid) is the reading display account.

    The renderers run as that account, so a root-owned 0600 file would be
    unreadable and every selection would silently fall back to the default.
    """
    path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix='.cluster-page-', dir=path.parent)
    try:
        with os.fdopen(fd, 'w', encoding='ascii') as output:
            os.fchmod(output.fileno(), 0o600)
            if owner is not None:
                os.fchown(output.fileno(), *owner)
            output.write(page + '\n')
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def adopt(path, owner):
    """Give an existing page file to the display account (older releases wrote it as root)."""
    try:
        info = path.lstat()
    except FileNotFoundError:
        return
    if stat.S_ISREG(info.st_mode) and info.st_nlink == 1 and (info.st_uid, info.st_gid) != tuple(owner):
        os.chown(path, *owner, follow_symlinks=False)


def select(path, command, map_available, owner=None):
    pages = available(map_available)
    current = read(path, map_available)
    if command == 'next':
        choice = pages[(pages.index(current) + 1) % len(pages)]
    elif command == 'previous':
        choice = pages[(pages.index(current) - 1) % len(pages)]
    else:
        if command not in pages:
            raise ValueError('Choose an available native cluster page or format')
        choice = command
    write(path, choice, owner)
    return choice


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--state', required=True, type=Path)
    parser.add_argument('--map-available', action='store_true')
    parser.add_argument('page', choices=(*available(True), 'next', 'previous', 'status'))
    args = parser.parse_args()
    if args.page == 'status':
        print(read(args.state, args.map_available))
        return
    try:
        print(select(args.state, args.page, args.map_available))
    except ValueError as error:
        parser.error(str(error))


if __name__ == '__main__':
    main()
