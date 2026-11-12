#!/usr/bin/env python3
"""tools/harness.py -- mysqltest-style driver for MiniVectorDB.

This harness exercises MiniVectorDB's optional plain-SQL adapter for local
engine regressions. Run it with MVDB_PROTOCOL=sql (run.sh does this for you).
The submitted entrypoint uses JSONL; use protocol_check/run.py for the
organizer's published protocol and fixtures. This local harness:

  * reads a ``.sql`` case (mysqltest syntax subset),
  * feeds the statements to ``./minivec --data <tmpdir>`` one at a time,
  * captures stdout,
  * compares it against the sibling ``.result`` file (or rewrites it).

Usage
-----
    tools/harness.py run    tests/cases/01_basic_crud.sql
    tools/harness.py run    tests/cases                  # whole directory
    tools/harness.py record tests/cases/01_basic_crud.sql
    tools/harness.py record tests/cases                  # (re)generate .result
    tools/harness.py list   tests/cases

Exit codes: 0 = pass, 1 = mismatch / assertion failure, 2 = usage error,
3 = binary missing or did not start.

Trailing horizontal whitespace is normalised for the local .result fixtures.
These SQL-text results are not evidence of official JSONL compatibility.

Directives understood
---------------------
    --echo <text>            write <text> into the expectation at this point
    --error <n>[,<n>...]     next statement is expected to fail with one of codes
    --sorted_result          sort the rows of the next statement's result
    --replace_column <n> <s> replace column <n> (1-based, header excluded) with <s>
    --delimiter <s>          change the statement delimiter
    --source <file>          inline another case file (relative to this one)
    --restart                kill the server and respawn it on the SAME data dir
    --sleep <sec>            pause before the next statement
    --disable_query_log / --enable_query_log / --skip  accepted and honoured/ignored
    #  and  -- <comment>      comment lines (not sent to the server)

LOCAL ADAPTER SETTINGS
----------------------
Everything the harness believes about the wire format lives in ``PROTOCOL``
below, plus the ``MYSQLTEST_VIEW`` switch. These settings do not select or
modify the official JSONL contract.
"""

from __future__ import annotations

import argparse
import difflib
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path

# --------------------------------------------------------------------------
# PROTOCOL: single source of truth for the wire format.
# --------------------------------------------------------------------------
PROTOCOL = {
    # how the server binary is invoked; {data} is the data directory
    "binary": "./minivec",
    "args": ["--data", "{data}"],
    # extra environment for the child (e.g. {"MVDB_OUT": "table"} when the
    # binary switches between mysqltest TSV and boxed-table output)
    "env": {},
    # appended to every statement before writing it to stdin.
    # ";\n"  = semicolon terminated + newline (what src/main.cpp expects)
    # "\n"   = newline terminated (no semicolon)
    # ""     = raw statement, no terminator
    "stmt_terminator": ";\n",
    # mysqltest logs every query into the .result file when query logging is on.
    # Our binary does not echo, so this is False; flip to True if the official
    # driver's .result files turn out to contain the statements themselves.
    "echo_statements": False,
    # regex deleted from the start of every response line (e.g. r"^mysql> ?")
    "strip_prompt": "",
    # regexes of whole response lines to delete (e.g. "^Query OK,")
    "drop_lines": [],
    # rewrite the response into mysqltest .result form: drop "Query OK"/"N rows
    # in set" lines and render tables as tab-separated header + rows.
    "mysqltest_view": False,
    # read timing: emit nothing if no byte arrives within idle_ms; otherwise
    # keep reading until settle_ms of silence.
    "idle_ms": 300,
    "settle_ms": 80,
    # "per_statement" = write one statement, read its reply (attribution on);
    # "bulk" = write a whole segment, close stdin, read until EOF (used when the
    #          server only flushes at EOF; per-statement directives such as
    #          --sorted_result / --replace_column then cannot apply).
    # "auto" = probe once per binary and pick one of the above.
    "feed": "auto",
    # optional stderr marker meaning "database opened, ready for statements"
    "ready_marker": "",
    "startup_s": 15.0,
    "exit_grace_s": 10.0,
}

