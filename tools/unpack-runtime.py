#!/usr/bin/env python3
"""Strict, bounded SDK archive installer. Does not execute archive content."""
import argparse
import gzip
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import stat
import sys
import tarfile
import tempfile
import unicodedata

RECEIPT = '.runtime-install.json'

class InvalidArchive(ValueError):
    pass

class SafeTarInfo(tarfile.TarInfo):
    def _proc_pax(self, archive):
        if self.size > 65536:
            raise InvalidArchive('PAX metadata exceeds its limit')
        return super()._proc_pax(archive)

    def _proc_gnulong(self, archive):
        if self.size > 8192:
            raise InvalidArchive('GNU name metadata exceeds its limit')
        return super()._proc_gnulong(archive)

class BoundedReader(io.RawIOBase):
    def __init__(self, source, limit):
        self.source, self.limit, self.total = source, limit, 0

    def readable(self):
        return True

    def read(self, size=-1):
        if size < 0:
            raise InvalidArchive('Unbounded archive read is not permitted')
        value = self.source.read(min(size, self.limit - self.total + 1))
        self.total += len(value)
        if self.total > self.limit:
            raise InvalidArchive('Decompressed archive exceeds its limit')
        return value

def digest(path):
    value = hashlib.sha256()
    with open(path, 'rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(chunk)
    return value.hexdigest()

def safe_name(name):
    if not isinstance(name, str) or not name or len(name.encode('utf-8')) > 4096:
        raise InvalidArchive('Archive member has an invalid name')
    if '\\' in name or any(unicodedata.category(character) in ('Cc', 'Cf') for character in name):
        raise InvalidArchive('Archive member has a control character or backslash')
    if name.startswith('/') or any(part == '..' for part in name.split('/')):
        raise InvalidArchive('Archive member escapes the installation root')
    parts = PurePosixPath(name).parts
    if any(':' in part for part in parts):
        raise InvalidArchive('Archive member is not a portable relative path')
    normalized = '/'.join(part for part in parts if part != '.')
    if normalized == RECEIPT:
        raise InvalidArchive('Archive cannot provide its own installer receipt')
    return normalized

def inspect_tree(directory):
    result = []
    for parent, directories, files in os.walk(directory, followlinks=False):
        for name in sorted(directories + files):
            path = Path(parent) / name
            relative = path.relative_to(directory).as_posix()
            if relative == RECEIPT:
                continue
            safe_name(relative)
            mode = path.lstat().st_mode
            if stat.S_ISLNK(mode) or not (stat.S_ISDIR(mode) or stat.S_ISREG(mode)):
                raise InvalidArchive('Installed tree contains a link or special file')
            if stat.S_ISREG(mode):
                result.append({'path': relative, 'size': path.stat().st_size, 'sha256': digest(path), 'executable': bool(mode & 0o111)})
            else:
                result.append({'path': relative, 'directory': True})
    return sorted(result, key=lambda item: item['path'])

def install(archive, destination, sha256, identity, max_bytes=8 * 1024 ** 3, max_files=100000):
    if archive.is_symlink() or not archive.is_file() or digest(archive) != sha256:
        raise InvalidArchive('Archive must be a regular file matching the exact SHA-256 pin')
    saved = None
    if destination.exists() or destination.is_symlink():
        if destination.is_symlink() or not destination.is_dir():
            raise InvalidArchive('Installation destination is already occupied')
        receipt_file = destination / RECEIPT
        if receipt_file.is_symlink() or receipt_file.stat().st_size > 64 * 1024 * 1024:
            raise InvalidArchive('Existing installation receipt is not a bounded regular file')
        saved = json.loads(receipt_file.read_text())
        if saved.get('schemaVersion') != 1 or saved.get('archiveSha256') != sha256 or saved.get('identity') != identity:
            raise InvalidArchive('Existing installation belongs to another artifact')
        # The receipt is not a trust anchor: independently rederive the tree from
        # the pinned archive below before accepting any cached installation.
    destination.parent.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix='.' + destination.name + '.install-', dir=destination.parent))
    try:
        seen, count, total = set(), 0, 0
        with gzip.open(archive, 'rb') as compressed:
            bounded = BoundedReader(compressed, max_bytes + max_files * 2048 + 1024 * 1024)
            with tarfile.open(fileobj=bounded, mode='r|', tarinfo=SafeTarInfo) as stream:
                for member in stream:
                    count += 1
                    if count > max_files:
                        raise InvalidArchive('Archive member count exceeds its limit')
                    if member.type not in (tarfile.REGTYPE, tarfile.AREGTYPE, tarfile.DIRTYPE) or member.sparse is not None:
                        raise InvalidArchive('Only ordinary files and directories are permitted')
                    name = safe_name(member.name)
                    if name in seen:
                        raise InvalidArchive('Archive contains a duplicate normalized path')
                    seen.add(name)
                    if not name:
                        if member.type != tarfile.DIRTYPE:
                            raise InvalidArchive('Root member must be a directory')
                        continue
                    if member.size < 0 or total + member.size > max_bytes:
                        raise InvalidArchive('Extracted archive exceeds its byte limit')
                    target = staging / name
                    if member.type == tarfile.DIRTYPE:
                        if member.size:
                            raise InvalidArchive('Directory member contains file bytes')
                        target.mkdir(parents=True, exist_ok=True)
                        continue
                    total += member.size
                    target.parent.mkdir(parents=True, exist_ok=True)
                    source = stream.extractfile(member)
                    if source is None:
                        raise InvalidArchive('File member has no data')
                    remaining = member.size
                    with target.open('xb') as output:
                        while remaining:
                            chunk = source.read(min(1024 * 1024, remaining))
                            if not chunk:
                                raise InvalidArchive('Truncated archive file')
                            output.write(chunk)
                            remaining -= len(chunk)
                        output.flush()
                        os.fsync(output.fileno())
                    target.chmod(0o755 if member.mode & 0o111 else 0o644)
                # Inspect the tar reader's own buffered tail, not just its underlying
                # reader: otherwise a hidden second archive could be silently ignored.
                while True:
                    tail = stream.fileobj.read(1024 * 1024)
                    if not tail:
                        break
                    if tail.strip(b'\0'):
                        raise InvalidArchive('Non-padding content follows the tar end marker')
        if digest(archive) != sha256:
            raise InvalidArchive('Archive changed during installation')
        files = inspect_tree(staging)
        if not any('sha256' in item for item in files):
            raise InvalidArchive('Installation archive has no files')
        if saved is not None:
            if saved.get('files') != files or inspect_tree(destination) != files:
                raise InvalidArchive('Installed bytes or receipt differ from the pinned archive')
            return {'destination': str(destination), 'reused': True, 'files': len(files), 'archiveSha256': sha256}
        receipt = {'schemaVersion': 1, 'archiveSha256': sha256, 'identity': identity, 'files': files}
        (staging / RECEIPT).write_text(json.dumps(receipt, indent=2, sort_keys=True) + '\n')
        if destination.exists() or destination.is_symlink():
            raise InvalidArchive('Installation destination appeared during extraction')
        staging.rename(destination)
        return {'destination': str(destination), 'reused': False, 'files': len(files), 'bytes': total, 'archiveSha256': sha256}
    finally:
        if staging.exists():
            shutil.rmtree(staging)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--archive', required=True)
    parser.add_argument('--destination', required=True)
    parser.add_argument('--sha256', required=True)
    parser.add_argument('--identity-json', required=True)
    parser.add_argument('--max-bytes', type=int, default=8 * 1024 ** 3)
    parser.add_argument('--max-files', type=int, default=100000)
    args = parser.parse_args()
    if args.max_bytes < 1 or args.max_files < 1:
        raise InvalidArchive('Positive extraction limits are required')
    identity = json.loads(args.identity_json)
    if not isinstance(identity, dict):
        raise InvalidArchive('Installation identity must be an object')
    print(json.dumps(install(Path(args.archive).absolute(), Path(args.destination).absolute(), args.sha256, identity, args.max_bytes, args.max_files)))

if __name__ == '__main__':
    try:
        main()
    except Exception:
        print('ERR_RUNTIME_ARCHIVE: invalid, changed or unsafe archive/installation; prior destination preserved', file=sys.stderr)
        sys.exit(1)
