#!/usr/bin/env python3
"""pfsfuzz.py - PFS3 move engine fuzz loop (Linux host, not Amiga).

Each iteration builds a small PFS3 image with AmiPart's host CLI (RDB +
PDS\\3 partition, internal formatter), fills it through the REAL pfs3aio
handler running under amifuse's handler bridge with a write / delete /
write pattern so the later files land in the holes (PFS3 allocates from a
roving pointer), writes the 128 KB AmiDefrag.journal file, records the
md5 of every file, then:

  1. runs  host/amidefrag IMAGE=.. PART=DH0 DEFRAG FORCE  and checks, again
     through the real handler, that every file reads back identical and
     the handler can still create and delete a file afterwards; that no
     fragmented file remains except those reported as skipped; and that
     no block is marked used without being referenced (the map's
     "unknown" count), i.e. nothing leaked;
  2. replays the SAME starting image several times with a simulated crash
     after a random write (AMIDEFRAG_CRASH_AFTER=n, and the torn-sector
     variant n't'), runs AmiDefrag again (journal finishes or undoes the
     interrupted file, then the rest), and checks as in 1.

Requirements: AmiPart host CLI (../../AmiPart/host/amipart or AMIPART=),
amifuse installed in a venv (AMIFUSE_PYTHON= its python, default the pipx
venv), pfs3aio handler (PFS3_HANDLER= or ~/UAE/*/L/pfs3aio), host/amidefrag.
Usage: python3 pfsfuzz.py [iterations] [base_seed]
Failed iterations keep their images as work/pfsfuzz/fail_*.hdf.
"""
import hashlib, os, random, subprocess, sys, shutil, glob, json

HERE = os.path.dirname(os.path.abspath(__file__))
HOST = os.path.join(HERE, "..", "host", "amidefrag")
WORK = os.path.join(HERE, "work", "pfsfuzz")
AMIPART = os.environ.get("AMIPART", os.path.join(HERE, "..", "..", "AmiPart", "host", "amipart"))
PYTHON = os.environ.get("AMIFUSE_PYTHON", os.path.expanduser("~/.local/share/pipx/venvs/amifuse/bin/python"))
DRIVER = os.environ.get("PFS3_HANDLER") or (glob.glob(os.path.expanduser("~/UAE/WB323/L/pfs3aio")) +
                                            glob.glob(os.path.expanduser("~/UAE/*/L/pfs3aio")) + [None])[0]
PART = "DH0"
CYL_BYTES = 516096          # 16 heads x 63 sectors x 512

# The handler bridge runs in amifuse's own interpreter: a small worker
# script receives a JSON job on stdin and answers with JSON on stdout.
BRIDGE_WORKER = r'''
import sys, os, json, hashlib
from pathlib import Path
import amifuse.fuse_fs as ff
job = json.load(sys.stdin)
orig = ff.HandlerBridge._run_until_replies
ff.HandlerBridge._run_until_replies = lambda self, max_iters=4000, **kw: orig(self, max_iters=max_iters, **kw)
b = ff.HandlerBridge(Path(job["image"]), Path(job["driver"]), read_only=False, partition=job["part"])
out = {"ok": True, "log": []}
def parent_lock(path):
    parts = [p for p in path.split("/") if p]
    if len(parts) <= 1:
        lk, _ = b.locate(0, "")
        return lk, parts[-1] if parts else ""
    lk, res2, locks = b.locate_path("/" + "/".join(parts[:-1]))
    return lk, parts[-1]
try:
    for op in job["ops"]:
        kind = op[0]
        if kind == "mkdir":
            lk, name = parent_lock(op[1])
            r = b.create_dir(lk, name)
            # CreateDir returns an exclusive lock: free it or every later
            # Lock() on the directory fails with "object in use"
            if r[0]:
                b.free_lock(r[0])
            out["log"].append(["mkdir", op[1], r[0]])
        elif kind == "write":
            path, size, seed = op[1], op[2], op[3]
            data = hashlib.sha256(seed.encode()).digest()
            rnd = bytearray()
            while len(rnd) < size:
                data = hashlib.sha256(data).digest(); rnd += data
            rnd = bytes(rnd[:size])
            r = b.open_file(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC)
            if not r:
                out["log"].append(["write", path, "open failed"]); out["ok"] = False; continue
            fh, lk = r
            pos = 0; failed = False
            while pos < size:
                n = b.write_handle(fh, rnd[pos:pos + 65536])
                if n <= 0: failed = True; break
                pos += n
            b.close_file(fh)
            if failed:
                # disk full: remove the partial file so every file on the
                # volume has known content
                lk2, name2 = parent_lock(path)
                b.delete_object(lk2, name2)
            out["log"].append(["write", path, size if not failed else "short"])
        elif kind == "delete":
            lk, name = parent_lock(op[1])
            r = b.delete_object(lk, name)
            out["log"].append(["delete", op[1], r[0], r[1]])
            if r[0] == 0: out["ok"] = False
        elif kind == "md5":
            res = {}
            for path in op[1]:
                st = b.stat_path(path)
                if not st:
                    res[path] = "MISSING"; continue
                size = st["size"]
                # read in chunks: read_file() allocates the whole size in
                # emulated memory and fails on files of a few MB
                r = b.open_file(path, os.O_RDONLY)
                if not r:
                    res[path] = "OPENFAIL"; continue
                fh, lk = r
                h = hashlib.md5(); got = 0
                while got < size:
                    chunk = b.read_handle_at(fh, got, min(262144, size - got))
                    if not chunk: break
                    h.update(chunk); got += len(chunk)
                b.close_file(fh)
                res[path] = h.hexdigest() if got == size else "SHORT:%d/%d" % (got, size)
            out["md5"] = res
        elif kind == "flush":
            b.flush_volume()
        elif kind == "info":
            out["info"] = b.get_disk_info()
finally:
    try:
        b.flush_volume()
    except Exception:
        pass
    ff._cleanup_bridge(b, None)
sys.stdout.write("\n@@JSON@@" + json.dumps(out) + "\n")
'''


