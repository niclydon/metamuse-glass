#!/usr/bin/env python3
"""Pack a rootfs directory into an SVR4 newc (070701) cpio.gz archive for Linux initramfs.

All entries are normalized to uid=0, gid=0, mtime=0 for reproducibility, and
character device nodes /dev/console (c 5 1) and /dev/null (c 1 3) are injected
automatically (required before /init mounts tmpfs/devtmpfs on /dev on the stock
XE24 3.4.94 kernel) without requiring root/mknod on the build host.
"""
import gzip
import os
import stat
import sys


def pad4(n: int) -> bytes:
    rem = n % 4
    return b"\x00" * ((4 - rem) % 4)


def emit_entry(out, name: str, mode: int, nlink: int, filesize: int,
               devmajor: int, devminor: int, rdevmajor: int, rdevminor: int,
               ino: int, data: bytes = b""):
    name_bytes = name.encode("utf-8") + b"\x00"
    hdr = (
        f"070701"
        f"{ino:08x}"
        f"{mode:08x}"
        f"{0:08x}"          # uid = 0 (root)
        f"{0:08x}"          # gid = 0 (root)
        f"{nlink:08x}"
        f"{0:08x}"          # mtime = 0 (reproducible)
        f"{filesize:08x}"
        f"{devmajor:08x}"
        f"{devminor:08x}"
        f"{rdevmajor:08x}"
        f"{rdevminor:08x}"
        f"{len(name_bytes):08x}"
        f"{0:08x}"          # check = 0
    ).encode("ascii")
    assert len(hdr) == 110
    out.write(hdr)
    out.write(name_bytes)
    out.write(pad4(110 + len(name_bytes)))
    if data:
        out.write(data)
        out.write(pad4(len(data)))


def pack_tree(root_dir: str, out_gz_path: str):
    ino = 1
    entries_seen = set()

    with open(out_gz_path, "wb") as raw_f:
        with gzip.GzipFile(filename="", mode="wb", fileobj=raw_f, compresslevel=9, mtime=0) as gz:
            # Walk deterministically
            for dirpath, dirnames, filenames in os.walk(root_dir, followlinks=False):
                dirnames.sort()
                filenames.sort()
                rel_dir = os.path.relpath(dirpath, root_dir)
                if rel_dir == ".":
                    rel_dir = "."
                    emit_entry(gz, ".", stat.S_IFDIR | 0o755, 2, 0, 0, 0, 0, 0, ino)
                    entries_seen.add(".")
                    ino += 1
                else:
                    st = os.lstat(dirpath)
                    mode = stat.S_IFDIR | (stat.S_IMODE(st.st_mode) or 0o755)
                    emit_entry(gz, rel_dir, mode, 2, 0, 0, 0, 0, 0, ino)
                    entries_seen.add(rel_dir)
                    ino += 1

                for fn in filenames:
                    full = os.path.join(dirpath, fn)
                    rel = fn if rel_dir == "." else f"{rel_dir}/{fn}"
                    if rel in ("dev/console", "dev/null"):
                        continue
                    st = os.lstat(full)
                    if stat.S_ISLNK(st.st_mode):
                        target = os.readlink(full).encode("utf-8")
                        emit_entry(gz, rel, stat.S_IFLNK | 0o777, 1, len(target),
                                   0, 0, 0, 0, ino, target)
                    elif stat.S_ISREG(st.st_mode):
                        with open(full, "rb") as f:
                            data = f.read()
                        mode = stat.S_IFREG | stat.S_IMODE(st.st_mode)
                        emit_entry(gz, rel, mode, 1, len(data),
                                   0, 0, 0, 0, ino, data)
                    elif stat.S_ISFIFO(st.st_mode):
                        mode = stat.S_IFIFO | stat.S_IMODE(st.st_mode)
                        emit_entry(gz, rel, mode, 1, 0, 0, 0, 0, 0, ino)
                    else:
                        continue
                    entries_seen.add(rel)
                    ino += 1

            # Ensure dev directory and required early character nodes exist
            if "dev" not in entries_seen:
                emit_entry(gz, "dev", stat.S_IFDIR | 0o755, 2, 0, 0, 0, 0, 0, ino)
                ino += 1
            emit_entry(gz, "dev/console", stat.S_IFCHR | 0o600, 1, 0, 0, 0, 5, 1, ino)
            ino += 1
            emit_entry(gz, "dev/null", stat.S_IFCHR | 0o666, 1, 0, 0, 0, 1, 3, ino)
            ino += 1

            # TRAILER!!!
            emit_entry(gz, "TRAILER!!!", 0, 1, 0, 0, 0, 0, 0, 0)


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <rootfs_dir> <output.cpio.gz>", file=sys.stderr)
        sys.exit(2)
    pack_tree(sys.argv[1], sys.argv[2])
