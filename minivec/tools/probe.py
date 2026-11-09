#!/usr/bin/env python3
"""tools/probe.py -- discover which wire framing a MiniVectorDB binary speaks.

The official competition driver is unpublished, so we measure the binary
instead of guessing: run the same 4-statement probe script under several
plausible framings and score how much of the SQL actually executed.

Framings tried (terminator / framing kind):

    bare-semicolon     ";" + newline, one statement at a time   <- src/main.cpp today
    queryok            ";" + newline, expects "Query OK" lines
    mysql-cli          ";" + newline, expects a "mysql> " prompt and "Query OK"
    prompt-nook        ";" + newline, expects a prompt but no "Query OK"
    newline            newline terminated, no semicolon (line per statement)
    newline-queryok    newline terminated, expects "Query OK"
    bulk-eof           everything at once, no terminator, read until EOF

The terminator is the only knob that changes what the server does; the
prompt / Query-OK expectations only describe how the harness must interpret
the reply.  So the score rewards *execution* and only mildly rewards a
framing whose expectations match what the server actually emitted:

    +6  both expected L2 distances (0 and 5) appear  -> L2_DISTANCE evaluated
    +3  a result table is visible (bordered or >=2 tab separated columns)
    +1  the probe's column headers appear
    +2  prompt / Query-OK expectations match the observed output
    -2  per ERROR line (capped at -6) -> the SQL did not parse
    -6  no output at all

Usage:
    tools/probe.py [--binary ./minivec] [--show] [--keep]
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

try:  # the harness owns the protocol knowledge; import it when available
    from harness import PROTOCOL, Server
except Exception:  # pragma: no cover - probe must also work standalone
    PROTOCOL = {"binary": "./minivec", "args": ["--data", "{data}"], "startup_s": 15.0,
                "exit_grace_s": 10.0, "idle_ms": 300, "settle_ms": 80}
    Server = None  # type: ignore


PROBE_SQL = [
    "create table probe (id bigint primary key, name varchar(8), v vector(2))",
    "insert into probe values (1, 'a', [0,0]), (2, 'b', [3,4])",
    "select id, name from probe order by id",
    "select id, l2_distance(v, [0,0]) as d from probe order by d",
]

EXPECT_VALUES = ("0", "5")       # L2 distances the last SELECT must show

FRAMINGS = [
    # name,             terminator, expect prompt, expect "Query OK", mode
    ("bare-semicolon",  ";\n", False, False, "interactive"),
    ("queryok",         ";\n", False, True,  "interactive"),
    ("mysql-cli",       ";\n", True,  True,  "interactive"),
    ("prompt-nook",     ";\n", True,  False, "interactive"),
    ("newline",         "\n",  False, False, "interactive"),
    ("newline-queryok", "\n",  False, True,  "interactive"),
    ("bulk-eof",        ";",   False, False, "bulk"),
]

PROMPT_RE = re.compile(r"^\s*(mysql|MariaDB) ?\[[^\]]*\]?> ?|^\s*mysql> ?")


def collect(protocol: dict, terminator: str, mode: str, data_dir: str) -> tuple[list[str], str]:
    """Run PROBE_SQL under one framing; return (per-statement chunks, stderr)."""
    if Server is None:
        raise RuntimeError("harness.py not importable next to probe.py")
    p = dict(PROTOCOL)
    p.update(protocol)
    p["stmt_terminator"] = terminator
    server = Server(p, data_dir)
    chunks: list[str] = []
    try:
        server.start()
        if mode == "bulk":
            # everything at once, ';' terminated, then close stdin
            payload = ("".join(s + ";\n" for s in PROBE_SQL)).encode()
            server.proc.stdin.write(payload)
            server.proc.stdin.flush()
            server.proc.stdin.close()
            deadline = time.time() + float(p["startup_s"])
            while time.time() < deadline and server.alive():
                time.sleep(0.02)
                chunk = server.read_until_quiet()
                if chunk:
                    chunks.append(chunk.decode("utf-8", "replace"))
            chunks.append(server.read_until_quiet().decode("utf-8", "replace"))
        else:
            for statement in PROBE_SQL:
                chunks.append(server.send((statement + terminator).encode()).decode("utf-8", "replace"))
    finally:
        stderr = server.stderr_text()
        server.stop()
    return chunks, stderr


def strip_prompt(text: str) -> tuple[str, int]:
    count = 0
    out = []
    for line in text.split("\n"):
        match = PROMPT_RE.match(line)
        if match:
            count += 1
            out.append(line[match.end():])
        else:
            out.append(line)
    return "\n".join(out), count


def analyse(chunks: list[str]) -> dict:
    body, prompt_count = strip_prompt("".join(chunks))
    lines = body.split("\n")
    border = sum(1 for l in lines if BORDER_RE.match(l))
    pipes = sum(1 for l in lines if l.startswith("|") and l.endswith("|"))
    tabs = sum(1 for l in lines if "\t" in l and not l.startswith("+"))
    query_ok = len(re.findall(r"^Query OK,", body, re.MULTILINE))
    rows_in_set = len(re.findall(r"^\d+ rows? in set$", body, re.MULTILINE))
    errors = re.findall(r"^ERROR (\d+)", body, re.MULTILINE)
    last = body
    hits = sum(1 for v in EXPECT_VALUES if re.search(rf"(?<![\d.]){v}(?![\d.])", last))
    first = (chunks[0] if chunks else "").strip().split("\n")[0][:34] if chunks else ""
    return {
        "body": body,
        "bytes": len(body),
        "prompt": prompt_count,
        "query_ok": query_ok,
        "rows_in_set": rows_in_set,
        "borders": border,
        "pipes": pipes,
        "tabs": tabs,
        "errors": errors,
        "dist_hits": hits,
        "header": ("id" in body and "name" in body) or re.search(r"^d$", body, re.MULTILINE) is not None,
        "table": (border >= 3 and pipes >= 3) or tabs >= 2,
        "first": first,
        "chunks": chunks,
    }


BORDER_RE = re.compile(r"^\+[-+]+\+$")


def score(f: dict) -> int:
    """Execution evidence only: the terminator decides whether SQL runs.

    The prompt / Query-OK expectations never change what the server emits, so
    they are reported as a separate "style" column and used only to describe
    how the harness must be configured -- not to rank framings.
    """
    total = 0
    total += 6 if f["dist_hits"] == len(EXPECT_VALUES) else 0
    total += 3 if f["table"] else 0
    total += 1 if f["header"] else 0
    total -= min(6, 2 * len(f["errors"]))
    total -= 6 if f["bytes"] == 0 else 0
    return total


def fmt_term(terminator: str) -> str:
    if not terminator:
        return "(none)"
    return terminator.replace("\n", "\\n")


def recommendation(name: str, terminator: str, f: dict) -> str:
    view = "True" if (f["query_ok"] or f["borders"]) else "False"
    prompt = 'r"^mysql> ?"' if f["prompt"] else '""'
    drop = '["^Query OK,", "^\\\\d+ rows? in set$"]' if f["query_ok"] and not view else "[]"
    feed = "bulk" if name == "bulk-eof" else "per_statement"
    term = terminator or ";\n"
    if feed == "bulk" and term in (";", ""):
        term = ";\n"
    return "\n".join([
        "PROTOCOL = {",
        f'    "stmt_terminator": {term!r},',
        '    "echo_statements": False,',
        f'    "mysqltest_view": {view},',
        f'    "strip_prompt": {prompt},',
        f'    "drop_lines": {drop},',
        f'    "feed": {feed!r},',
        "}",
        f"# best framing: {name}",
    ])


def preview(text: str, limit: int = 14) -> str:
    lines = text.split("\n")
    shown, suffix = lines[:limit], ""
    if len(lines) > limit:
        suffix = f"\n    ... ({len(lines) - limit} more lines)"
    return "\n".join("    " + l for l in shown) + suffix


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="probe.py", description=__doc__.splitlines()[0])
    parser.add_argument("--binary", default="./minivec")
    parser.add_argument("--show", action="store_true", help="print raw output per framing")
    parser.add_argument("--keep", action="store_true", help="keep probe data dirs")
    parser.add_argument("--set", dest="overrides", action="append", metavar="KEY=VALUE")
    args = parser.parse_args(argv)

    binary = args.binary
    if not os.path.isfile(binary):
        sys.stderr.write(f"probe: {binary} not found -- build it first (make -j)\n")
        return 3

    protocol = dict(PROTOCOL)
    protocol["binary"] = binary
    for item in args.overrides or []:
        key, _, value = item.partition("=")
        protocol[key] = value

    rows = []
    for index, (name, terminator, expect_prompt, expect_query_ok, mode) in enumerate(FRAMINGS):
        data_dir = tempfile.mkdtemp(prefix=f"probe-{name}-")
        try:
            chunks, stderr = collect(protocol, terminator, mode, data_dir)
        except Exception as exc:  # a framing may legitimately kill the process
            chunks, stderr = [], f"{type(exc).__name__}: {exc}"
        finally:
            if not args.keep:
                shutil.rmtree(data_dir, ignore_errors=True)
        f = analyse(chunks)
        f["stderr"] = stderr
        f["match"] = bool(f["prompt"]) == expect_prompt and bool(f["query_ok"]) == expect_query_ok
        f["expect_prompt"] = expect_prompt
        f["expect_query_ok"] = expect_query_ok
        rows.append((score(f), index, name, terminator, f))

    # execution evidence first; on a tie keep the listed order, so a simple
    # framing (bare semicolon) wins over a fancier one that behaves identically
    rows.sort(key=lambda r: (-r[0], r[1]))
    print(f"binary: {binary}\n")
    head = (f"{'score':>5}  {'framing':<16} {'term':<8} {'bytes':>6} {'tbl':<5} "
            f"{'ok':>3} {'prm':>4} {'err':<6} style  first reply")
    print(head)
    print("-" * (len(head) + 12))
    for total, index, name, terminator, f in rows:
        style = "match" if f["match"] else "differs"
        print(f"{total:>5}  {name:<16} {fmt_term(terminator):<8} {f['bytes']:>6} "
              f"{str(f['table'])[:4]:<5} {f['query_ok']:>3} {f['prompt']:>4} "
              f"{','.join(f['errors'][:2]):<6} {style:<6} {f['first']}")

    best_score, best_index, best_name, best_term, best = rows[0]
    print(f"\nbest framing: {best_name} (score {best_score})")
    if best["bytes"] == 0:
        print("  -> no framing produced any output: the binary never answered stdin."
              " Check `./minivec --data DIR < file.sql` by hand.")
    elif best["errors"] and not best["table"]:
        print(f"  -> replies arrived but the probe SQL failed ({','.join(best['errors'][:3])});"
              " the dialect differs, fix PROBE_SQL before trusting the ranking.")
    elif not best["dist_hits"] == len(EXPECT_VALUES):
        print("  -> SQL ran but the expected L2 distances (0 and 5) were not both seen;"
              " L2_DISTANCE or vector literals may differ. Re-run with --show.")
    if best["prompt"]:
        print("  -> the server echoes a prompt; keep strip_prompt set.")
    if best["query_ok"] and not best["table"]:
        print('  -> "Query OK" without tables; enable mysqltest_view/drop_lines.')

    print("\nsuggested settings (paste into tools/harness.py PROTOCOL):")
    print("  " + recommendation(best_name, best_term, best).replace("\n", "\n  "))

    semicolon_ok = any(r[4]["bytes"] > 0 for r in rows if r[3] == ";\n")
    newline_ok = any(r[4]["bytes"] > 0 for r in rows if r[3] == "\n")
    if semicolon_ok and newline_ok:
        print("\nnote: both ';'-terminated and newline-terminated input got a reply,"
              " so the terminator is not significant for this binary.")
    elif semicolon_ok != newline_ok:
        good = ";" if semicolon_ok else "newline"
        print(f"\nnote: only {good}-terminated input got a reply.")

    if args.show:
        for total, index, name, terminator, f in rows:
            print(f"\n=== {name} (score {total}) ===")
            print(preview(f["body"]) if f["body"] else "    (no output)")
            stderr = (f.get("stderr") or "").strip()
            if stderr:
                print("    [stderr] " + stderr.splitlines()[-1][:200])
    return 0


if __name__ == "__main__":
    sys.exit(main())
