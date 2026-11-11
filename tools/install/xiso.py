import os
import struct

SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"

PARTITION_OFFSETS = (0, 0x2080000, 0xFD90000, 0x18300000)
ATTRIBUTE_DIRECTORY = 0x10
EMPTY = 0xFFFF


class XisoError(Exception):
    pass


class Xiso:
    pass

    def __init__(self, path):
        self.path = path
        self.f = open(path, "rb")
        self.size = os.fstat(self.f.fileno()).st_size
        self.base = None
        for offset in PARTITION_OFFSETS:
            if offset + 33 * SECTOR > self.size:
                continue
            self.f.seek(offset + 32 * SECTOR)
            head = self.f.read(SECTOR)
            if head[:20] == MAGIC and head[SECTOR - 20:] == MAGIC:
                self.base = offset
                self.root_sector, self.root_size = struct.unpack_from("<II", head, 20)
                break
        if self.base is None:
            self.f.close()
            raise XisoError(f"{path} is not an Xbox disc image")

    def close(self):
        self.f.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def _read(self, sector, size):
        self.f.seek(self.base + sector * SECTOR)
        data = self.f.read(size)
        if len(data) != size:
            raise XisoError(f"{self.path} ends early: is the image complete?")
        return data

    def directory(self, sector, size):
        pass
        if size == 0:
            return []
        table = self._read(sector, size)
        found, pending, seen = [], [0], set()
        while pending:
            offset = pending.pop()
            if offset in seen or offset + 14 > len(table):
                continue
            seen.add(offset)
            left, right, start, length, attributes, name_length = struct.unpack_from("<HHIIBB", table, offset)
            if left == EMPTY and right == EMPTY and start == 0xFFFFFFFF:
                continue
            name = table[offset + 14:offset + 14 + name_length].decode("latin-1")
            if name:
                found.append((name, bool(attributes & ATTRIBUTE_DIRECTORY), start, length))
            for link in (left, right):
                if link not in (0, EMPTY):
                    pending.append(link * 4)
        return sorted(found, key=lambda e: e[0].lower())

    def root(self):
        return self.directory(self.root_sector, self.root_size)

    def walk(self, sector=None, size=None, prefix=""):
        pass
        if sector is None:
            sector, size = self.root_sector, self.root_size
        for name, is_dir, start, length in self.directory(sector, size):
            rel = prefix + name
            if is_dir:
                yield from self.walk(start, length, rel + "/")
            else:
                yield rel, start, length

    def extract(self, out, skip=lambda rel: False, progress=None):
        pass

        files = [f for f in self.walk() if not skip(f[0])]
        total = sum(f[2] for f in files) or 1
        done = 0
        for rel, start, length in files:
            dst = os.path.join(out, *rel.split("/"))
            if not (os.path.isfile(dst) and os.path.getsize(dst) == length):
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                self.f.seek(self.base + start * SECTOR)
                remaining = length
                with open(dst + ".part", "wb") as w:
                    while remaining:
                        chunk = self.f.read(min(remaining, 1 << 22))
                        if not chunk:
                            raise XisoError(f"{self.path} ends early while reading {rel}")
                        w.write(chunk)
                        remaining -= len(chunk)
                os.replace(dst + ".part", dst)
            done += length
            if progress:
                progress(done, total, rel)
        return len(files)


def is_xbox_image(path):
    try:
        with Xiso(path):
            return True
    except (XisoError, OSError):
        return False


def root_names(path):
    pass
    try:
        with Xiso(path) as x:
            return {name.lower() for name, _, _, _ in x.root()}
    except (XisoError, OSError):
        return set()