def sh(cmd, env=None, check=True):
    e = dict(os.environ)
    if env: e.update(env)
    r = subprocess.run(cmd, shell=True, capture_output=True, text=True, env=e)
    if check and r.returncode != 0:
        raise RuntimeError(f"command failed ({r.returncode}): {cmd}\n{r.stdout}\n{r.stderr}")
    return r


def bridge(img, ops):
    job = json.dumps({"image": os.path.abspath(img), "driver": DRIVER, "part": PART, "ops": ops})
    worker = os.path.join(WORK, "bridge_worker.py")
    with open(worker, "w") as f:
        f.write(BRIDGE_WORKER)
    r = subprocess.run([PYTHON, "-I", worker], input=job, capture_output=True, text=True, timeout=3600)
    if "@@JSON@@" not in r.stdout:
        raise RuntimeError("bridge worker failed:\n" + r.stdout[-2000:] + r.stderr[-2000:])
    return json.loads(r.stdout.split("@@JSON@@")[-1].strip().splitlines()[0])


def content_md5(size, seed):
    data = hashlib.sha256(seed.encode()).digest()
    rnd = bytearray()
    while len(rnd) < size:
        data = hashlib.sha256(data).digest(); rnd += data
    return hashlib.md5(bytes(rnd[:size])).hexdigest()


# ---------------------------------------------------------------- image ---