BORDER_RE = re.compile(r"^\+[-+]+\+$")
ERROR_RE = re.compile(r"\bERROR (\d+)\b")
ROWS_IN_SET_RE = re.compile(r"^\d+ rows? in set$")
QUERY_OK_RE = re.compile(r"^Query OK,")

KNOWN_DIRECTIVES = {
    "echo", "error", "sorted_result", "replace_column", "delimiter", "source",
    "restart", "sleep", "skip", "disable_query_log", "enable_query_log",
    "disable_abort_on_error", "enable_abort_on_error", "disable_result_log",
    "enable_result_log", "disable_warnings", "enable_warnings", "require",
    "let", "exec", "write_file", "remove_file", "connection", "connect",
}


class UsageError(Exception):
    pass


class ServerError(Exception):
    pass


# --------------------------------------------------------------------------
# Normalisation (borrowed verbatim in spirit from the official seekdb script)
# --------------------------------------------------------------------------
def normalize_trailing_horizontal_whitespace(content: bytes) -> bytes:
    lines = content.split(b"\n")
    for index, line in enumerate(lines):
        if line.endswith(b"\r"):
            lines[index] = line[:-1].rstrip(b" \t") + b"\r"
        else:
            lines[index] = line.rstrip(b" \t")
    return b"\n".join(lines)


def to_mysqltest_view(text: str) -> str:
    """Turn mysql-client output into mysqltest .result form.

    ``+-----+`` borders, ``Query OK, ...`` and ``N rows in set`` lines are
    dropped; ``| a | b |`` rows become tab separated ``a<TAB>b``.
    """
    out: list[str] = []
    for line in text.split("\n"):
        if BORDER_RE.match(line):
            continue
        if QUERY_OK_RE.match(line) or ROWS_IN_SET_RE.match(line):
            continue
        if len(line) >= 2 and line.startswith("|") and line.endswith("|"):
            cells = [c.strip() for c in line[1:-1].split("|")]
            out.append("\t".join(cells))
            continue
        out.append(line)
    return "\n".join(out)


# --------------------------------------------------------------------------
# Result table view: lets --sorted_result / --replace_column re-render a
# bordered table with correct column widths.
# --------------------------------------------------------------------------
@dataclass
class TableView:
    pre: list[str] = field(default_factory=list)
    header: list[str] = field(default_factory=list)
    rows: list[list[str]] = field(default_factory=list)
    post: list[str] = field(default_factory=list)
    style: str = "none"           # bordered | plain | none
    align: list[bool] = field(default_factory=list)
    final_newline: bool = True


def _cells_from_bordered(line: str) -> tuple[list[str], list[tuple[str, int, int]]]:
    raw = line[1:-1].split("|")
    values, padding = [], []
    for piece in raw:
        stripped = piece.strip()
        lead = len(piece) - len(piece.lstrip(" "))
        trail = len(piece) - len(piece.rstrip(" "))
        values.append(stripped)
        padding.append((stripped, lead, trail))
    return values, padding


def parse_view(text: str) -> TableView:
    view = TableView()
    view.final_newline = text.endswith("\n")
    lines = text.split("\n")
    if view.final_newline:
        lines = lines[:-1]

    first_border = next((i for i, l in enumerate(lines) if BORDER_RE.match(l)), None)
    if first_border is not None:
        view.style = "bordered"
        view.pre = lines[:first_border]
        j = first_border + 1
        pad_rows: list[list[tuple[str, int, int]]] = []
        while j < len(lines):
            line = lines[j]
            if BORDER_RE.match(line):
                j += 1
                continue
            if len(line) >= 2 and line.startswith("|") and line.endswith("|"):
                cells, padding = _cells_from_bordered(line)
                if not view.header:
                    view.header = cells
                else:
                    view.rows.append(cells)
                    pad_rows.append(padding)
                j += 1
                continue
            break
        view.post = lines[j:]
        ncol = len(view.header)
        view.align = [False] * ncol
        for c in range(ncol):
            lead = trail = 0
            for p in pad_rows:
                if c < len(p):
                    lead += p[c][1]
                    trail += p[c][2]
            view.align[c] = lead > trail
        return view

    if any("\t" in l for l in lines):
        view.style = "plain"
        for i, line in enumerate(lines):
            cells = line.split("\t")
            if not view.header:
                view.header = cells
            elif len(cells) == len(view.header):
                view.rows.append(cells)
            else:
                view.pre = lines[:i]
                view.post = lines[i:]
                break
        view.align = [False] * len(view.header)
        return view

    view.style = "none"
    view.post = lines
    return view


