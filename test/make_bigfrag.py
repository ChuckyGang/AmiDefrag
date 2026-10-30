#!/usr/bin/env python3
"""
make_bigfrag.py - build a large, heavily fragmented FFS / PFS3 / SFS test image
filled with real files, using the REAL filesystem handlers (via amifuse).

    test/make_bigfrag.py --fs ffs  [--size-mb 1024] [--out test/frag1g_ffs.hdf]
    test/make_bigfrag.py --fs pfs3 ...
    test/make_bigfrag.py --fs sfs  ...

How the fragmentation is produced ("sieve" method):
  1. AmiPart creates an RDB image with one partition (FRG0/FRG1/FRG2), the
     handler embedded in the RDB, and formats it.
  2. The partition is mounted read-write through amifuse with the real
     handler (FastFileSystem 3.2 / pfs3aio / SmartFilesystem).
  3. A single big "tail" filler (TAIL_FRACTION of the disk) is written so one
     large contiguous free run exists at the end.
  4. The rest of the disk is filled to the brim with alternating small filler
     files: A (future holes, 2..32 blocks) and B (future gaps, 1..8 blocks).
  5. All A fillers are deleted -> free space is a sieve of small holes.
  6. Real files (default: /mnt/share/Amiga/Demos, MODULES, then ~/UAE/WB323; directory
     structure preserved, names cut to 30 chars) are copied in until the
     disk is full or --fill is reached. Every file larger than a hole is
     split across many holes by the handler's own allocator.
  7. The B fillers and the tail filler are deleted -> the real files stay
     shredded, with small free gaps between the pieces plus one big free
     run at the end.
  8. Unmount, then host/amidefrag reports the result.

Needs: AmiPart host CLI (../AmiPart/host/amipart or AMIPART=), amifuse 0.6+
on PATH, handler binaries (default ~/UAE/WB323/L/...). Linux only (FUSE).
Rough speed through the emulated handlers: ~2 MB/s and ~100 files/s, i.e.
around 30-45 minutes per 1 GB image. Run the three in parallel.
"""
import argparse, os, random, shutil, subprocess, sys, time, errno

HERE = os.path.dirname(os.path.abspath(__file__))
WORK = os.path.join(HERE, "work")
CYL_BYTES = 16 * 63 * 512                 # 16 heads x 63 sectors
BLOCK = 512
TAIL_FRACTION = 0.12
FILES_PER_DIR = 200

FS = {
    "ffs":  dict(dostype="DOS3", part="FRG0", vol="FragFFS",
                 handler=os.path.expanduser("~/UAE/WB323/L/FastFileSystem"), embed=False),
    "pfs3": dict(dostype="PDS3", part="FRG1", vol="FragPFS",
                 handler=os.path.expanduser("~/UAE/WB323/L/pfs3aio"), embed=True),
    "sfs":  dict(dostype="SFS0", part="FRG2", vol="FragSFS",
                 handler=os.path.expanduser("~/UAE/WB323/L/SmartFilesystem"), embed=True),
}

T0 = time.time()
def log(msg):
    print(f"[{time.time()-T0:7.1f}s] {msg}", flush=True)

def sh(cmd, check=True):
    r = subprocess.run(cmd, shell=True, capture_output=True, text=True)
    if check and r.returncode != 0:
        sys.exit(f"command failed ({r.returncode}): {cmd}\n{r.stdout}{r.stderr}")
    return r

def make_image(out, cyls, fs, amipart):
    f = FS[fs]
    os.makedirs(WORK, exist_ok=True)
    if os.path.exists(out): os.remove(out)
    with open(out, "wb") as fh:
        fh.truncate(cyls * CYL_BYTES)
    script = os.path.join(WORK, f"bigfrag_{fs}.script")
    lines = [f"OPEN FILE {out}", "INIT NEW"]
    if f["embed"]:
        lines.append(f"ADDFS TYPE={f['dostype']} FILE={f['handler']}")
    lines.append(f"ADDPART NAME={f['part']} LOW=2 HIGH={cyls-1} TYPE={f['dostype']} VOLNAME={f['vol']}")
    lines += ["WRITE", "CLOSE"]
    with open(script, "w") as fh:
        fh.write("\n".join(lines) + "\n")
    r = sh(f'"{amipart}" SCRIPT "{script}" FORCE')
    log("AmiPart: " + " | ".join(l for l in r.stdout.splitlines() if l.strip())[-300:])

