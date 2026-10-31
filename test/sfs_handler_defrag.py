#!/usr/bin/env python3
"""
sfs_handler_defrag.py - drive the SmartFilesystem handler's defragmenter on
an image through amifuse, the way AmiDefrag's SFS engine does on the Amiga.

Checks, in order:
  1. AmiDefrag's own stats of the partition before (host/amidefrag).
  2. md5 of every file through a read-only amifuse mount (skip: --no-md5).
  3. Runs the real handler (the one embedded in the RDB, or --driver) under
     amifuse's 68k runtime, sends ACTION_SFS_QUERY / DEFRAGMENT_INIT /
     DEFRAGMENT_STEP exactly like src/sfs_defrag.c, replays every reported
     MOVE on a copy of the bitmap read with ACTION_SFS_READ_BITMAP before
     the run and compares it with the bitmap read after the run.  That is
     the check that the MOVE reports describe what really happened, which
     the live map in the GUI relies on.
  4. Stats after, and the md5s again.

Run with amifuse's Python (the shebang of ~/.local/bin/amifuse):
  /home/john/.local/share/pipx/venvs/amifuse/bin/python test/sfs_handler_defrag.py \
      test/work/sfs64.hdf --partition FRG2 [--max-steps N] [--no-md5]
"""
import argparse
import hashlib
import os
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent

PKT_BASE = 0xF00000
ACTION_SFS_QUERY = PKT_BASE + 1
ACTION_SFS_READ_BITMAP = PKT_BASE + 201
ACTION_SFS_DEFRAGMENT_INIT = PKT_BASE + 301
ACTION_SFS_DEFRAGMENT_STEP = PKT_BASE + 302
ACTION_FLUSH = 27
TAG_USER = 0x80000000
ASQ_BLOCK_SIZE = TAG_USER + 1001
ASQ_TOTAL_BLOCKS = TAG_USER + 1002
ASQ_VERSION = TAG_USER + 5001
ID_MOVE = 0x4D4F5645
ID_DONE = 0x444F4E45
STEP_LONGS = 1000
STEP_SLACK = 20


def log(msg):
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def amidefrag_stats(img, part):
    exe = ROOT / "host" / "amidefrag"
    out = subprocess.run([str(exe), f"IMAGE={img}", f"PART={part}"], capture_output=True, text=True).stdout
    return out.strip().splitlines()[-1] if out.strip() else "(no output)"