def render_view(view: TableView) -> str:
    if view.style == "none":
        lines = list(view.pre) + list(view.post)
        text = "\n".join(lines)
        return text + "\n" if view.final_newline else text

    if view.style == "plain":
        lines = list(view.pre) + ["\t".join(view.header)]
        lines += ["\t".join(r) for r in view.rows]
        lines += list(view.post)
        text = "\n".join(lines)
        return text + "\n" if view.final_newline else text

    ncol = len(view.header)
    width = [len(h) for h in view.header]
    for row in view.rows:
        for c in range(ncol):
            if c < len(row):
                width[c] = max(width[c], len(row[c]))

    def border() -> str:
        return "+" + "+".join("-" * (w + 2) for w in width) + "+"

    lines = list(view.pre) + [border()]
    cells = []
    for c in range(ncol):
        pad = width[c] - len(view.header[c])
        cells.append(" " + view.header[c] + " " * pad + " ")
    lines.append("|" + "|".join(cells) + "|")
    lines.append(border())
    for row in view.rows:
        cells = []
        for c in range(ncol):
            value = row[c] if c < len(row) else ""
            pad = width[c] - len(value)
            if c < len(view.align) and view.align[c]:
                cells.append(" " + " " * pad + value + " ")
            else:
                cells.append(" " + value + " " * pad + " ")
        lines.append("|" + "|".join(cells) + "|")
    lines.append(border())
    lines += list(view.post)
    text = "\n".join(lines)
    return text + "\n" if view.final_newline else text


# --------------------------------------------------------------------------
# Case file parsing
# --------------------------------------------------------------------------
@dataclass
class Query:
    sql: str
    echo_text: str
    line: int
    errors: list[int] | None = None
    sorted_result: bool = False
    replace: list[tuple[int, str]] = field(default_factory=list)


@dataclass
class Echo:
    text: str


@dataclass
class Restart:
    line: int


@dataclass
class Sleep:
    seconds: float


DIRECTIVE_RE = re.compile(r"^--\s*([A-Za-z_][A-Za-z0-9_]*)\s*(.*)$")


def _comment_line(stripped: str) -> bool:
    if stripped.startswith("#"):
        return True
    if stripped == "--":
        return True
    return stripped.startswith("--") and len(stripped) > 2 and stripped[2] in " \t"


def split_statements(buf: str, delimiter: str) -> tuple[list[str], str]:
    """Split ``buf`` on ``delimiter`` outside quotes/backticks."""
    pieces: list[str] = []
    i = 0
    start = 0
    in_single = in_double = in_backtick = False
    n = len(buf)
    while i < n:
        c = buf[i]
        if in_single:
            if c == "\\":
                i += 2
                continue
            if c == "'":
                if i + 1 < n and buf[i + 1] == "'":
                    i += 2
                    continue
                in_single = False
            i += 1
            continue
        if in_double:
            if c == "\\":
                i += 2
                continue
            if c == '"':
                in_double = False
            i += 1
            continue
        if in_backtick:
            if c == "`":
                in_backtick = False
            i += 1
            continue
        if c == "'":
            in_single = True
        elif c == '"':
            in_double = True
        elif c == "`":
            in_backtick = True
        elif buf.startswith(delimiter, i):
            pieces.append(buf[start:i])
            i += len(delimiter)
            start = i
            continue
        i += 1
    return pieces, buf[start:]