def mount(out, fs, mnt):
    f = FS[fs]
    os.makedirs(mnt, exist_ok=True)
    sh(f'amifuse mount "{out}" --partition {f["part"]} --driver "{f["handler"]}" '
       f'--mountpoint "{mnt}" --write')
    for _ in range(60):
        if os.path.ismount(mnt): return
        time.sleep(0.5)
    sys.exit("amifuse mount did not appear")

def unmount(mnt):
    for _ in range(10):
        r = sh(f'amifuse unmount "{mnt}"', check=False)
        time.sleep(1)
        if not os.path.ismount(mnt): return
    log("WARNING: could not unmount " + mnt)

class Full(Exception): pass

def write_file(path, data):
    """Write data; raise Full when the volume refuses (ENOSPC or the EIO
    amifuse hands back for a full disk). Partial files are removed."""
    try:
        with open(path, "wb") as fh:
            fh.write(data)
    except OSError as e:
        try: os.remove(path)
        except OSError: pass
        if e.errno in (errno.ENOSPC, errno.EIO, errno.EDQUOT):
            raise Full()
        raise

def copy_file(src, dst):
    try:
        with open(src, "rb") as s, open(dst, "wb") as d:
            shutil.copyfileobj(s, d, 256 * 1024)
    except OSError as e:
        try: os.remove(dst)
        except OSError: pass
        if e.errno in (errno.ENOSPC, errno.EIO, errno.EDQUOT):
            raise Full()
        raise

