#!/usr/bin/env python3
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Offline, no-overwrite export of an explicitly selected private history archive."""
import argparse
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import signal
import stat
import sys
import uuid

from trip_history import MAX_BYTES, encode, metadata, restore

MAX_EXPORT_BYTES = MAX_BYTES + 2048
SIGNALS = (signal.SIGINT, signal.SIGTERM, signal.SIGHUP)


class Interrupted(BaseException):
    def __init__(self, signum):
        self.signum = signum


@contextmanager
def acquiring():
    """Register ownership before a pending soft signal can unwind acquisition."""
    old = signal.pthread_sigmask(signal.SIG_BLOCK, SIGNALS)
    try:
        yield
    finally:
        signal.pthread_sigmask(signal.SIG_SETMASK, old)


def canonical(value):
    path = Path(value)
    if (not path.is_absolute() or str(path) != value or len(os.fsencode(value)) > 4000 or
            any(ord(char) < 32 or ord(char) == 127 for char in value)):
        raise ValueError('Invalid path')
    return path


def private_directory(path, fd):
    info = os.fstat(fd)
    named = path.lstat()
    if (path.resolve(strict=True) != path or not stat.S_ISDIR(info.st_mode) or
            info.st_uid != os.getuid() or info.st_gid != os.getgid() or
            stat.S_IMODE(info.st_mode) != 0o700 or
            metadata(info)[:2] != metadata(named)[:2]):
        raise ValueError('Invalid directory')
    return metadata(info)[:2]


def private_file(info):
    if (not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or
            info.st_gid != os.getgid() or stat.S_IMODE(info.st_mode) != 0o600 or
            info.st_nlink != 1 or not 0 < info.st_size <= MAX_BYTES):
        raise ValueError('Invalid input')


class Input:
    def __init__(self, value):
        self.path = canonical(value)
        self.directory = self.fd = None
        try:
            with acquiring():
                self.directory = os.open(self.path.parent, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC)
            self.directory_identity = private_directory(self.path.parent, self.directory)
            with acquiring():
                self.fd = os.open(self.path.name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK | os.O_CLOEXEC,
                                  dir_fd=self.directory)
            before = os.fstat(self.fd)
            private_file(before)
            self.mark = metadata(before)
            self.raw = self.read()
            self.check()
            if self.read() != self.raw:
                raise ValueError('Input bytes changed')
            self.check()
            self.history = restore(self.raw)
        except BaseException:
            self.close()
            raise

    def read(self):
        os.lseek(self.fd, 0, os.SEEK_SET)
        raw = bytearray()
        while len(raw) <= MAX_BYTES:
            chunk = os.read(self.fd, MAX_BYTES + 1 - len(raw))
            if not chunk:
                break
            raw.extend(chunk)
        if len(raw) != self.mark[6]:
            raise ValueError('Input size changed')
        return bytes(raw)

    def check(self):
        if (private_directory(self.path.parent, self.directory) != self.directory_identity or
                metadata(os.fstat(self.fd)) != self.mark or
                metadata(os.stat(self.path.name, dir_fd=self.directory, follow_symlinks=False)) != self.mark):
            raise ValueError('Input identity changed')

    def close(self):
        for key in ('fd', 'directory'):
            fd = getattr(self, key)
            if fd is not None:
                os.close(fd)
                setattr(self, key, None)


def export(input_path, output_directory):
    folder = canonical(output_directory)
    source = None
    parent = directory = fd = None
    created = published = complete = False
    identity = file_identity = None
    temporary = '.trip-export-' + uuid.uuid4().hex
    target = 'trip-history-export.json'
    try:
        with acquiring():
            source = Input(input_path)
        # restore is the sole schema/arithmetic authority; retain its exact values.
        input_hash = hashlib.sha256(source.raw).hexdigest()
        body = {'version': 1, 'kind': 'ultra-headunit-trip-history-export',
                'classification': 'HISTORICAL', 'sourceSchema': 'ultra-headunit-trip-history-v1',
                'inputSha256': input_hash, 'recordCount': len(source.history),
                'history': source.history}
        raw = encode(body, MAX_EXPORT_BYTES)
        with acquiring():
            parent = os.open(folder.parent, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC)
        parent_identity = private_directory(folder.parent, parent)
        with acquiring():
            os.mkdir(folder.name, 0o700, dir_fd=parent)
            created = True
            identity = metadata(os.stat(folder.name, dir_fd=parent, follow_symlinks=False))[:2]
            directory = os.open(folder.name, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC,
                                dir_fd=parent)
        if private_directory(folder, directory) != identity:
            raise ValueError('Output replaced')
        with acquiring():
            fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW | os.O_CLOEXEC,
                         0o600, dir_fd=directory)
            file_identity = metadata(os.fstat(fd))[:2]
        os.fchmod(fd, 0o600)
        offset = 0
        while offset < len(raw):
            written = os.write(fd, raw[offset:])
            if written <= 0:
                raise OSError('Incomplete write')
            offset += written
        info = os.fstat(fd)
        if (info.st_size != len(raw) or info.st_nlink != 1 or
                stat.S_IMODE(info.st_mode) != 0o600 or
                (info.st_uid, info.st_gid) != (os.getuid(), os.getgid())):
            raise ValueError('Output permissions changed')
        os.fsync(fd)
        source.check()
        if (private_directory(folder.parent, parent) != parent_identity or
                private_directory(folder, directory) != identity or
                metadata(os.stat(temporary, dir_fd=directory, follow_symlinks=False)) != metadata(os.fstat(fd))):
            raise ValueError('Output identity changed')
        info = os.fstat(fd)
        if (info.st_size != len(raw) or info.st_nlink != 1 or
                stat.S_IMODE(info.st_mode) != 0o600 or
                (info.st_uid, info.st_gid) != (os.getuid(), os.getgid())):
            raise ValueError('Output permissions changed')
        # link is atomic and fails on ANY existing destination; replace cannot do that.
        with acquiring():
            os.link(temporary, target, src_dir_fd=directory, dst_dir_fd=directory, follow_symlinks=False)
            published = True
        os.unlink(temporary, dir_fd=directory)
        os.fsync(directory)
        os.fsync(parent)
        source.check()
        if (private_directory(folder, directory) != identity or
                private_directory(folder.parent, parent) != parent_identity or
                metadata(os.stat(target, dir_fd=directory, follow_symlinks=False)) != metadata(os.fstat(fd)) or
                os.fstat(fd).st_nlink != 1 or os.fstat(fd).st_size != len(raw) or
                stat.S_IMODE(os.fstat(fd).st_mode) != 0o600 or
                (os.fstat(fd).st_uid, os.fstat(fd).st_gid) != (os.getuid(), os.getgid())):
            raise ValueError('Published output changed')
        complete = True
        return {'recordCount': len(source.history), 'inputSha256': input_hash,
                'exportSha256': hashlib.sha256(raw).hexdigest(), 'path': str(folder / target)}
    finally:
        # Soft signals cannot interrupt our bounded cleanup. Never remove a foreign inode.
        with acquiring():
            if directory is not None:
                for name in (temporary, target) if published and not complete else (temporary,):
                    try:
                        if metadata(os.stat(name, dir_fd=directory, follow_symlinks=False))[:2] == file_identity:
                            os.unlink(name, dir_fd=directory)
                    except OSError:
                        pass
            for descriptor in (fd, directory):
                if descriptor is not None:
                    os.close(descriptor)
            if created and not complete and parent is not None and identity is not None:
                try:
                    if metadata(os.stat(folder.name, dir_fd=parent, follow_symlinks=False))[:2] == identity:
                        os.rmdir(folder.name, dir_fd=parent)
                except (FileNotFoundError, OSError):
                    pass
            if parent is not None:
                os.close(parent)
            if source is not None:
                source.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', required=True, help='Explicit canonical private history file')
    parser.add_argument('--output-directory', required=True, help='New directory below an owned 0700 parent')
    args = parser.parse_args()
    previous = {}
    def interrupt(signum, frame):
        raise Interrupted(signum)
    try:
        for signum in SIGNALS:
            previous[signum] = signal.signal(signum, interrupt)
        print(json.dumps(export(args.input, args.output_directory), sort_keys=True))
        return 0
    except Interrupted as error:
        print('Trip history export interrupted; durability unknown.', file=sys.stderr)
        return 128 + error.signum
    except (OSError, ValueError, TypeError, OverflowError):
        print('Trip history export refused or failed; durability unknown.', file=sys.stderr)
        return 1
    finally:
        for signum, handler in previous.items():
            signal.signal(signum, handler)


if __name__ == '__main__':
    sys.exit(main())