def parse_case(path: Path, warn, _stack: tuple[Path, ...] = ()) -> list:
    path = path.resolve()
    if path in _stack:
        raise UsageError(f"--source cycle: {path}")
    if not path.is_file():
        raise UsageError(f"case file not found: {path}")

    text = path.read_text(encoding="utf-8", errors="replace")
    cmds: list = []
    delimiter = ";"
    pending = ""
    line_no = 0
    next_errors: list[int] | None = None
    next_sorted = False
    next_replace: list[tuple[int, str]] = []

    def emit(statement: str, at_line: int) -> None:
        nonlocal next_errors, next_sorted, next_replace
        if not statement.strip():
            return
        cmds.append(
            Query(
                sql=statement.strip(),
                echo_text=statement.strip() + delimiter,
                line=at_line,
                errors=next_errors,
                sorted_result=next_sorted,
                replace=list(next_replace),
            )
        )
        next_errors, next_sorted, next_replace = None, False, []

    for raw_line in text.split("\n"):
        line_no += 1
        stripped = raw_line.strip()
        directive = DIRECTIVE_RE.match(stripped) if stripped.startswith("--") else None
        is_directive = (
            not pending.strip()
            and directive is not None
            and directive.group(1).lower() in KNOWN_DIRECTIVES
        )
        if not pending.strip() and not is_directive and _comment_line(stripped):
            continue                     # plain comment (# or -- )
        if is_directive:
            name, rest = directive.group(1).lower(), directive.group(2).strip()
            if name == "echo":
                cmds.append(Echo(text=rest))
            elif name == "error":
                codes = [int(x) for x in re.findall(r"\d+", rest)]
                next_errors = codes or [0]
            elif name == "sorted_result":
                next_sorted = True
            elif name == "replace_column":
                bits = rest.split(None, 1)
                if not bits or not bits[0].isdigit():
                    warn(f"{path}:{line_no}: bad --replace_column")
                else:
                    next_replace.append((int(bits[0]), bits[1] if len(bits) > 1 else ""))
            elif name == "delimiter":
                if not rest:
                    warn(f"{path}:{line_no}: --delimiter without value")
                else:
                    delimiter = rest
            elif name == "source":
                target = Path(rest)
                if not target.is_absolute():
                    target = path.parent / target
                cmds.extend(parse_case(target, warn, _stack + (path,)))
            elif name == "restart":
                cmds.append(Restart(line=line_no))
            elif name == "sleep":
                try:
                    cmds.append(Sleep(seconds=float(rest or "1")))
                except ValueError:
                    warn(f"{path}:{line_no}: bad --sleep")
            continue

        if not pending.strip() and stripped.startswith("--") and directive is not None:
            warn(f"{path}:{line_no}: unknown directive --{directive.group(1)}, ignored")
            continue

        pending += raw_line + "\n"
        statements, pending = split_statements(pending, delimiter)
        for statement in statements:
            emit(statement, line_no)

    statements, pending = split_statements(pending, delimiter)
    for statement in statements:
        emit(statement, line_no)
    if pending.strip() and not _comment_line(pending.strip()):
        emit(pending, line_no)
    return cmds


