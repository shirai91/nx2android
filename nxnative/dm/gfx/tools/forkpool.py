#!/usr/bin/env python3
"""The work in parallel without POSIX semaphores.

A process pool (multiprocessing, concurrent.futures) makes POSIX semaphores, and those live in /dev/shm: a host
without it (Android has none) cannot make a pool at all, and the work runs in a single process. Forking does not
need them: the children inherit what they need and hand their results back through a pipe.

    fork_map(fn, items, jobs=8, init=None)  ->  the results, in the items' order

init is called in each child before its share of the work (the pool's initializer). Each child writes its share as
one pickle; a child that dies leaves the parent without it, and the child's own message (on stderr) says why.
A child that fails sends its message instead of its share: the parent reads every child's pipe, prints the
messages and stops. So the failure is read once, as the reason, and not as the other children's broken pipes.
"""
import os
import pickle
import sys
import traceback


def fork_map(fn, items, jobs=8, init=None):
    """fn over items, in forked children (one strided chunk each); the results come back in the items' order"""
    items = list(items)
    if not items: return []
    jobs = max(1, min(int(jobs or 1), len(items)))
    indexed = list(enumerate(items))
    chunks = [indexed[i::jobs] for i in range(jobs) if indexed[i::jobs]]   # strided: the big items spread out
    sys.stdout.flush(); sys.stderr.flush()   # a child would repeat the parent's buffered output
    kids = []
    for ch in chunks:
        r, w = os.pipe()
        pid = os.fork()
        if pid == 0:   # the child
            try:
                os.close(r)
                if init: init()
                out = pickle.dumps(('ok', [(i, fn(item)) for i, item in ch]), protocol=pickle.HIGHEST_PROTOCOL)
            except SystemExit as e:   # a deliberate stop (the split's red line): the message goes to the parent, not a traceback
                out = pickle.dumps(('error', str(e) or 'the child stopped without a message'), protocol=pickle.HIGHEST_PROTOCOL)
            except BaseException:
                out = pickle.dumps(('error', traceback.format_exc()), protocol=pickle.HIGHEST_PROTOCOL)
            try:
                with os.fdopen(w, 'wb') as f:
                    f.write(out); f.flush()
            except BrokenPipeError:
                pass   # the parent is gone: there is nothing to send it to
            sys.stdout.flush(); sys.stderr.flush()
            os._exit(0)
        os.close(w)
        kids.append((pid, r))
    results, errors = [], []
    for pid, r in kids:
        try:
            with os.fdopen(r, 'rb') as f:
                got = pickle.load(f)
        except (EOFError, pickle.UnpicklingError) as e:
            os.waitpid(pid, 0)
            errors.append(f'a forked child died before it delivered its results ({e.__class__.__name__})')
            continue
        os.waitpid(pid, 0)
        if got[0] == 'error': errors.append(got[1])
        else: results += got[1]
    if errors:   # every child's pipe is read before this: no child is left writing into a pipe nobody reads
        for e in errors: print(e, file=sys.stderr, flush=True)
        raise RuntimeError(f'{len(errors)} of the {len(kids)} forked children failed (their messages are above)')
    results.sort(key=lambda p: p[0])
    return [r for _, r in results]