def md5_tree(img, part, driver):
    mnt = HERE / "work" / "mnt_sfscheck"
    mnt.mkdir(parents=True, exist_ok=True)
    cmd = ["amifuse", "mount", str(img), "--partition", part, "--mountpoint", str(mnt)]
    if driver:
        cmd += ["--driver", str(driver)]
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(120):
        if os.path.ismount(mnt):
            break
        time.sleep(0.5)
    else:
        proc.kill()
        raise SystemExit("amifuse mount did not come up")
    sums = {}
    try:
        for dp, dn, fn in os.walk(mnt):
            dn.sort()
            for f in sorted(fn):
                p = Path(dp) / f
                try:
                    h = hashlib.md5()
                    with open(p, "rb") as fh:
                        while True:
                            b = fh.read(1 << 20)
                            if not b:
                                break
                            h.update(b)
                    sums[str(p.relative_to(mnt))] = h.hexdigest()
                except OSError as e:
                    sums[str(p.relative_to(mnt))] = f"ERR:{e.errno}"
    finally:
        subprocess.run(["amifuse", "unmount", str(mnt)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            proc.wait(timeout=60)
        except subprocess.TimeoutExpired:
            proc.kill()
    return sums


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("--partition", required=True)
    ap.add_argument("--driver", default=None, help="handler binary (default: the one embedded in the RDB)")
    ap.add_argument("--max-steps", type=int, default=0, help="stop after N steps (0 = run to DONE)")
    ap.add_argument("--no-md5", action="store_true")
    ap.add_argument("--debug", action="store_true")
    args = ap.parse_args()
    img = Path(args.image).resolve()

    import amifuse.fuse_fs as ff
    orig = ff.HandlerBridge._run_until_replies

    def patched(self, max_iters=4000, **kw):
        return orig(self, max_iters=max_iters, **kw)
    ff.HandlerBridge._run_until_replies = patched

    log(f"before: {amidefrag_stats(img, args.partition)}")
    sums0 = None
    if not args.no_md5:
        sums0 = md5_tree(img, args.partition, args.driver)
        log(f"md5 of {len(sums0)} files taken")

    driver = args.driver
    temp_driver = None
    if driver is None:
        res = ff.extract_embedded_driver(img, None, args.partition)
        if res is None:
            raise SystemExit("no embedded driver, use --driver")
        temp_driver, _, _ = res
        driver = temp_driver
    bridge = ff.HandlerBridge(img, Path(driver), read_only=False, debug=args.debug, partition=args.partition)
    mem = bridge.mem
    alloc = bridge.vh.alloc

    def dopkt(typ, pargs):
        with bridge._lock:
            bridge.launcher.send_packet(bridge.state, typ, pargs)
            replies = bridge._run_until_replies()
        if not replies:
            raise RuntimeError(f"no reply to packet 0x{typ:x}")
        _, _, res1, res2 = replies[-1]
        return res1, res2

    try:
        # ---- query ----
        tagmem = alloc.alloc_memory(4 * 8, label="sfs_tags")
        for i, tag in enumerate((ASQ_VERSION, ASQ_TOTAL_BLOCKS, ASQ_BLOCK_SIZE, 0)):
            mem.w32(tagmem.addr + i * 8, tag)
            mem.w32(tagmem.addr + i * 8 + 4, 0)
        res1, res2 = dopkt(ACTION_SFS_QUERY, [tagmem.addr])
        if res1 == 0:
            raise SystemExit(f"ACTION_SFS_QUERY failed, res2={res2}")
        version = mem.r32(tagmem.addr + 4)
        total = mem.r32(tagmem.addr + 12)
        bsize = mem.r32(tagmem.addr + 20)
        log(f"handler SmartFilesystem {version >> 16}.{version & 0xFFFF}, {total} blocks of {bsize} bytes")

        # ---- bitmap before ----
        bmbytes = total // 8 + 32
        bmmem = alloc.alloc_memory(bmbytes, label="sfs_bitmap")
        mem.w_block(bmmem.addr, b"\x00" * bmbytes)
        res1, res2 = dopkt(ACTION_SFS_READ_BITMAP, [bmmem.addr, 0, total])
        if res1 == 0:
            raise SystemExit(f"ACTION_SFS_READ_BITMAP failed, res2={res2}")
        bm = bytearray(mem.r_block(bmmem.addr, bmbytes))
        free0 = sum(bin(b).count("1") for b in bm[: (total + 7) // 8])
        log(f"bitmap before: {free0} free blocks")

        def bm_set(first, n, free):
            for b in range(first, first + n):
                byte, bit = b >> 3, 7 - (b & 7)
                if free:
                    bm[byte] |= 1 << bit
                else:
                    bm[byte] &= ~(1 << bit) & 0xFF

        # ---- defragment ----
        res1, res2 = dopkt(ACTION_SFS_DEFRAGMENT_INIT, [])
        if res1 == 0:
            raise SystemExit(f"ACTION_SFS_DEFRAGMENT_INIT failed, res2={res2}")
        stepmem = alloc.alloc_memory((STEP_LONGS + STEP_SLACK) * 4, label="sfs_steps")
        steps = moves = moved = 0
        empty = 0
        bad = 0
        done = False
        t0 = time.time()
        last = t0
        while not done:
            if args.max_steps and steps >= args.max_steps:
                log(f"stopping after {steps} steps as asked")
                break
            mem.w32(stepmem.addr, 0)
            res1, res2 = dopkt(ACTION_SFS_DEFRAGMENT_STEP, [stepmem.addr, STEP_LONGS])
            steps += 1
            if res1 == 0:
                log(f"ACTION_SFS_DEFRAGMENT_STEP failed at step {steps}, res2={res2}")
                break
            buf = mem.r_block(stepmem.addr, (STEP_LONGS + STEP_SLACK) * 4)
            i = 0
            nmoves_here = 0
            while i + 8 <= len(buf):
                sid = int.from_bytes(buf[i:i + 4], "big")
                ln = int.from_bytes(buf[i + 4:i + 8], "big")
                if sid == 0:
                    break
                if sid == ID_MOVE and ln == 3:
                    n = int.from_bytes(buf[i + 8:i + 12], "big")
                    src = int.from_bytes(buf[i + 12:i + 16], "big")
                    dst = int.from_bytes(buf[i + 16:i + 20], "big")
                    if src + n > total or dst + n > total or n == 0:
                        bad += 1
                    else:
                        # the handler reports after the fact: dst is in use, src free
                        if any(not (bm[b >> 3] >> (7 - (b & 7))) & 1 for b in range(dst, dst + n)):
                            bad += 1
                        bm_set(src, n, True)
                        bm_set(dst, n, False)
                        moves += 1
                        moved += n
                        nmoves_here += 1
                elif sid == ID_DONE:
                    done = True
                i += 8 + 4 * ln
            if nmoves_here == 0 and not done:
                empty += 1
            now = time.time()
            if now - last >= 10:
                last = now
                log(f"{steps} steps, {moves} moves, {moved} blocks ({moved * bsize // 1048576} MB), {empty} empty steps, {bad} odd moves")
        secs = time.time() - t0
        log(f"{'DONE' if done else 'stopped'}: {steps} steps, {moves} moves, {moved} blocks "
            f"({moved * bsize // 1048576} MB) in {secs:.0f} s, {empty} empty steps, {bad} odd moves")

        # ---- bitmap after vs replay ----
        mem.w_block(bmmem.addr, b"\x00" * bmbytes)
        res1, res2 = dopkt(ACTION_SFS_READ_BITMAP, [bmmem.addr, 0, total])
        after = bytes(mem.r_block(bmmem.addr, bmbytes))
        nb = (total + 7) // 8
        diff = sum(bin(a ^ b).count("1") for a, b in zip(after[:nb], bm[:nb]))
        free1 = sum(bin(b).count("1") for b in after[:nb])
        log(f"bitmap after: {free1} free blocks; replayed MOVEs differ from it in {diff} block(s)")

        dopkt(ACTION_FLUSH, [])
    finally:
        ff._cleanup_bridge(bridge, Path(temp_driver) if temp_driver else None)

    log(f"after:  {amidefrag_stats(img, args.partition)}")
    if sums0 is not None:
        sums1 = md5_tree(img, args.partition, args.driver)
        missing = [k for k in sums0 if k not in sums1]
        changed = [k for k in sums0 if k in sums1 and sums0[k] != sums1[k]]
        log(f"md5 check: {len(sums1)} files, {len(missing)} missing, {len(changed)} changed")
        for k in (missing + changed)[:10]:
            print("   ", k, sums0.get(k), "->", sums1.get(k))
        if missing or changed:
            sys.exit(1)
    if diff:
        sys.exit(2)


if __name__ == "__main__":
    main()
