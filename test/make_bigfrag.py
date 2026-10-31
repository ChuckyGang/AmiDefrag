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
  4. The rest of the disk is filled to the brim with alternating files: a
     filler A (2..32 blocks, a future hole) and a real small source file
     (<= 32 KB, a "spacer" that stays).
  5. All A fillers and the tail are deleted -> free space is a sieve of
     1-16 KB holes plus one big run.
  6. The larger real files (default sources: /mnt/share/Amiga/Demos, MODULES,
     then ~/UAE/WB323; directory structure preserved, names cut to 30 chars)
     are copied in until --fill or a 16 MB free-space margin is reached.
     Every file larger than a hole is split across many holes by the
     handler's own allocator.
  Nothing is deleted after step 5: the handlers (through amifuse) stop
  answering lookups late in a long session, and PFS3 cannot be cleaned up
  offline at all. --cleanup-only removes leftovers of older runs (FFS/SFS).
  Afterwards host/amidefrag reports the result.

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
SPACER_MAX = 32 * 1024                   # real files up to this size serve as spacers

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

def rm_retry(path, isdir=False):
    """Delete with retries; a missing object counts as done."""
    for _ in range(3):
        try:
            (os.rmdir if isdir else os.remove)(path)
            return True
        except FileNotFoundError:
            return True
        except OSError:
            time.sleep(0.2)
    return False

def cleanup_known(mnt, gaps, dirs):
    """In-session cleanup by the names we created, no directory listing:
    amifuse's readdir (Examine/ExNext) never returns on 1 GB PFS3 volumes
    although create, Lock and Delete work, so listdir would see nothing."""
    failed = 0
    if not rm_retry(os.path.join(mnt, "zz_tail")): failed += 1
    for i, p in enumerate(gaps):
        if not rm_retry(p): failed += 1
        if (i + 1) % 10000 == 0: log(f"cleanup: {i + 1}/{len(gaps)} gap files, {failed} failures")
    for d in range(dirs):
        if not rm_retry(os.path.join(mnt, f"sieve{d:04d}"), isdir=True): failed += 1
    return failed

