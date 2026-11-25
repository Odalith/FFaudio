#!/usr/bin/env python3
"""Heuristic lock-nesting scan of lib/*.c: which locks are taken while another is held (directly, or through a call
to a function that takes one). Prints pairs 'held -> taken  (function, file:line)'.

    scripts/dev/lock_scan.py [lib-dir]      (default: the lib/ next to this script's repository)

Heuristic: it reads the source as text, so check every pair it prints against docs/threading.md ("Lock order") by
reading the code. Run it after any change that adds a lock or takes one in a new place."""
import os, re, sys, glob, collections

root = os.path.join(sys.argv[1] if len(sys.argv) > 1 else
                    os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "lib"), "")
LOCK = re.compile(r"\b(SDL_LockMutex|SDL_TryLockMutex|SDL_UnlockMutex)\(\s*([^)]+?)\s*\)")
MIX = re.compile(r"\b(lock_mixer|unlock_mixer|SDL_LockAudioDevice|SDL_UnlockAudioDevice)\(")
FUNC = re.compile(r"^(?:static\s+)?(?:inline\s+)?[A-Za-z_][\w \*]*?[\s\*]([A-Za-z_]\w*)\([^;]*\)\s*\{?\s*$")

def norm(expr):
    e = expr.strip()
    e = re.sub(r"^&", "", e)
    e = re.sub(r"\b(t|ap|q|d|r|tp|s)->", "", e)
    e = e.replace("t->ap->", "")
    return e

funcs = {}  # name -> (file, start line, [lines])
for path in sorted(glob.glob(root + "*.c")):
    lines = open(path).read().split("\n")
    i = 0
    while i < len(lines):
        m = FUNC.match(lines[i])
        if m and not lines[i].startswith(" ") and "(" in lines[i] and not lines[i].rstrip().endswith(";"):
            name = m.group(1)
            j = i
            while j < len(lines) and "{" not in lines[j]: j += 1
            depth, k, body = 0, j, []
            while k < len(lines):
                depth += lines[k].count("{") - lines[k].count("}")
                body.append((k + 1, lines[k]))
                if depth == 0 and k > j - 1 and "}" in lines[k]: break
                k += 1
            funcs[name] = (path.split("/")[-1], body)
            i = k + 1
        else:
            i += 1

# Which functions take which locks directly
takes = collections.defaultdict(set)
for name, (f, body) in funcs.items():
    for ln, l in body:
        for m in LOCK.finditer(l):
            if m.group(1) != "SDL_UnlockMutex": takes[name].add(norm(m.group(2)))
        for m in MIX.finditer(l):
            if m.group(1) in ("lock_mixer", "SDL_LockAudioDevice"): takes[name].add("DEVICE")
# Transitive closure over calls (a few rounds)
calls = {n: set(re.findall(r"\b([a-z_]\w*)\(", " ".join(l for _, l in b))) & set(funcs) - {n} for n, (_, b) in funcs.items()}
closure = {n: set(s) for n, s in takes.items()}
for _ in range(6):
    for n in funcs:
        for c in calls.get(n, ()):
            closure.setdefault(n, set()).update(closure.get(c, set()))

pairs = collections.defaultdict(list)
for name, (f, body) in funcs.items():
    held = []
    texts = [l for _, l in body]
    for idx, (ln, l) in enumerate(body):
        # an unlock right before a return leaves a branch, not the main path
        branch_exit = any("return" in t or "goto " in t or "break;" in t or "continue;" in t
                          for t in texts[idx:idx + 3])
        code = l.split("//")[0]
        events = []
        for m in LOCK.finditer(code): events.append((m.start(), m.group(1), norm(m.group(2))))
        for m in MIX.finditer(code):
            events.append((m.start(), "lock" if m.group(1) in ("lock_mixer", "SDL_LockAudioDevice") else "unlock", "DEVICE"))
        # calls to lock-taking functions while holding something
        if held:
            for c in re.findall(r"\b([a-z_]\w*)\(", code):
                if c in closure and c != name and c not in ("lock_mixer", "unlock_mixer"):
                    for lk in closure[c]:
                        for h in held:
                            if lk != h: pairs[(h, lk)].append("%s via %s() %s:%d" % (name, c, f, ln))
        for _, kind, lk in sorted(events):
            if kind in ("SDL_LockMutex", "SDL_TryLockMutex", "lock"):
                for h in held:
                    if h != lk: pairs[(h, lk)].append("%s %s:%d%s" % (name, f, ln, " (try)" if kind == "SDL_TryLockMutex" else ""))
                held.append(lk)
            else:
                if lk in held and not branch_exit: held.remove(lk)
for (h, lk), where in sorted(pairs.items()):
    print("%-28s -> %-28s %s%s" % (h, lk, "; ".join(where[:3]), " (+%d)" % (len(where) - 3) if len(where) > 3 else ""))
