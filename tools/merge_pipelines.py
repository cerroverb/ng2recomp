#!/usr/bin/env python3
"""Merge pipeline lists (ng2-pipelines.bin from test runs) into the packaged seed.

usage: merge_pipelines.py <out.bin> <list.bin>...

Each list is a 12-byte header ("NG2P" magic, version, record size) and fixed-size
records (src/gpu/video.cpp, PipelineKey). Records are kept once each, in the order
first seen; a list from another version or record size is refused.
"""
import struct
import sys

MAGIC, VERSION = 0x4E473250, 1
RECORD_SIZE = 96  # sizeof(PipelineKey), src/gpu/video.cpp


def records(path):
    data = open(path, "rb").read()
    if len(data) < 12:
        return None, []
    magic, version, size = struct.unpack_from("<III", data)
    if magic != MAGIC or version != VERSION:
        raise SystemExit(f"{path}: not a pipeline list of version {VERSION}")
    if size != RECORD_SIZE:
        raise SystemExit(f"{path}: records of {size} bytes, not {RECORD_SIZE}")
    body = data[12:]
    return size, [body[i:i + size] for i in range(0, len(body) - size + 1, size)]


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    out, lists = sys.argv[1], sys.argv[2:]
    seen, kept, size = set(), [], None
    for path in lists:
        s, recs = records(path)
        if s is None:
            continue
        if size is None:
            size = s
        elif s != size:
            raise SystemExit(f"{path}: records of {s} bytes, not {size}")
        for r in recs:
            if r not in seen:
                seen.add(r)
                kept.append(r)
    if size is None:
        raise SystemExit("no records")
    with open(out, "wb") as f:
        f.write(struct.pack("<III", MAGIC, VERSION, size))
        f.writelines(kept)
    print(f"{out}: {len(kept)} pipelines from {len(lists)} lists")


if __name__ == "__main__":
    main()
