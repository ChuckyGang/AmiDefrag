#!/usr/bin/env python3
"""
amifuse_patched.py - run amifuse with a larger per-operation reply budget.

amifuse waits at most 50 handler bursts for a packet reply. pfs3aio on a
1 GB volume exceeds that for long operations (Examine on a cold volume,
the commit after tens of thousands of deletes), and amifuse then reports
EIO / "not found" for an operation the handler is still busy with. This
wrapper raises the budget (AMIFUSE_MAX_ITERS, default 4000) and otherwise
behaves exactly like the amifuse command. Run it with amifuse's own Python:

    $(head -1 $(which amifuse) | cut -c3-) test/amifuse_patched.py ls IMAGE ...
"""
import os, sys
import amifuse.fuse_fs as ff

_orig = ff.HandlerBridge._run_until_replies
_iters = int(os.environ.get("AMIFUSE_MAX_ITERS", "4000"))

def _patched(self, max_iters=_iters, *a, **kw):
    return _orig(self, max_iters, *a, **kw)

ff.HandlerBridge._run_until_replies = _patched

if __name__ == "__main__":
    sys.exit(ff.main())