# --------------------------------------------------------------------------
# Server process + per-statement response capture
# --------------------------------------------------------------------------
class Server:
    def __init__(self, protocol: dict, data_dir: str):
        self.protocol = protocol
        self.data_dir = data_dir
        self.proc: subprocess.Popen | None = None
        self._buf = bytearray()
        self._err = bytearray()
        self._eof = False
        self._cv = threading.Condition()
        self._threads: list[threading.Thread] = []

    # -- lifecycle ---------------------------------------------------------
    def _pump(self, stream, target, is_stdout: bool) -> None:
        try:
            while True:
                chunk = stream.read(65536)
                if not chunk:
                    break
                with self._cv:
                    target.extend(chunk)
                    if is_stdout:
                        self._cv.notify_all()
        finally:
            if is_stdout:
                with self._cv:
                    self._eof = True
                    self._cv.notify_all()

    def start(self) -> None:
        binary = self.protocol["binary"]
        if not os.path.isfile(binary):
            raise ServerError(f"binary not found: {binary} (run `make -j` first)")
        if not os.access(binary, os.X_OK):
            raise ServerError(f"binary not executable: {binary}")
        argv = [binary] + [a.format(data=self.data_dir) for a in self.protocol["args"]]
        env = dict(os.environ)
        env.update({str(k): str(v) for k, v in (self.protocol.get("env") or {}).items()})
        self._buf.clear()
        self._err.clear()
        self._eof = False
        self.proc = subprocess.Popen(
            argv,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            bufsize=0,
            env=env,
        )
        self._threads = [
            threading.Thread(target=self._pump, args=(self.proc.stdout, self._buf, True), daemon=True),
            threading.Thread(target=self._pump, args=(self.proc.stderr, self._err, False), daemon=True),
        ]
        for t in self._threads:
            t.start()

        marker = (self.protocol.get("ready_marker") or "").encode()
        deadline = time.time() + float(self.protocol["startup_s"])
        while time.time() < deadline:
            if self.proc.poll() is not None:
                raise ServerError(
                    f"server exited during startup (code {self.proc.returncode}): "
                    f"{self.stderr_text()[-800:]}"
                )
            if not marker or marker in bytes(self._err):
                return
            time.sleep(0.005)
        if marker:
            raise ServerError(f"startup marker {marker!r} never appeared; stderr={self.stderr_text()[-400:]}")

    def stderr_text(self) -> str:
        return bytes(self._err).decode("utf-8", "replace")

    def alive(self) -> bool:
        return self.proc is not None and self.proc.poll() is None

    def send(self, payload: bytes) -> bytes:
        self.write(payload)
        return self.read_until_quiet()

    def write(self, payload: bytes) -> None:
        if self.proc is None or self.proc.poll() is not None:
            raise ServerError(
                f"server is not running (exit {self.proc.returncode if self.proc else '?'}): "
                f"{self.stderr_text()[-400:]}"
            )
        self.proc.stdin.write(payload)
        self.proc.stdin.flush()

    def close_stdin(self) -> None:
        if self.proc is not None and self.proc.stdin is not None:
            try:
                self.proc.stdin.close()
            except OSError:
                pass

    def read_all(self, timeout_s: float = 60.0) -> bytes:
        """Drain stdout until the server closes it (or the timeout expires)."""
        out = bytearray()
        deadline = time.time() + timeout_s
        while time.time() < deadline:
            with self._cv:
                finished = self._eof and not self._buf
            out += self.read_until_quiet()
            if finished:
                break
            if self.proc is not None and self.proc.poll() is not None:
                out += self.read_until_quiet()
                break
        return bytes(out)

    def read_until_quiet(self) -> bytes:
        idle = float(self.protocol["idle_ms"]) / 1000.0
        settle = float(self.protocol["settle_ms"]) / 1000.0
        out = bytearray()
        seen = False
        deadline = time.time() + idle
        with self._cv:
            while True:
                if self._buf:
                    out += self._buf
                    self._buf.clear()
                    seen = True
                    deadline = time.time() + settle
                if self._eof and not self._buf:
                    break
                remaining = deadline - time.time()
                if remaining <= 0:
                    break
                self._cv.wait(min(remaining, 0.25))
        return bytes(out)

    def stop(self) -> int | None:
        if self.proc is None:
            return None
        try:
            self.proc.stdin.close()
        except OSError:
            pass
        grace = float(self.protocol["exit_grace_s"])
        try:
            code = self.proc.wait(timeout=grace)
        except subprocess.TimeoutExpired:
            self.proc.terminate()
            try:
                code = self.proc.wait(timeout=grace)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                code = self.proc.wait(timeout=grace)
        for t in self._threads:
            t.join(timeout=2.0)
        self.proc = None
        return code


# --------------------------------------------------------------------------
# Running a case
# --------------------------------------------------------------------------
def transform(chunk: bytes, query: Query | None, protocol: dict) -> str:
    text = chunk.decode("utf-8", "replace")
    prompt = protocol.get("strip_prompt") or ""
    if prompt:
        text = re.sub(prompt, "", text, flags=re.MULTILINE)
    for pattern in protocol.get("drop_lines") or []:
        text = "\n".join(l for l in text.split("\n") if not re.search(pattern, l))
    needs_view = bool(query and (query.sorted_result or query.replace))
    if needs_view and text.strip():
        view = parse_view(text)
        if query.sorted_result and view.rows:
            view.rows.sort(key=lambda r: "\t".join(r))
        for column, replacement in query.replace:
            index = column - 1
            for row in view.rows:
                if 0 <= index < len(row):
                    row[index] = replacement
        if view.style != "none":
            text = render_view(view)
    if protocol.get("mysqltest_view"):
        text = to_mysqltest_view(text)
    return text


