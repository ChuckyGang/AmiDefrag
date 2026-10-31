#!/usr/bin/env python3
"""
set_fs_stack.py - set the handler stack size of the filesystem entries in an
RDB image (FileSysHeaderBlock fhb_StackSize + the FSEB_STACKSIZE patch flag).

A handler started from an RDB entry without that patch flag runs on the OS
default stack (4000 bytes on OS 3.2), which is too small for the
SmartFilesystem defragmenter.  Run this with the image NOT in use (quit the
emulator first), then reboot: the handler is started with the new stack.

  test/set_fs_stack.py IMAGE [--stack 32768] [--dostype SFS0]
"""
import argparse
import struct
import sys

FSEB_STACKSIZE = 0x10


def be32(b, off):
    return struct.unpack_from(">I", b, off)[0]


def checksum_fix(blk, summed_longs):
    total = sum(struct.unpack_from(f">{summed_longs}I", blk, 0)) - be32(blk, 8)
    struct.pack_into(">I", blk, 8, (-total) & 0xFFFFFFFF)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("--stack", type=int, default=32768)
    ap.add_argument("--dostype", default=None, help="only entries with this 4-char type, e.g. SFS0")
    ap.add_argument("--block-size", type=int, default=512)
    a = ap.parse_args()
    bs = a.block_size
    want = None
    if a.dostype:
        t = a.dostype.encode("latin-1")
        want = (t[0] << 24) | (t[1] << 16) | (t[2] << 8) | (t[3] if len(t) > 3 and not t[3:4].isdigit() else int(a.dostype[3]))
    with open(a.image, "r+b") as f:
        rdsk = None
        for i in range(16):
            f.seek(i * bs)
            b = f.read(bs)
            if b[:4] == b"RDSK":
                rdsk = i
                break
        if rdsk is None:
            sys.exit("no RDSK block in the first 16 blocks")
        f.seek(rdsk * bs)
        rd = f.read(bs)
        fs_blk = be32(rd, 32)          # rdb_FileSysHeaderList
        changed = 0
        while fs_blk != 0xFFFFFFFF:
            f.seek(fs_blk * bs)
            blk = bytearray(f.read(bs))
            if blk[:4] != b"FSHD":
                sys.exit(f"block {fs_blk}: not a FSHD block")
            dostype = be32(blk, 32)
            flags = be32(blk, 40)
            old_stack = be32(blk, 60)
            name = struct.pack(">I", dostype)
            label = f"{name[:3].decode('latin-1')}\\{name[3]}"
            if want is None or dostype == want:
                struct.pack_into(">I", blk, 40, flags | FSEB_STACKSIZE)
                struct.pack_into(">I", blk, 60, a.stack)
                checksum_fix(blk, be32(blk, 4))
                f.seek(fs_blk * bs)
                f.write(blk)
                changed += 1
                print(f"block {fs_blk}: {label} patch_flags 0x{flags:x} -> 0x{flags | FSEB_STACKSIZE:x}, "
                      f"stack {old_stack} -> {a.stack}")
            else:
                print(f"block {fs_blk}: {label} left alone (flags 0x{flags:x}, stack {old_stack})")
            fs_blk = be32(blk, 16)     # fhb_Next
        print(f"{changed} entr{'y' if changed == 1 else 'ies'} changed")


if __name__ == "__main__":
    main()
