import struct
import sys

MAGIC_FILE = 0x43505A42
VERSION_FILE = 1
MAGIC_LIST = 0x4C505A42
VERSION_LIST = 1
HEADER_FILE = 24
HEADER_LIST = 16


def read_list(path):
    pass
    data = open(path, "rb").read()
    if len(data) >= HEADER_FILE:
        magic, version, bytes_list, bytes_cache = struct.unpack_from("<IIQQ", data, 0)
        if magic == MAGIC_FILE:
            if version != VERSION_FILE or HEADER_FILE + bytes_list + bytes_cache != len(data):
                raise SystemExit(f"{path}: pipelines file of another version or damaged")
            data = data[HEADER_FILE:HEADER_FILE + bytes_list]
            if not data:
                return None, []
    if len(data) < HEADER_LIST:
        raise SystemExit(f"{path}: not a list")
    magic, version, size, count = struct.unpack_from("<IIII", data, 0)
    if magic != MAGIC_LIST or version != VERSION_LIST or len(data) != HEADER_LIST + size * count:
        raise SystemExit(f"{path}: list of another version or damaged")
    return size, [data[HEADER_LIST + i * size:HEADER_LIST + (i + 1) * size] for i in range(count)]


def write_list(path, size, records):
    with open(path, "wb") as f:
        f.write(struct.pack("<IIII", MAGIC_LIST, VERSION_LIST, size, len(records)))
        for r in records:
            f.write(r)
    print(f"{path}: {len(records)} pipelines")


def key_of(record):


    return record


def main(argv):
    if len(argv) < 3:
        raise SystemExit(__doc__)
    command = argv[1]
    if command == "info":
        size, records = read_list(argv[2])
        print(f"{argv[2]}: {len(records)} pipelines, {size} bytes each")
    elif command == "extract" and len(argv) == 4:
        size, records = read_list(argv[2])
        if size is None:
            raise SystemExit(f"{argv[2]}: no list in it")
        write_list(argv[3], size, records)
    elif command == "merge" and len(argv) >= 4:
        size, seen, out = None, set(), []
        for path in argv[3:]:
            s, records = read_list(path)
            if s is None:
                continue
            if size is not None and s != size:
                raise SystemExit(f"{path}: records of {s} bytes, not {size}")
            size = s
            for r in records:
                if key_of(r) not in seen:
                    seen.add(key_of(r))
                    out.append(r)
        write_list(argv[2], size or 0, out)
    elif command == "minus" and len(argv) == 5:
        size, a = read_list(argv[3])
        _, b = read_list(argv[4])
        known = {key_of(r) for r in b}
        write_list(argv[2], size or 0, [r for r in a if key_of(r) not in known])
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main(sys.argv)