def check_bulk_errors(pending: list[Query], text: str, notes: list[tuple[str, str]]) -> None:
    """Coarse --error verification when replies cannot be attributed.

    In bulk mode the whole segment's output arrives as one blob, so we can only
    compare the *number* of ERROR lines and their codes, in order.
    """
    expected = [q.errors for q in pending if q.errors is not None]
    actual = [int(m.group(1)) for m in ERROR_RE.finditer(text)]
    if len(actual) != len(expected):
        notes.append(
            ("FAIL", f"bulk: expected {len(expected)} error(s), server emitted {len(actual)}")
        )
        return
    for query, code in zip([q for q in pending if q.errors is not None], actual):
        if code not in (query.errors or []):
            notes.append(
                ("FAIL", f"line {query.line}: expected ERROR {query.errors}, got {code}")
            )


_FEED_CACHE: dict[tuple[str, str], str] = {}


def detect_feed(protocol: dict) -> str:
    """Decide whether the server answers while stdin stays open.

    ``src/main.cpp`` reads with ``fread(..., 65536, stdin)``, which blocks until
    the buffer is full *or* stdin reaches EOF.  Such a server cannot answer a
    statement before EOF, so the harness must feed a whole segment, close stdin
    and read to EOF ("bulk").  A server that flushes per statement is driven
    "per_statement" and keeps per-statement attribution (and --error checks).
    """
    key = (protocol["binary"], protocol["stmt_terminator"])
    if key in _FEED_CACHE:
        return _FEED_CACHE[key]
    data_dir = tempfile.mkdtemp(prefix="minivec-preflight-")
    feed = "per_statement"
    server = Server(protocol, data_dir)
    try:
        server.start()
        reply = server.send(b"SHOW TABLES" + protocol["stmt_terminator"].encode())
        if not reply.strip():
            feed = "bulk"
    except (ServerError, OSError):
        feed = "bulk"
    finally:
        server.stop()
        shutil.rmtree(data_dir, ignore_errors=True)
    _FEED_CACHE[key] = feed
    return feed


def run_case(case_path: Path, protocol: dict, data_dir: str | None = None,
             warn=None, timeout_s: float = 600.0) -> tuple[str, list[tuple[str, str]], str]:
    """Execute *case_path*; return (captured document, notes, data_dir)."""
    warn = warn or (lambda msg: None)
    notes: list[tuple[str, str]] = []
    cmds = parse_case(case_path, warn)
    own_dir = data_dir is None
    work_dir = data_dir or tempfile.mkdtemp(prefix="minivec-data-")
    Path(work_dir).mkdir(parents=True, exist_ok=True)

    doc_parts: list[str] = []
    deadline = time.time() + timeout_s
    server = Server(protocol, work_dir)
    bulk = (protocol.get("feed") or "per_statement") == "bulk"
    if (protocol.get("feed") or "auto") == "auto":
        bulk = detect_feed(protocol) == "bulk"
    pending_bulk: list[Query] = []

    def flush_bulk() -> None:
        """Send every buffered statement at once, close stdin, read to EOF."""
        if not pending_bulk:
            return
        if any(q.sorted_result or q.replace for q in pending_bulk):
            notes.append(("WARN", "bulk feed: --sorted_result/--replace_column ignored"))
        payload = "".join(q.sql + protocol["stmt_terminator"] for q in pending_bulk)
        server.start()
        server.write(payload.encode())
        server.close_stdin()
        text = transform(server.read_all(timeout_s), None, protocol)
        doc_parts.append(text)
        check_bulk_errors(pending_bulk, text, notes)
        code = server.stop()
        if code not in (0, None):
            notes.append(("WARN", f"server exit code {code} (bulk segment)"))
        pending_bulk.clear()

    try:
        if not bulk:
            server.start()
        for cmd in cmds:
            if time.time() > deadline:
                raise UsageError(f"case timeout after {timeout_s:.0f}s")
            if isinstance(cmd, Echo):
                if bulk:
                    flush_bulk()
                doc_parts.append(cmd.text + "\n")
                continue
            if isinstance(cmd, Sleep):
                time.sleep(cmd.seconds)
                continue
            if isinstance(cmd, Restart):
                if bulk:
                    flush_bulk()
                    continue
                code = server.stop()
                if code not in (0, None):
                    notes.append(("WARN", f"line {cmd.line}: --restart exit code {code}"))
                server.start()
                continue

            if bulk:
                pending_bulk.append(cmd)
                continue

            payload = (cmd.sql + protocol["stmt_terminator"]).encode()
            chunk = server.send(payload)
            text = transform(chunk, cmd, protocol)

            found = [int(m.group(1)) for m in ERROR_RE.finditer(text)]
            if cmd.errors is None:
                if found:
                    notes.append(("FAIL", f"line {cmd.line}: unexpected ERROR {found[0]}"))
            else:
                if not found:
                    notes.append(("FAIL", f"line {cmd.line}: expected ERROR {cmd.errors}, got none"))
                elif not any(code in cmd.errors for code in found):
                    notes.append(
                        ("FAIL", f"line {cmd.line}: expected ERROR {cmd.errors}, got {found[0]}")
                    )

            if protocol.get("echo_statements"):
                doc_parts.append(cmd.echo_text + "\n")
            doc_parts.append(text)
        if bulk:
            flush_bulk()
    finally:
        tail = server.stderr_text()
        exit_code = server.stop()
        if exit_code not in (0, None):
            notes.append(("WARN", f"server exit code {exit_code}; stderr tail: {tail[-300:].strip()}"))

    if own_dir and os.environ.get("HARNESS_KEEP_DATA"):
        notes.append(("INFO", f"data dir kept: {work_dir}"))
    elif own_dir:
        shutil.rmtree(work_dir, ignore_errors=True)

    return "".join(doc_parts), notes, work_dir


