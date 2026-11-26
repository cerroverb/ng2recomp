#!/usr/bin/env python3
"""Print this game's downloadable content packages under a folder, at any
depth (STFS LIVE/PIRS/CON packages of content type 2 for title 544307D5),
one path per line; given a package, that one if it is. Only files that can
be packages are opened (no extension, 40 KB or more), and system folders are
passed over, so a whole drive is quick to search.

usage: find_dlc.py <folder | package>
"""
import os
import struct
import sys

TITLE_ID = 0x544307D5
MARKETPLACE_CONTENT = 2
# System folders at the top of a drive (/run and /var stay: USB drives mount
# under /run/media, and some systems keep home in /var/home), and anywhere.
SKIP_TOP = {"proc", "sys", "dev", "usr", "lib", "lib64", "bin", "sbin", "etc", "boot", "snap",
            "windows", "program files", "program files (x86)", "programdata"}
SKIP = {".cache", ".git", "node_modules", "$recycle.bin", "system volume information"}


def is_dlc(path):
    try:
        with open(path, "rb") as f:
            h = f.read(0x364)
    except OSError:
        return False
    return (len(h) == 0x364 and h[:4] in (b"LIVE", b"PIRS", b"CON ")
            and struct.unpack_from(">I", h, 0x344)[0] == MARKETPLACE_CONTENT
            and struct.unpack_from(">I", h, 0x360)[0] == TITLE_ID)


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    top = os.path.realpath(sys.argv[1])
    if os.path.isfile(top):  # one package (RecompLauncher's setup)
        if is_dlc(top):
            print(top)
        return
    for root, dirs, files in os.walk(top):
        drive = os.path.dirname(root) == root or os.path.ismount(root)
        dirs[:] = [d for d in dirs if d.lower() not in SKIP and not (drive and d.lower() in SKIP_TOP)]
        for name in sorted(files):
            path = os.path.join(root, name)
            if "." in name or not os.path.isfile(path):
                continue
            try:
                if os.path.getsize(path) < 0xA000:
                    continue
            except OSError:
                continue
            if is_dlc(path):
                print(path)


if __name__ == "__main__":
    main()