def cleanup_fillers(mnt):
    """--cleanup-only: delete the tail filler, every file left in the sieve
    directories and the directories themselves, by listing them. Works for
    FFS and SFS; on 1 GB PFS3 amifuse cannot list directories (see above),
    so leftovers there must be deleted on the Amiga. Returns failures."""
    failed = 0
    if not rm_retry(os.path.join(mnt, "zz_tail")): failed += 1
    try: tops = sorted(n for n in os.listdir(mnt) if n.startswith("sieve"))
    except OSError: tops = []
    if not tops:
        log("cleanup: no sieve directories listed (nothing left, or the handler cannot list directories)")
    for i, dn in enumerate(tops):
        d = os.path.join(mnt, dn)
        try: names = os.listdir(d)
        except OSError: names = []
        for n in names:
            if not rm_retry(os.path.join(d, n)): failed += 1
        if not rm_retry(d, isdir=True): failed += 1
        if (i + 1) % 100 == 0: log(f"cleanup: {i + 1}/{len(tops)} dirs, {failed} failures")
    return failed

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
    ap.add_argument("--cleanup-only", action="store_true",
                    help="only remove leftover fillers from an existing image (after an interrupted run)")
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
    if a.cleanup_only:
        mount(out, a.fs, mnt)
        try:
            failed = cleanup_fillers(mnt)
            log(f"cleanup done ({failed} failures)")
        finally:
            if not a.keep_mounted: unmount(mnt)
        report(out, fs)
        return
    make_image(out, cyls, a.fs, a.amipart)
    mount(out, a.fs, mnt)
    try:
        # Gather the real files once: small ones (<= SPACER_MAX) become the
        # spacers of the sieve, the rest is copied into the holes afterwards.
        def gather(src):
            for root, dnames, fnames in os.walk(src):
                dnames[:] = sorted(d for d in dnames if not os.path.islink(os.path.join(root, d)))
                for fn in sorted(fnames):
                    if fn.endswith(".uaem") or fn.startswith("."): continue
                    sp = os.path.join(root, fn)
                    try: size = os.path.getsize(sp)
                    except OSError: continue
                    if 0 < size <= part_bytes // 4:
                        yield sp, size
        small = []
        for src in srcs:
            small += [(sp, sz) for sp, sz in gather(src) if sz <= SPACER_MAX]
        rng.shuffle(small)
        if not small: sys.exit("no small source files for spacers")
        log(f"{len(small)} small source files (<= {SPACER_MAX // 1024} KB) available as spacers")

        # 3. tail filler: one big contiguous file, deleted before the real
        # files go in, so one large free run survives at the end of the disk.
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

        # 4. sieve: alternate a filler A (future hole, 2..32 blocks) with a
        # real small file (spacer, stays) until the disk is full. The spacers
        # are legitimate content, so nothing has to be deleted afterwards:
        # that matters because the handlers (through amifuse) stop answering
        # deletes and lookups late in a long session, and PFS3 cannot be
        # cleaned up offline at all (amifuse never gets Examine answered on
        # 1 GB PFS3 volumes).
        holes, dirs, nsp = [], 0, 0
        a_bytes = s_bytes = 0
        try:
            while True:
                d = os.path.join(mnt, f"small{dirs:04d}")
                os.mkdir(d); dirs += 1
                taken = set()
                for i in range(FILES_PER_DIR // 2):
                    na = rng.randint(2, 32) * BLOCK
                    pa = os.path.join(d, f"a{i:03d}"); write_file(pa, pad[:na]); holes.append(pa); a_bytes += na
                    sp, sz = small[nsp % len(small)]
                    nm = ami_name(os.path.basename(sp), taken)
                    copy_file(sp, os.path.join(d, nm)); nsp += 1; s_bytes += sz
                if dirs % 20 == 0:
                    log(f"sieve: {len(holes)} holes, {(a_bytes + s_bytes) // (1024*1024)} MB")
        except Full:
            pass
        log(f"sieve full: {len(holes)} hole files ({a_bytes // (1024*1024)} MB), "
            f"{nsp} spacer files ({s_bytes // (1024*1024)} MB), {dirs} dirs")

        # 5. open the holes, free the tail
        hf = sum(0 if rm_retry(p) else 1 for p in holes)
        if not rm_retry(tail): hf += 1
        log(f"holes opened, tail removed ({hf} failures)")

        # 6. the larger real files, directory structure kept. Never run into
        # "disk full": amifuse hangs on the first operation after a partial
        # big write. Cap at 95% of the hole volume + tail, 16 MB margin.
        target = min(int(part_bytes * a.fill), int((a_bytes + tail_bytes) * 0.95))
        MARGIN = 16 * 1024 * 1024
        st = dict(copied=0, nfiles=0, full=False)

        def free_bytes():
            try:
                v = os.statvfs(mnt)
                return v.f_bavail * v.f_frsize
            except OSError:
                return None

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
                if size <= SPACER_MAX or size > part_bytes // 4: continue
                fb = free_bytes()
                if fb is not None and size + MARGIN > fb:
                    st["full"] = True; return
                try:
                    copy_file(sp, os.path.join(dst_dir, ami_name(fn, taken)))
                except Full:
                    st["full"] = True; return
                st["copied"] += size; st["nfiles"] += 1
                if st["nfiles"] % 500 == 0:
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
        log(f"real files done: {nfiles} files, {copied // (1024*1024)} MB" + (" (stopped at free-space margin)" if full else ""))
    finally:
        if not a.keep_mounted:
            unmount(mnt)

    report(out, fs)

def report(out, fs):
    tool = os.path.join(HERE, "..", "host", "amidefrag")
    if os.access(tool, os.X_OK):
        r = sh(f'"{tool}" IMAGE="{out}" PART={fs["part"]}', check=False)
        print(r.stdout[-1200:])
    log(f"done: {out}")

if __name__ == "__main__":
    main()
