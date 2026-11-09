#!/usr/bin/env python3
# An aarch64 sysroot for the cross build (cmake/toolchains/linux-aarch64-clang.cmake) out of
# Ubuntu 24.04's arm64 packages, without root: the packages and everything they depend on are
# read from the archive's package lists, downloaded, checked and unpacked with bsdtar. Ubuntu
# 24.04 is what the ReXGlue SDK builds its own linux-arm64 release on (ubuntu-24.04-arm in its
# .github/workflows/_build-platform.yaml), so the game needs no glibc newer than 2.39.
#
#   python3 -I packaging/aarch64/fetch-sysroot.py <download dir> <sysroot> <package>...
#
# The chain of checks: each suite's InRelease is verified with gpgv against the Ubuntu archive
# keyring (Arch's ubuntu-keyring package), the package lists against the SHA-256 sums in it,
# and every package against the SHA-256 sum in the lists.
import hashlib
import lzma
import os
import re
import subprocess
import sys
import urllib.request

ARCHIVE = "https://ports.ubuntu.com/ubuntu-ports"
# Later suites replace the packages of earlier ones (noble-updates carries the security fixes too).
SUITES = ["noble", "noble-security", "noble-updates"]
COMPONENTS = ["main", "universe"]
KEYRING = "/usr/share/keyrings/ubuntu-archive-keyring.gpg"


def fetch(url, path):
    if not os.path.exists(path):
        urllib.request.urlretrieve(url, path + ".part")
        os.rename(path + ".part", path)
    return path


def sha256(path):
    with open(path, "rb") as f:
        return hashlib.file_digest(f, "sha256").hexdigest()


def package_lists(download_dir, suite):
    release = fetch(f"{ARCHIVE}/dists/{suite}/InRelease", os.path.join(download_dir, f"InRelease-{suite}"))
    signed = subprocess.run(["gpgv", "--quiet", "--keyring", KEYRING, "--output", "-", release],
                            check=True, capture_output=True, text=True).stdout
    sums = {name: digest for digest, name in re.findall(r"^ ([0-9a-f]{64}) +\d+ (\S+)$", signed, re.M)}
    for component in COMPONENTS:
        name = f"{component}/binary-arm64/Packages.xz"
        path = fetch(f"{ARCHIVE}/dists/{suite}/{name}",
                     os.path.join(download_dir, f"Packages-{suite}-{component}.xz"))
        if sha256(path) != sums[name]:
            os.remove(path)
            sys.exit(f"{name} of {suite} does not match its InRelease")
        yield lzma.open(path, "rt").read()


def read_packages(text):
    for stanza in text.split("\n\n"):
        fields, key = {}, None
        for line in stanza.splitlines():
            if line.startswith((" ", "\t")):
                continue
            key, _, value = line.partition(":")
            fields[key] = value.strip()
        if "Package" in fields:
            yield fields


def alternatives(dependencies):
    # "a (>= 1) | b, c:any" -> [["a", "b"], ["c"]]
    return [[re.split(r"[ (:]", choice.strip(), maxsplit=1)[0] for choice in group.split("|")]
            for group in dependencies.split(",") if group.strip()]


def main():
    download_dir, sysroot, wanted = sys.argv[1], sys.argv[2], sys.argv[3:]
    os.makedirs(download_dir, exist_ok=True)
    packages = {}
    for suite in SUITES:
        for text in package_lists(download_dir, suite):
            for fields in read_packages(text):
                packages[fields["Package"]] = fields
    providers = {}
    for name, fields in packages.items():
        for group in alternatives(fields.get("Provides", "")):
            providers.setdefault(group[0], name)

    def resolve(name):
        return name if name in packages else providers.get(name)

    chosen, queue = {}, [[name] for name in wanted]
    while queue:
        group = queue.pop()
        if any(resolve(choice) in chosen for choice in group):
            continue
        name = next((resolve(choice) for choice in group if resolve(choice)), None)
        if name is None:
            continue
        fields = chosen[name] = packages[name]
        queue.extend(alternatives(fields.get("Pre-Depends", "")))
        queue.extend(alternatives(fields.get("Depends", "")))

    # Ubuntu's merged /usr, made first so packages that still ship files in /lib land in /usr/lib
    # (-P: bsdtar otherwise refuses to unpack through a link).
    os.makedirs(sysroot, exist_ok=True)
    for top in ("bin", "sbin", "lib"):
        os.makedirs(os.path.join(sysroot, "usr", top), exist_ok=True)
        if not os.path.lexists(os.path.join(sysroot, top)):
            os.symlink(f"usr/{top}", os.path.join(sysroot, top))
    for name in sorted(chosen):
        fields = chosen[name]
        path = fetch(f"{ARCHIVE}/{fields['Filename']}",
                     os.path.join(download_dir, os.path.basename(fields["Filename"])))
        if sha256(path) != fields["SHA256"]:
            os.remove(path)
            sys.exit(f"{path} does not match its package list")
        members = subprocess.run(["bsdtar", "-tf", path], check=True, capture_output=True,
                                 text=True).stdout.split()
        data = next(member for member in members if member.startswith("data.tar"))
        unpack = subprocess.Popen(["bsdtar", "-xPf", "-", "-C", sysroot], stdin=subprocess.PIPE)
        subprocess.run(["bsdtar", "-xOf", path, data], check=True, stdout=unpack.stdin)
        unpack.stdin.close()
        if unpack.wait():
            sys.exit(f"{path} did not unpack")
        print(name, fields["Version"])

    # Absolute links made relative, so they stay inside the sysroot.
    for root, dirs, files in os.walk(sysroot):
        for entry in dirs + files:
            link = os.path.join(root, entry)
            if os.path.islink(link) and os.readlink(link).startswith("/"):
                target = os.path.join(sysroot, os.readlink(link).lstrip("/"))
                os.remove(link)
                os.symlink(os.path.relpath(target, root), link)


main()