def build_image(img, rng, it):
    """Random fragmented PFS3 image; returns {amiga_path: md5}."""
    cyl = rng.randint(16, 70)                       # 8 .. 35 MB
    if os.path.exists(img): os.remove(img)
    with open(img, "wb") as f:
        f.truncate(cyl * CYL_BYTES)
    script = os.path.join(WORK, "part.script")
    with open(script, "w") as f:
        f.write(f"OPEN FILE {os.path.abspath(img)}\nINIT NEW\n"
                f"ADDPART NAME={PART} LOW=2 HIGH={cyl - 1} TYPE=PDS3 VOLNAME=PFSFUZZ\nWRITE\nCLOSE\n")
    sh(f'"{AMIPART}" SCRIPT "{script}" FORCE')
    total = (cyl - 2) * CYL_BYTES
    # PFS3 allocates from a roving pointer, so holes are only reused once
    # the end of the disk is reached: fill it almost completely first,
    # free every other file, then write files larger than the holes.
    budget = int(total * rng.uniform(0.88, 0.96))
    cap = max(8000, total // 10)
    files = {}
    ops = []
    dirs = [""]
    for d in range(rng.randint(0, 3)):
        ops.append(["mkdir", f"dir{d}"])
        dirs.append(f"dir{d}/")
    # the journal first: it must exist before the extra files fill the disk
    ops.append(["write", "AmiDefrag.journal", 128 * 1024, "zero"])
    names = []
    used = n = 0
    while used < budget and n < 400:
        size = min(cap, rng.choice([rng.randint(1, 3000), rng.randint(3000, 60000),
                                    rng.randint(60000, 400000), rng.randint(200000, cap)]))
        if used + size > budget: break
        path = rng.choice(dirs) + f"f{n}"
        seed = f"{it}:{path}:{size}"
        ops.append(["write", path, size, seed])
        files[path] = content_md5(size, seed)
        names.append(path)
        used += size
        n += 1
    for path in names[rng.randint(0, 1)::2]:
        ops.append(["delete", path])
        del files[path]
    extra = 0
    while extra < 40:
        size = min(cap, rng.randint(50000, 600000))
        path = rng.choice(dirs) + f"g{extra}"
        seed = f"{it}:{path}:{size}"
        ops.append(["write", path, size, seed])
        files[path] = content_md5(size, seed)
        extra += 1
    ops.append(["flush"])
    res = bridge(img, ops)
    if os.environ.get("PFSFUZZ_DEBUG"):
        for entry in res["log"]:
            print("      ", entry)
    # a failed write (disk full) just means that file is not there
    for entry in res["log"]:
        if entry[0] == "write" and entry[2] in ("open failed", "short"):
            files.pop(entry[1], None)
    # files that were written short are unknown: verify what is there
    return files


def check_files(img, files):
    res = bridge(img, [["md5", sorted(files)], ["write", "post_run_probe", 1024, "probe"],
                       ["delete", "post_run_probe"], ["flush"]])
    bad = [p for p, h in files.items() if res["md5"].get(p) != h]
    return bad, res["ok"]


def analyse(img):
    """(fragmented, unknown, note, stdout)"""
    r = sh(f'"{HOST}" IMAGE="{img}" PART={PART}', check=False)
    frag = unknown = None
    note = ""
    for line in r.stdout.splitlines():
        if line.startswith("Files:"):
            frag = int(line.split("fragmented:")[1].split()[0])
        if "unknown" in line and "blocks" in line and line.strip().startswith("?"):
            unknown = int(line.split()[2])
        if line.startswith("Note:"):
            note = line
    return frag, unknown, note, r.stdout


def run_defrag(img, env=None, trace=False):
    """With trace=True the shim lists every write; single-sector writes are
    the metadata ones (journal, bitmap, anode) and make the best crash
    points.  Returned as the 7th element when asked for."""
    e = dict(env or {})
    if trace: e["AMIDEFRAG_TRACE_WRITES"] = "1"
    r = sh(f'"{HOST}" IMAGE="{img}" PART={PART} DEFRAG FORCE', env=e, check=False)
    singles = []
    if trace:
        for line in r.stderr.splitlines():
            if line.startswith("[shim] w#") and line.endswith(" +1"):
                singles.append(int(line.split("#")[1].split()[0]))
    writes = skipped = moved = errors = 0
    for line in r.stdout.splitlines():
        if line.startswith("Device I/O:"):
            writes = int(line.split("read,")[1].split()[0])
        if line.startswith("Done:"):
            moved = int(line.split("fragmented,")[1].split()[0])
            skipped = int(line.split("blocks),")[1].split()[0])
            errors = int(line.split("skipped for space,")[1].split()[0])
    return r.returncode, writes, moved, skipped, errors, r.stdout + (r.stderr if not trace else ""), singles


# ----------------------------------------------------------------- loop ---

def expect(cond, msg, log):
    if not cond:
        log.append("FAIL: " + msg)
    return cond


def iteration(it, seed):
    rng = random.Random(seed)
    log = []
    img = os.path.join(WORK, f"it{it}.hdf")
    start = os.path.join(WORK, f"it{it}_start.hdf")
    files = build_image(img, rng, it)
    frag0, unk0, note0, _ = analyse(img)
    log.append(f"image: {os.path.getsize(img) // 1048576} MB, {len(files)} files, fragmented {frag0}, unknown {unk0} {note0}")
    if not expect(unk0 == 0, f"fresh image has {unk0} unknown blocks", log):
        return False, log
    shutil.copy(img, start)
    ok = True

    # --- 1. clean run ---
    rc, writes, moved, skipped, errors, out, singles = run_defrag(img, trace=True)
    log.append(f"clean run: rc {rc}, {writes} writes ({len(singles)} metadata), moved {moved}, skipped {skipped}, errors {errors}")
    ok &= expect(rc in (0, 5), f"defrag rc {rc}\n{out[-1500:]}", log)
    ok &= expect(errors == 0, f"{errors} files skipped with errors\n{out[-1500:]}", log)
    bad, hok = check_files(img, files)
    ok &= expect(not bad, f"files changed after clean run: {bad}", log)
    ok &= expect(hok, "handler could not write after the clean run", log)
    frag1, unk1, note1, out1 = analyse(img)
    ok &= expect(frag1 == skipped, f"after clean run {frag1} fragmented, {skipped} skipped {note1}", log)
    ok &= expect(unk1 == 0, f"after clean run {unk1} unknown blocks {note1}", log)
    if not ok or writes < 2:
        return ok, log

    # --- 2. crash replays ---
    for k in range(6):
        # mostly right at a metadata write (crash before or in it), sometimes anywhere
        if singles and rng.random() < 0.8:
            n = rng.choice(singles) + rng.choice([0, 0, 1])
            n = min(n, writes - 1)
        else:
            n = rng.randint(0, writes - 1)
        torn = rng.random() < 0.5
        spec = f"{n}{'t' if torn else ''}"
        shutil.copy(start, img)
        rc, _, _, _, _, out, _ = run_defrag(img, env={"AMIDEFRAG_CRASH_AFTER": spec})
        if not expect(rc == 99, f"crash replay {spec}: rc {rc} instead of 99\n{out[-800:]}", log):
            ok = False; continue
        fragc, unkc, notec, _ = analyse(img)
        # the crashed image must still read correctly through the handler
        bad, hok = check_files(img, files)
        ok &= expect(not bad, f"crash {spec}: files damaged before recovery: {bad}", log)
        rc2, _, moved2, skipped2, errors2, out2, _ = run_defrag(img)
        ok &= expect(rc2 in (0, 5), f"crash {spec}: rerun rc {rc2}\n{out2[-1200:]}", log)
        bad, hok = check_files(img, files)
        ok &= expect(not bad, f"crash {spec}: files changed after rerun: {bad}", log)
        ok &= expect(hok, f"crash {spec}: handler could not write after rerun", log)
        frag2, unk2, note2, _ = analyse(img)
        ok &= expect(frag2 == skipped2, f"crash {spec}: after rerun {frag2} fragmented, {skipped2} skipped {note2}", log)
        ok &= expect(unk2 == 0, f"crash {spec}: after rerun {unk2} unknown (leaked) blocks {note2}", log)
        log.append(f"crash {spec}: crashed with {fragc} fragmented / {unkc} unknown, rerun moved {moved2}, skipped {skipped2}, leaks {unk2}")
    return ok, log


def main():
    iters = int(sys.argv[1]) if len(sys.argv) > 1 else 3
    base = int(sys.argv[2]) if len(sys.argv) > 2 else random.randrange(1 << 30)
    os.makedirs(WORK, exist_ok=True)
    if not os.path.exists(HOST): sys.exit("build host/amidefrag first (make linux)")
    if not os.path.exists(AMIPART): sys.exit(f"AmiPart host CLI not found at {AMIPART}")
    if not DRIVER or not os.path.exists(DRIVER): sys.exit("pfs3aio handler not found (PFS3_HANDLER=)")
    fails = 0
    for it in range(iters):
        seed = base + it
        print(f"--- iteration {it} seed {seed}", flush=True)
        try:
            ok, log = iteration(it, seed)
        except Exception as ex:
            ok, log = False, [f"EXCEPTION: {ex}"]
        for l in log:
            print("   ", l, flush=True)
        if not ok:
            fails += 1
            for f in (f"it{it}.hdf", f"it{it}_start.hdf"):
                src = os.path.join(WORK, f)
                if os.path.exists(src): shutil.copy(src, os.path.join(WORK, f"fail_{seed}_{f}"))
        print("    ->", "OK" if ok else "FAILED", flush=True)
    print(f"{iters - fails}/{iters} iterations passed")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
