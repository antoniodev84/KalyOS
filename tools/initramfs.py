import os
import stat
import sys
from pathlib import Path

root = Path(sys.argv[1])
output = Path(sys.argv[2])
entries = [root] + sorted(root.rglob('*'), key=lambda p: (len(p.parts), str(p)))
archive = bytearray()

def entry(name, mode, data, ino):
    encoded = name.encode() + b'\0'
    fields = [ino, mode, 0, 0, 2 if stat.S_ISDIR(mode) else 1, 0, len(data), 0, 0, 0, 0, len(encoded), 0]
    archive.extend(b'070701' + b''.join(f'{v:08x}'.encode() for v in fields))
    archive.extend(encoded)
    archive.extend(b'\0' * (-len(archive) % 4))
    archive.extend(data)
    archive.extend(b'\0' * (-len(archive) % 4))

for ino, path in enumerate(entries, 1):
    name = '.' if path == root else str(path.relative_to(root))
    metadata = path.lstat()
    if stat.S_ISLNK(metadata.st_mode):
        data = os.readlink(path).encode()
    elif stat.S_ISREG(metadata.st_mode):
        data = path.read_bytes()
    else:
        data = b''
    entry(name, metadata.st_mode, data, ino)
entry('TRAILER!!!', 0, b'', len(entries) + 1)
output.write_bytes(archive)