def result_path_for(case_path: Path) -> Path:
    return case_path.with_suffix(".result")


def diff_report(expected: str, actual: str, expected_name: str, actual_name: str) -> str:
    diff = difflib.unified_diff(
        expected.split("\n"),
        actual.split("\n"),
        fromfile=expected_name,
        tofile=actual_name,
        lineterm="",
        n=3,
    )
    return "\n".join(diff)


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------
def discover_cases(target: Path) -> list[Path]:
    if target.is_dir():
        return sorted(p for p in target.glob("*.sql") if p.is_file())
    return [target]


def check_binary(protocol: dict) -> None:
    binary = protocol["binary"]
    if not os.path.isfile(binary):
        sys.stderr.write(
            f"harness: {binary} not found -- build it first:  make -j\n"
        )
        raise SystemExit(3)
    if not os.access(binary, os.X_OK):
        sys.stderr.write(f"harness: {binary} is not executable\n")
        raise SystemExit(3)


def cmd_run(args, protocol: dict) -> int:
    check_binary(protocol)
    cases = discover_cases(Path(args.case))
    if not cases:
        raise UsageError("no .sql cases found")
    failures = 0
    for case_path in cases:
        expected_file = result_path_for(case_path)
        if not expected_file.is_file():
            print(f"[{case_path.name}] NO RESULT FILE ({expected_file.name}) -- run `record` first")
            failures += 1
            continue
        warn = lambda msg: print(f"warning: {msg}", file=sys.stderr)
        actual, notes, _ = run_case(case_path, protocol, warn=warn, timeout_s=args.timeout)
        expected_bytes = expected_file.read_bytes()
        actual_bytes = actual.encode("utf-8")
        same = normalize_trailing_horizontal_whitespace(
            expected_bytes
        ) == normalize_trailing_horizontal_whitespace(actual_bytes)
        hard = [n for n in notes if n[0] == "FAIL"]
        if args.allow_errors:
            hard = []
        for kind, msg in notes:
            print(f"  {kind}: {msg}")
        if same and not hard:
            print(f"[{case_path.name}] ok ({len(expected_bytes.splitlines())} lines)")
            continue
        failures += 1
        print(f"[{case_path.name}] FAILED")
        if not same:
            sys.stdout.write(
                diff_report(
                    expected_bytes.decode("utf-8", "replace"),
                    actual,
                    f"expected/{expected_file.name}",
                    f"actual/{case_path.stem}.out",
                )
                + "\n"
            )
        if args.dump:
            out = Path(args.dump) / f"{case_path.stem}.out"
            out.parent.mkdir(parents=True, exist_ok=True)
            out.write_text(actual, encoding="utf-8")
            print(f"  actual output written to {out}")
    if failures:
        print(f"\n{failures} case(s) failed")
        return 1
    print(f"\n{len(cases)} case(s) passed")
    return 0