def ami_name(name, taken):
    """Cut to 30 chars, strip characters AmigaDOS cannot take, keep unique."""
    s = "".join(c if (32 < ord(c) < 127 or 160 <= ord(c) < 256) and c not in ':/' else '_'
                for c in name)
    s = s.strip(". ") or "file"
    base = s[:30]
    cand, n = base, 1
    while cand.lower() in taken:
        suf = f"~{n}"
        cand = base[:30 - len(suf)] + suf
        n += 1
    taken.add(cand.lower())
    return cand

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--fs", choices=FS.keys(), required=True)
    ap.add_argument("--size-mb", type=int, default=1024, help="partition size in MB (default 1024)")
    ap.add_argument("--out", help="image path (default test/frag<size>_<fs>.hdf)")
    ap.add_argument("--fill", type=float, default=0.70, help="stop copying real files at this fraction of the disk")
    ap.add_argument("--src", action="append", help="source directory of real files (repeatable)")
    ap.add_argument("--seed", type=int, default=20261030)
    ap.add_argument("--amipart", default=os.environ.get("AMIPART", os.path.join(HERE, "..", "..", "AmiPart", "host", "amipart")))
    ap.add_argument("--keep-mounted", action="store_true", help="debug: leave the FUSE mount up")
    a = ap.parse_args()

    fs = FS[a.fs]
    if not os.path.isfile(fs["handler"]): sys.exit("handler not found: " + fs["handler"])
    if not os.access(a.amipart, os.X_OK): sys.exit("AmiPart host CLI not found: " + a.amipart)
    if not shutil.which("amifuse"): sys.exit("amifuse not on PATH")
    srcs = a.src or ["/mnt/share/Amiga/Demos", "/mnt/share/Amiga/MODULES", os.path.expanduser("~/UAE/WB323")]
    srcs = [s for s in srcs if os.path.isdir(s)]
    if not srcs: sys.exit("no source directory found")

    part_cyls = (a.size_mb * 1024 * 1024 + CYL_BYTES - 1) // CYL_BYTES
    cyls = part_cyls + 2
    part_bytes = part_cyls * CYL_BYTES
    out = os.path.abspath(a.out or os.path.join(HERE, f"frag{a.size_mb}_{a.fs}.hdf"))
    mnt = os.path.join(WORK, f"mnt_{a.fs}")
    rng = random.Random(a.seed)
    pad = os.urandom(64 * 1024)

    log(f"{a.fs}: {part_bytes // (1024*1024)} MB partition {fs['part']} in {out}")
    make_image(out, cyls, a.fs, a.amipart)
    mount(out, a.fs, mnt)
    try:
        # 3. tail filler: one big contiguous file, freed at the end.
        tail = os.path.join(mnt, "zz_tail")
        tail_bytes = int(part_bytes * TAIL_FRACTION)
        try:
            with open(tail, "wb") as fh:
                left = tail_bytes
                while left > 0:
                    n = min(left, len(pad)); fh.write(pad[:n]); left -= n
        except OSError as e:
            sys.exit(f"tail filler failed: {e}")
        log(f"tail filler {tail_bytes // (1024*1024)} MB written")

        # 4. sieve: alternate A (holes) and B (gaps) until the disk is full.
        holes, gaps, dirs = [], [], 0
        a_bytes = b_bytes = 0
        try:
            while True:
                d = os.path.join(mnt, f"sieve{dirs:04d}")
                os.mkdir(d); dirs += 1
                for i in range(FILES_PER_DIR // 2):
                    na = rng.randint(2, 32) * BLOCK
                    nb = rng.randint(1, 8) * BLOCK
                    pa = os.path.join(d, f"a{i:03d}"); write_file(pa, pad[:na]); holes.append(pa); a_bytes += na
                    pb = os.path.join(d, f"b{i:03d}"); write_file(pb, pad[:nb]); gaps.append(pb); b_bytes += nb
                if dirs % 20 == 0:
                    log(f"sieve: {len(holes)} pairs, {(a_bytes + b_bytes) // (1024*1024)} MB")
        except Full:
            pass
        log(f"sieve full: {len(holes)} hole files ({a_bytes // (1024*1024)} MB), "
            f"{len(gaps)} gap files ({b_bytes // (1024*1024)} MB), {dirs} dirs")

        # 5. open the holes
        for p in holes: os.remove(p)
        log("holes opened")

        # 6. real files: own recursive walk so truncated directory names
        # are carried along to the destination path.
        target = int(part_bytes * a.fill)
        st = dict(copied=0, nfiles=0, full=False)

        def copy_tree(src_dir, dst_dir):
            try: entries = sorted(os.listdir(src_dir))
            except OSError: return
            taken = set()
            files = [e for e in entries if os.path.isfile(os.path.join(src_dir, e))]
            subs  = [e for e in entries if os.path.isdir(os.path.join(src_dir, e)) and not os.path.islink(os.path.join(src_dir, e))]
            for fn in files:
                if st["full"] or st["copied"] >= target: return
                if fn.endswith(".uaem") or fn.startswith("."): continue
                sp = os.path.join(src_dir, fn)
                size = os.path.getsize(sp)
                if size == 0 or size > part_bytes // 4: continue
                try:
                    copy_file(sp, os.path.join(dst_dir, ami_name(fn, taken)))
                except Full:
                    st["full"] = True; return
                st["copied"] += size; st["nfiles"] += 1
                if st["nfiles"] % 1000 == 0:
                    log(f"real files: {st['nfiles']}, {st['copied'] // (1024*1024)} MB")
            for dn in subs:
                if st["full"] or st["copied"] >= target: return
                nd = os.path.join(dst_dir, ami_name(dn, taken))
                try: os.mkdir(nd)
                except OSError as e:
                    if e.errno in (errno.ENOSPC, errno.EIO): st["full"] = True; return
                    raise
                copy_tree(os.path.join(src_dir, dn), nd)

        for src in srcs:
            if st["full"] or st["copied"] >= target: break
            top = os.path.join(mnt, ami_name(os.path.basename(src.rstrip("/")), set()))
            os.mkdir(top)
            copy_tree(src, top)
        copied, nfiles, full = st["copied"], st["nfiles"], st["full"]
        log(f"real files done: {nfiles} files, {copied // (1024*1024)} MB" + (" (disk full)" if full else ""))

        # 7. remove tail and gaps. Tail first: SFS parks deleted files in
        # .recycled (FIFO, a few hundred entries) and the many gap deletions
        # that follow push the big tail file out of it again.
        os.remove(tail)
        for p in gaps: os.remove(p)
        for i in range(dirs):
            try: os.rmdir(os.path.join(mnt, f"sieve{i:04d}"))
            except OSError: pass
        log("tail and gap fillers removed")
    finally:
        if not a.keep_mounted:
            unmount(mnt)

    tool = os.path.join(HERE, "..", "host", "amidefrag")
    if os.access(tool, os.X_OK):
        r = sh(f'"{tool}" IMAGE="{out}" PART={fs["part"]}', check=False)
        print(r.stdout[-1200:])
    log(f"done: {out}")

if __name__ == "__main__":
    main()