def cmd_record(args, protocol: dict) -> int:
    check_binary(protocol)
    cases = discover_cases(Path(args.case))
    if not cases:
        raise UsageError("no .sql cases found")
    warn = lambda msg: print(f"warning: {msg}", file=sys.stderr)
    for case_path in cases:
        actual, notes, _ = run_case(case_path, protocol, warn=warn, timeout_s=args.timeout)
        target = result_path_for(case_path)
        if actual and not actual.endswith("\n"):
            actual += "\n"
        target.write_text(actual, encoding="utf-8")
        print(f"[{case_path.name}] recorded -> {target.name} ({len(actual.splitlines())} lines)")
        for kind, msg in notes:
            print(f"  {kind}: {msg}")
    return 0


def cmd_list(args, protocol: dict) -> int:
    cases = discover_cases(Path(args.case))
    if not cases:
        raise UsageError("no .sql cases found")
    width = max(len(p.name) for p in cases)
    for case_path in cases:
        result = result_path_for(case_path)
        if result.is_file():
            status = f"result {len(result.read_bytes().splitlines()):>4} lines"
        else:
            status = "result MISSING"
        print(f"{case_path.name:<{width}}  {status}")
    return 0


def apply_overrides(protocol: dict, items: list[str]) -> None:
    for item in items or []:
        if "=" not in item:
            raise UsageError(f"--set expects key=value, got {item!r}")
        key, value = item.split("=", 1)
        key = key.strip()
        if key not in protocol:
            raise UsageError(f"unknown protocol key {key!r}; known: {', '.join(sorted(protocol))}")
        default = protocol[key]
        if isinstance(default, bool):
            protocol[key] = value.strip().lower() in ("1", "true", "yes", "on")
        elif isinstance(default, int):
            protocol[key] = int(value)
        elif isinstance(default, float):
            protocol[key] = float(value)
        elif isinstance(default, list):
            protocol[key] = [v for v in value.split(",") if v] if value else []
        else:
            protocol[key] = value


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="harness.py",
        description="mysqltest-style driver for MiniVectorDB (stdin/stdout protocol)",
    )
    parser.add_argument("--binary", default=None, help="server binary (default ./minivec)")
    parser.add_argument("--set", dest="overrides", action="append", metavar="KEY=VALUE",
                        help="override a PROTOCOL entry (repeatable)")
    parser.add_argument("--env", dest="env", action="append", metavar="KEY=VALUE",
                        help="environment variable for the server binary (repeatable),"
                             " e.g. --env MVDB_OUT=table")
    sub = parser.add_subparsers(dest="command", required=True)

    for name in ("run", "record", "list"):
        p = sub.add_parser(name)
        p.add_argument("case", help="a .sql file or a directory of .sql files")
        if name == "run":
            p.add_argument("--allow-errors", action="store_true",
                           help="do not fail on --error assertion violations")
            p.add_argument("--dump", default=None, help="directory to write actual outputs")
            p.add_argument("--timeout", type=float, default=600.0)
        if name == "record":
            p.add_argument("--timeout", type=float, default=600.0)

    args = parser.parse_args(argv)
    protocol = dict(PROTOCOL)
    protocol["env"] = dict(PROTOCOL.get("env") or {})
    if args.binary:
        protocol["binary"] = args.binary
    for item in args.env or []:
        if "=" not in item:
            raise SystemExit(f"--env expects KEY=VALUE, got {item!r}")
        key, value = item.split("=", 1)
        protocol["env"][key] = value
    try:
        apply_overrides(protocol, args.overrides)
        handlers = {"run": cmd_run, "record": cmd_record, "list": cmd_list}
        return handlers[args.command](args, protocol)
    except UsageError as exc:
        sys.stderr.write(f"harness: {exc}\n")
        return 2
    except ServerError as exc:
        sys.stderr.write(f"harness: {exc}\n")
        return 3
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys.exit(main())
