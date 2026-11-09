#!/usr/bin/env python3
"""tools/build_conversation.py -- build and police `agent/conversation.json`.

The scoring of the Agent track is a *ranking over the student's own evidence*
inside one JSON file (<= 128 KiB, UTF-8, top-level object).  Reviewers cite a
JSON pointer plus an exact snippet, so every claim in that file has to be
locatable: a citation is only worth anything if the quoted string really sits at
the pointer it names.

This tool does exactly three things -- and nothing else:

  1. reads the raw notes you wrote by hand in `agent/raw/` (markdown record
     blocks or JSONL, see docs/CONVERSATION-GUIDE.md),
  2. reorganises them into the four-dimension document,
  3. verifies the result and REFUSES to emit when a rule is broken.

It never invents a prompt, a measurement or a reflection.  Content comes from
the raw notes verbatim; the tool only moves it and checks it.  A document that
fails a rule is not written at all, so a broken file can never reach the branch.

Usage
-----
    python3 tools/build_conversation.py                  # agent/raw -> agent/conversation.json
    python3 tools/build_conversation.py --check          # validate only, write nothing
    python3 tools/build_conversation.py --check tools/samples/good
    python3 tools/build_conversation.py --verify agent/conversation.json
    python3 tools/build_conversation.py --stats

Exit codes: 0 = ok, 1 = a rule rejected the document (precise message on
stderr), 2 = usage error.

Rules enforced (codes appear in the messages)
---------------------------------------------
    E_UTF8                      a raw file is not valid UTF-8
    E_JSON                      a raw .jsonl line is not a JSON object
    E_JSON_DUP_KEY              a JSON object repeats a key
    E_NAN                       NaN / Infinity in JSON
    E_SYNTAX                    a markdown record block cannot be parsed
    E_KIND / E_ID               unknown `kind` / missing or duplicate `id`
    E_DIMENSION                 `dimension` is not one of the four dimensions
    E_MISSING_FIELD             a required field is absent or empty
    E_SCORE_HINT                score_hint is not one of 0.5 / 1 / 1.5 / 2
    E_EVIDENCE_TARGET           `target` does not name `<record-id>#<field>`
    E_POINTER                   a JSON pointer does not resolve
    E_POINTER_TYPE              a pointer resolves to something that is not a string
    E_QUOTE_EMPTY / E_QUOTE_TOO_LONG   a quote is empty or longer than 240 chars
    E_QUOTE_NOT_FOUND           the quote does not appear at its pointer  <-- the important one
    E_MISSING_RESULT_EVIDENCE   an instance is cited as evidence but records no result
    E_TOO_LARGE                 the file would exceed the 128 KiB budget
    E_TOP_LEVEL / E_SCHEMA      wrong shape for a top-level object
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

# --------------------------------------------------------------------------
# contract constants
# --------------------------------------------------------------------------
MAX_BYTES_DEFAULT = 128 * 1024          # the grader's hard ceiling
QUOTE_MAX_CHARS = 240                   # reviewers quote this much, no more
WARN_BYTES_RATIO = 0.80                 # start trimming at 80 % of the budget

DIM_UNDERSTAND = "问题理解与学习规划"
DIM_PROMPT = "Prompt 设计与迭代"
DIM_VERIFY = "验证纠错与独立判断"
DIM_RETRO = "总结复盘与知识迁移"
DIMENSIONS = [DIM_UNDERSTAND, DIM_PROMPT, DIM_VERIFY, DIM_RETRO]

DEFAULT_DIMENSION = {
    "prompt": DIM_UNDERSTAND,
    "conversation": DIM_PROMPT,
    "experiment": DIM_VERIFY,
    "retrospective": DIM_RETRO,
    "limitation": DIM_RETRO,
}

KIND_TO_ARRAY = {
    "prompt": "user_prompts",
    "conversation": "conversations",
    "experiment": "experiments",
    "retrospective": "retrospective",
    "limitation": "limitations",
}
KIND_ORDER = {kind: i for i, kind in enumerate(KIND_TO_ARRAY)}

REQUIRED_FIELDS = {
    "prompt": ["text"],
    "conversation": ["prompt", "decision"],
    "experiment": ["question", "change", "command", "observed"],
    "retrospective": ["text"],
    "limitation": ["text"],
}

# raw field name -> emitted field name, per kind (only rows that are renamed)
RENAME = {
    "prompt": {"text": "prompt"},
}

SCHEMA_ID = "agent-learning-evidence-v4"
TOP_LEVEL_REQUIRED = [
    "schema", "synthetic", "dimensions",
    "user_prompts", "conversations", "experiments", "retrospective", "limitations",
]

SCORE_HINTS = (0.5, 1.0, 1.5, 2.0)


# --------------------------------------------------------------------------
# errors
# --------------------------------------------------------------------------
class RuleError(Exception):
    """A rule rejected the document; carries the message printed to stderr."""

    def __init__(self, code: str, where: str, message: str, extra: list[str] | None = None):
        super().__init__(message)
        self.code = code
        self.where = where
        self.message = message
        self.extra = extra or []

    def render(self) -> str:
        head = f"error: {self.code} {self.where}" if self.where else f"error: {self.code}"
        lines = [head, f"  {self.message}"]
        for line in self.extra:
            lines.append(f"  {line}")
        return "\n".join(lines)


def fail(code: str, where: str, message: str, extra: list[str] | None = None):
    raise RuleError(code, where, message, extra)


# --------------------------------------------------------------------------
# strict JSON loading (duplicate keys, NaN)
# --------------------------------------------------------------------------
def _no_constants(token: str):
    raise ValueError(f"non-standard JSON constant {token!r}")


def _pairs_no_duplicates(pairs, where: str = ""):
    seen = {}
    for key, value in pairs:
        if key in seen:
            fail("E_JSON_DUP_KEY", where, f"duplicate key {key!r} in a JSON object")
        seen[key] = value
    return seen


def strict_json_loads(text: str, where: str):
    try:
        return json.loads(
            text,
            object_pairs_hook=lambda pairs: _pairs_no_duplicates(pairs, where),
            parse_constant=_no_constants,
        )
    except RuleError:
        raise
    except ValueError as exc:            # includes JSONDecodeError
        if "non-standard JSON constant" in str(exc):
            fail("E_NAN", where, str(exc))
        fail("E_JSON", where, f"invalid JSON: {exc}")


# --------------------------------------------------------------------------
# markdown record blocks: a deliberately small YAML subset
# --------------------------------------------------------------------------
FENCE_RE = re.compile(r"^`{3,}\s*record\s*$", re.IGNORECASE)
KEY_RE = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*):(?:[ \t]+(.*))?$")
BLOCK_SCALAR = {"|", "|-", "|+", ">", ">-"}


class Line:
    __slots__ = ("indent", "content", "lineno")

    def __init__(self, indent: int, content: str, lineno: int):
        self.indent = indent
        self.content = content
        self.lineno = lineno


def _tokenize(body: list[str], first_lineno: int) -> list[Line]:
    out: list[Line] = []
    for offset, raw in enumerate(body):
        expanded = raw.replace("\t", "    ")
        stripped = expanded.lstrip(" ")
        out.append(Line(len(expanded) - len(stripped), stripped, first_lineno + offset))
    return out


def _unquote(value: str) -> str:
    value = value.strip()
    if len(value) >= 2 and value[0] == value[-1] and value[0] in ("'", '"'):
        inner = value[1:-1]
        if value[0] == '"':
            return inner.replace('\\"', '"').replace("\\n", "\n").replace("\\\\", "\\")
        return inner.replace("''", "'")
    return value


def _collect_block_scalar(lines: list[Line], i: int, indent: int, where: str) -> tuple[str, int]:
    collected: list[list[str]] = []
    while i < len(lines) and (lines[i].indent > indent or lines[i].content == ""):
        collected.append([" " * lines[i].indent + lines[i].content, lines[i].lineno])
        i += 1
    while collected and collected[-1][0].strip() == "":
        collected.pop()
    if not collected:
        return "", i
    pad = min(len(text) - len(text.lstrip(" ")) for text, _ in collected if text.strip())
    body = "\n".join(text[pad:] if text.strip() else "" for text, _ in collected)
    return body.rstrip("\n"), i


def _is_seq_item(line: Line) -> bool:
    return line.content == "-" or line.content.startswith("- ")


def _parse_block(lines: list[Line], i: int, indent: int, where: str):
    if i >= len(lines):
        return {}, i
    if _is_seq_item(lines[i]):
        return _parse_sequence(lines, i, indent, where)
    return _parse_mapping(lines, i, indent, where)


def _parse_mapping(lines: list[Line], i: int, indent: int, where: str):
    result: dict = {}
    while i < len(lines):
        line = lines[i]
        if line.content.strip() == "":
            i += 1
            continue
        if line.content.startswith("#"):
            i += 1
            continue
        if line.indent < indent:
            break
        if line.indent > indent:
            fail("E_SYNTAX", f"{where}:{line.lineno}",
                 f"unexpected indentation ({line.indent} > {indent}): {line.content!r}")
        if _is_seq_item(line):
            break
        match = KEY_RE.match(line.content)
        if not match:
            fail("E_SYNTAX", f"{where}:{line.lineno}",
                 f"expected `key: value` inside a record, got {line.content!r}")
        key, rest = match.group(1), (match.group(2) or "")
        i += 1
        if key in result:
            fail("E_SYNTAX", f"{where}:{line.lineno}", f"duplicate field {key!r}")
        if rest.strip() in BLOCK_SCALAR:
            value, i = _collect_block_scalar(lines, i, indent, where)
            result[key] = value
        elif rest.strip() == "":
            if i < len(lines) and (lines[i].indent > indent) and lines[i].content.strip():
                value, i = _parse_block(lines, i, lines[i].indent, where)
                result[key] = value
            else:
                result[key] = ""
        else:
            result[key] = _unquote(rest)
    return result, i


def _parse_sequence(lines: list[Line], i: int, indent: int, where: str):
    result: list = []
    while i < len(lines):
        line = lines[i]
        if line.content.strip() == "":
            i += 1
            continue
        if line.content.startswith("#"):
            i += 1
            continue
        if line.indent != indent or not _is_seq_item(line):
            break
        rest = line.content[2:] if len(line.content) > 1 else ""
        item_indent = indent + 2
        if rest.strip() == "":
            i += 1
            if i < len(lines) and lines[i].indent > indent and lines[i].content.strip():
                value, i = _parse_block(lines, i, lines[i].indent, where)
            else:
                value = ""
        elif KEY_RE.match(rest):
            lines[i] = Line(item_indent, rest, line.lineno)     # rewrite `- key: v`
            value, i = _parse_mapping(lines, i, item_indent, where)
        else:
            value = _unquote(rest)
            i += 1
        result.append(value)
    return result, i


def _repo_candidates() -> list[Path]:
    """Project dir first, then the pushed repo root (agent/ sits one level up there)."""
    here = Path(__file__).resolve().parent.parent
    return [here, here.parent]


def _agent_dir() -> Path:
    for base in _repo_candidates():
        if (base / "agent" / "raw").is_dir():
            return base / "agent"
    return _repo_candidates()[0] / "agent"


def _display_path(path: Path) -> str:
    """Shortest repo-relative path for messages and `source` fields (never absolute)."""
    resolved = path.resolve()
    best: Path | None = None
    for base in _repo_candidates():
        try:
            rel = resolved.relative_to(base)
        except ValueError:
            continue
        if best is None or len(rel.parts) < len(best.parts):
            best = rel
    return best.as_posix() if best is not None else resolved.name


def parse_markdown(path: Path) -> list[dict]:
    shown = _display_path(path)
    try:
        text = path.read_text(encoding="utf-8")
    except UnicodeDecodeError as exc:
        fail("E_UTF8", shown, f"raw notes must be UTF-8: {exc}")
    lines = text.splitlines()
    records: list[dict] = []
    i = 0
    while i < len(lines):
        if not FENCE_RE.match(lines[i].strip()):
            i += 1
            continue
        start = i + 1
        j = start
        while j < len(lines) and not lines[j].strip().startswith("```"):
            j += 1
        if j >= len(lines):
            fail("E_SYNTAX", f"{shown}:{i + 1}", "record block is not closed with ```")
        body = _tokenize(lines[start:j], start + 1)
        parsed, consumed = _parse_block(body, 0, body[0].indent if body else 0, shown)
        if consumed != len(body):
            leftover = body[consumed]
            fail("E_SYNTAX", f"{shown}:{leftover.lineno}",
                 f"could not parse {leftover.content!r} (bad indentation?)")
        if not isinstance(parsed, dict):
            fail("E_SYNTAX", f"{shown}:{start + 1}", "a record block must be a mapping of fields")
        records.append({"__source__": f"{shown}:{start + 1}", "__raw__": parsed})
        i = j + 1
    return records


def parse_jsonl(path: Path) -> list[dict]:
    shown = _display_path(path)
    try:
        text = path.read_text(encoding="utf-8")
    except UnicodeDecodeError as exc:
        fail("E_UTF8", shown, f"raw notes must be UTF-8: {exc}")
    records: list[dict] = []
    for lineno, raw in enumerate(text.splitlines(), start=1):
        if not raw.strip():
            continue
        where = f"{shown}:{lineno}"
        value = strict_json_loads(raw, where)
        if not isinstance(value, dict):
            fail("E_JSON", where, f"each .jsonl line must be a JSON object, got {type(value).__name__}")
        records.append({"__source__": where, "__raw__": value})
    return records


def load_raw(input_dir: Path) -> list[dict]:
    if not input_dir.is_dir():
        fail("E_SYNTAX", str(input_dir), "raw notes directory does not exist")
    files = sorted(p for p in input_dir.rglob("*")
                   if p.is_file() and p.suffix.lower() in (".md", ".jsonl") and not p.name.startswith("."))
    records: list[dict] = []
    for path in files:
        if path.suffix.lower() == ".jsonl":
            records.extend(parse_jsonl(path))
        else:
            records.extend(parse_markdown(path))
    return records


# --------------------------------------------------------------------------
# document assembly
# --------------------------------------------------------------------------
def _require_text(raw: dict, field: str, where: str) -> str:
    value = raw.get(field)
    if not isinstance(value, str) or not value.strip():
        fail("E_MISSING_FIELD", where,
             f"field {field!r} is required and must be a non-empty string "
             f"(this is a {raw.get('kind')!r} record)")
    return value


def _optional_text(raw: dict, field: str, where: str) -> str:
    value = raw.get(field, "")
    if value is None:
        return ""
    if isinstance(value, (int, float)):
        return str(value)
    if not isinstance(value, str):
        fail("E_MISSING_FIELD", where, f"field {field!r} must be a string")
    return value


def _evidence_list(raw: dict, where: str) -> list[dict]:
    items = raw.get("evidence") or []
    if isinstance(items, dict):
        items = [items]
    if not isinstance(items, list):
        fail("E_MISSING_FIELD", where, "field 'evidence' must be a list of citations")
    cleaned = []
    for index, item in enumerate(items):
        if not isinstance(item, dict):
            fail("E_MISSING_FIELD", where, f"evidence[{index}] must be a mapping with target/pointer + quote")
        quote = item.get("quote")
        if not isinstance(quote, str):
            fail("E_QUOTE_EMPTY", where, f"evidence[{index}] has no 'quote' string")
        entry = {"quote": quote.strip()}
        if item.get("target"):
            entry["target"] = str(item["target"]).strip()
        elif item.get("pointer"):
            entry["pointer"] = str(item["pointer"]).strip()
        else:
            fail("E_EVIDENCE_TARGET", where,
                 f"evidence[{index}] needs `target: <record-id>#<field>` (or `pointer: /a/0/b`) "
                 "so the citation can be located")
        note = item.get("note")
        if isinstance(note, str) and note.strip():
            entry["note"] = note.strip()
        cleaned.append(entry)
    return cleaned


def _result_evidence(raw: dict, where: str) -> dict | None:
    value = raw.get("result_evidence")
    if value in (None, "", []):
        return None
    if isinstance(value, str):
        value = {"summary": value}
    if not isinstance(value, dict):
        fail("E_MISSING_FIELD", where, "result_evidence must be a mapping or a string")
    summary = value.get("summary")
    if not isinstance(summary, str) or not summary.strip():
        fail("E_MISSING_RESULT_EVIDENCE", where,
             "result_evidence has no non-empty 'summary' -- say what was measured")
    out = {"summary": summary.strip()}
    for field in ("command", "measured", "artifact"):
        text = value.get(field)
        if isinstance(text, str) and text.strip():
            out[field] = text.strip()
    if value.get("target"):
        out["target"] = str(value["target"]).strip()
    elif value.get("pointer"):
        out["pointer"] = str(value["pointer"]).strip()
    return out


def assemble(records: list[dict]) -> dict:
    """Turn raw records into the emitted document (citations still unresolved)."""
    parsed: list[dict] = []
    for entry in records:
        raw, where = entry["__raw__"], entry["__source__"]
        kind = raw.get("kind")
        if not isinstance(kind, str) or kind.strip().lower() not in KIND_TO_ARRAY:
            fail("E_KIND", where,
                 f"field 'kind' must be one of {', '.join(KIND_TO_ARRAY)}, got {kind!r}")
        kind = kind.strip().lower()
        rec_id = raw.get("id")
        if not isinstance(rec_id, str) or not rec_id.strip():
            fail("E_ID", where, "field 'id' is required (it is how citations point at this record)")
        rec_id = rec_id.strip()
        dimension = _optional_text(raw, "dimension", where).strip() or DEFAULT_DIMENSION[kind]
        if dimension not in DIMENSIONS:
            fail("E_DIMENSION", where,
                 f"dimension {dimension!r} is not one of the four dimensions",
                 ["  " + d for d in DIMENSIONS])
        for required in REQUIRED_FIELDS[kind]:
            _require_text(raw, required, where)
        score_hint = raw.get("score_hint")
        if score_hint not in (None, ""):
            try:
                score_hint = float(score_hint)
            except (TypeError, ValueError):
                fail("E_SCORE_HINT", where, f"score_hint must be a number, got {raw.get('score_hint')!r}")
            if score_hint not in SCORE_HINTS:
                fail("E_SCORE_HINT", where, f"score_hint must be one of 0.5 / 1 / 1.5 / 2, got {score_hint}")
        parsed.append({
            "kind": kind,
            "id": rec_id,
            "where": where,
            "dimension": dimension,
            "date": _optional_text(raw, "date", where).strip(),
            "raw": raw,
            "evidence": _evidence_list(raw, where),
            "result_evidence": _result_evidence(raw, where),
            "score_hint": score_hint if score_hint not in (None, "") else None,
        })

    # deterministic order: kind, date, id
    parsed.sort(key=lambda r: (KIND_ORDER[r["kind"]], r["date"], r["id"]))

    seen: dict[str, str] = {}
    for rec in parsed:
        if rec["id"] in seen:
            fail("E_ID", rec["where"],
                 f"duplicate id {rec['id']!r} (already defined at {seen[rec['id']]})")
        seen[rec["id"]] = rec["where"]

    doc: dict = {
        "schema": SCHEMA_ID,
        "synthetic": False,
        "dimensions": list(DIMENSIONS),
        "generator": {
            "tool": "tools/build_conversation.py",
            "rules": {
                "max_bytes": MAX_BYTES_DEFAULT,
                "quote_max_chars": QUOTE_MAX_CHARS,
                "score_per_instance": "0.5 / 1 / 1.5 / 2, decided by reviewers",
                "citation": "each evidence item carries json_pointer + quote; the quote must occur verbatim at that pointer",
            },
            "note": "content is transcribed from agent/raw/; this tool only reorganises and validates it",
        },
    }
    for array in KIND_TO_ARRAY.values():
        doc[array] = []

    index: dict[str, tuple[str, int, dict]] = {}
    for rec in parsed:
        array = KIND_TO_ARRAY[rec["kind"]]
        emitted: dict = {"id": rec["id"], "dimension": rec["dimension"]}
        if rec["date"]:
            emitted["date"] = rec["date"]
        raw = rec["raw"]
        for field in REQUIRED_FIELDS[rec["kind"]]:
            emitted[RENAME.get(rec["kind"], {}).get(field, field)] = raw[field].strip()
        for field in ("prompt_ref", "reflection", "conclusion", "note", "decision_detail"):
            text = _optional_text(raw, field, rec["where"]).strip()
            if text:
                emitted[field] = text
        if rec["result_evidence"]:
            emitted["result_evidence"] = rec["result_evidence"]
        emitted["evidence"] = rec["evidence"]          # resolved below
        if rec["score_hint"] is not None:
            emitted["score_hint"] = rec["score_hint"]
        emitted["source"] = rec["where"]
        doc[array].append(emitted)
        index[rec["id"]] = (array, len(doc[array]) - 1, emitted)

    _resolve_citations(doc, index)
    doc["summary"] = _summary(doc)
    return doc


def _resolve_citations(doc: dict, index: dict[str, tuple[str, int, dict]]) -> None:
    for array in KIND_TO_ARRAY.values():
        for pos, instance in enumerate(doc[array]):
            resolved = []
            for item in instance.get("evidence", []):
                pointer = _resolve_target(item, index, array, pos, instance)
                resolved.append({"json_pointer": pointer, "quote": item["quote"],
                                 **({"note": item["note"]} if item.get("note") else {})})
            instance["evidence"] = resolved
            result = instance.get("result_evidence")
            if result and (result.get("target") or result.get("pointer")):
                result["json_pointer"] = _resolve_target(result, index, array, pos, instance,
                                                         field="result_evidence")


def _resolve_target(item: dict, index: dict, array: str, pos: int, instance: dict,
                    field: str = "evidence") -> str:
    if "pointer" in item:
        pointer = item["pointer"]
        if not isinstance(pointer, str) or not pointer.startswith("/"):
            fail("E_POINTER", instance["source"],
                 f"{field}: {pointer!r} is not a JSON pointer (it must start with '/')")
        return pointer
    target = item["target"]
    if "#" not in target:
        fail("E_EVIDENCE_TARGET", instance["source"],
             f"{field}: target {target!r} must look like `<record-id>#<field>`",
             ["  example: target: p1#prompt   ->   /user_prompts/0/prompt"])
    rec_id, _, name = target.partition("#")
    rec_id, name = rec_id.strip(), name.strip()
    if rec_id not in index:
        fail("E_EVIDENCE_TARGET", instance["source"],
             f"{field}: target {target!r} names unknown record id {rec_id!r}",
             [f"  known ids: {', '.join(sorted(index))[:400]}"])
    target_array, target_pos, target_instance = index[rec_id]
    if name not in target_instance:
        fail("E_EVIDENCE_TARGET", instance["source"],
             f"{field}: record {rec_id!r} has no field {name!r}",
             [f"  fields: {', '.join(sorted(target_instance))}"])
    return f"/{target_array}/{target_pos}/{name}"


def _summary(doc: dict) -> dict:
    counts = {array: len(doc[array]) for array in KIND_TO_ARRAY.values()}
    per_dimension = {d: 0 for d in DIMENSIONS}
    citations = 0
    for array in KIND_TO_ARRAY.values():
        for instance in doc[array]:
            per_dimension[instance["dimension"]] = per_dimension.get(instance["dimension"], 0) + 1
            citations += len(instance.get("evidence") or [])
    scored = sum(1 for array in KIND_TO_ARRAY.values() for inst in doc[array] if inst.get("evidence"))
    return {
        "counts": counts,
        "per_dimension": per_dimension,
        "evidence_citations": citations,
        "scored_instances": scored,
        "score_range": {"min": scored * 0.5, "max": scored * 2.0},
        "score_note": "each instance with citations scores 0.5/1/1.5/2 at review time; "
                      "the range above is arithmetic, not a claimed score",
    }


# --------------------------------------------------------------------------
# pointer resolution and document validation
# --------------------------------------------------------------------------
def pointer_resolve(doc, pointer: str, where: str):
    if pointer in ("", "/"):
        return doc
    if not pointer.startswith("/"):
        fail("E_POINTER", where, f"{pointer!r} is not a JSON pointer")
    node = doc
    for token in pointer.split("/")[1:]:
        token = token.replace("~1", "/").replace("~0", "~")
        if isinstance(node, list):
            try:
                index = int(token)
            except ValueError:
                fail("E_POINTER", where, f"{pointer!r}: {token!r} is not a list index")
            if index < 0 or index >= len(node):
                fail("E_POINTER", where,
                     f"{pointer!r}: index {index} is out of range (array has {len(node)} item(s))")
            node = node[index]
        elif isinstance(node, dict):
            if token not in node:
                fail("E_POINTER", where,
                     f"{pointer!r}: no key {token!r}",
                     [f"  available: {', '.join(sorted(node))[:300]}"])
            node = node[token]
        else:
            fail("E_POINTER", where,
                 f"{pointer!r}: cannot descend into {type(node).__name__} at {token!r}")
    return node


def _closest(answer: str, quote: str) -> str:
    import difflib
    lines = [ln for ln in answer.splitlines() if ln.strip()]
    best = difflib.get_close_matches(quote, lines, n=1, cutoff=0.4)
    if best:
        return best[0][:QUOTE_MAX_CHARS]
    head = answer.strip()
    return head[:QUOTE_MAX_CHARS] + ("…" if len(head) > QUOTE_MAX_CHARS else "")


def check_citations(doc: dict) -> int:
    checked = 0
    for array in KIND_TO_ARRAY.values():
        for pos, instance in enumerate(doc.get(array, [])):
            where = f"/{array}/{pos}"
            for e_index, item in enumerate(instance.get("evidence") or []):
                if not isinstance(item, dict):
                    fail("E_QUOTE_NOT_FOUND", where, f"evidence[{e_index}] is not an object")
                pointer = item.get("json_pointer")
                quote = item.get("quote")
                if not isinstance(pointer, str) or not pointer:
                    fail("E_POINTER", where, f"evidence[{e_index}] has no json_pointer")
                if not isinstance(quote, str):
                    fail("E_QUOTE_EMPTY", where, f"evidence[{e_index}] has no quote string")
                if not quote.strip():
                    fail("E_QUOTE_EMPTY", f"{where}/evidence/{e_index}",
                         "the quote is empty -- a reviewer cannot cite nothing")
                if len(quote) > QUOTE_MAX_CHARS:
                    fail("E_QUOTE_TOO_LONG", f"{where}/evidence/{e_index}",
                         f"quote is {len(quote)} chars, the limit is {QUOTE_MAX_CHARS}",
                         [f"  quote: {quote[:120]}…"])
                target = pointer_resolve(doc, pointer, f"{where}/evidence/{e_index}")
                if not isinstance(target, str):
                    fail("E_POINTER_TYPE", f"{where}/evidence/{e_index}",
                         f"{pointer} holds a {type(target).__name__}, not a string",
                         ["  cite a text field (prompt/decision/observed/...), not a list or object"])
                if quote not in target:
                    fail("E_QUOTE_NOT_FOUND", f"{where}/evidence/{e_index}",
                         f"the quote does not appear verbatim at {pointer}",
                         [f"  quote: {quote[:QUOTE_MAX_CHARS]}",
                          f"  at {pointer}: {_closest(target, quote)}",
                          "  copy the string from the source character for character "
                          "(same punctuation, same whitespace)"])
                checked += 1
    return checked


def check_result_evidence(doc: dict) -> int:
    scored = 0
    for array in KIND_TO_ARRAY.values():
        for pos, instance in enumerate(doc.get(array, [])):
            where = f"/{array}/{pos}"
            citations = instance.get("evidence") or []
            result = instance.get("result_evidence")
            if citations:
                scored += 1
                if not isinstance(result, dict) or not str(result.get("summary", "")).strip():
                    fail("E_MISSING_RESULT_EVIDENCE", where,
                         f"instance {instance.get('id')!r} is cited as evidence "
                         f"({len(citations)} citation(s)) but records no result_evidence.summary",
                         ["  add the measurement: command run, numbers seen, artefact path"])
            if isinstance(result, dict) and result.get("json_pointer"):
                pointer_resolve(doc, result["json_pointer"], f"{where}/result_evidence")
    return scored


def verify_document(doc, size: int, max_bytes: int) -> dict:
    if not isinstance(doc, dict):
        fail("E_TOP_LEVEL", "", f"the document must be a JSON object, got {type(doc).__name__}")
    for key in TOP_LEVEL_REQUIRED:
        if key not in doc:
            fail("E_SCHEMA", "", f"top-level key {key!r} is missing",
                 [f"  required: {', '.join(TOP_LEVEL_REQUIRED)}"])
    if doc["synthetic"] is not False:
        fail("E_SCHEMA", "/synthetic",
             f"`synthetic` must be the literal false, got {doc['synthetic']!r}",
             ["  evidence produced by a tool or copied from a template must never be marked as real"])
    for array in KIND_TO_ARRAY.values():
        if not isinstance(doc[array], list):
            fail("E_SCHEMA", f"/{array}", f"{array} must be an array, got {type(doc[array]).__name__}")
    citations = check_citations(doc)
    scored = check_result_evidence(doc)
    if size > max_bytes:
        offenders = sorted(
            ((len(json.dumps(inst, ensure_ascii=False)), f"/{array}/{pos}")
             for array in KIND_TO_ARRAY.values()
             for pos, inst in enumerate(doc.get(array, []))),
            reverse=True,
        )[:5]
        fail("E_TOO_LARGE", "",
             f"the document is {size} bytes, the budget is {max_bytes} "
             f"({max_bytes - size:+d} bytes over)",
             ["  largest instances: " + ", ".join(f"{p} ({n} B)" for n, p in offenders),
              "  trim long quotes first: 240 characters is already the citation limit here, "
              "reviewers accept shorter ones"])
    return {"citations": citations, "scored": scored, "size": size}


# --------------------------------------------------------------------------
# commands
# --------------------------------------------------------------------------
def build_bytes(input_dir: Path, max_bytes: int) -> tuple[bytes, dict]:
    records = load_raw(input_dir)
    if not records:
        fail("E_SCHEMA", str(input_dir),
             "no records found -- drop markdown record blocks or .jsonl lines in there "
             "(docs/CONVERSATION-GUIDE.md)",
             ["  a `--check` on an empty directory is not a pass: there is nothing to score"])
    doc = assemble(records)
    payload = json.dumps(doc, ensure_ascii=False, indent=2) + "\n"
    data = payload.encode("utf-8")
    parsed = strict_json_loads(payload, "<generated>")
    stats = verify_document(parsed, len(data), max_bytes)
    stats["document"] = parsed
    return data, stats


def cmd_build(args) -> int:
    input_dir = Path(args.input)
    out = Path(args.out)
    shown_in = _display_path(input_dir)
    data, stats = build_bytes(input_dir, args.max_bytes)
    warn = []
    if len(data) >= args.max_bytes * WARN_BYTES_RATIO:
        warn.append(f"warning: {len(data)} of {args.max_bytes} bytes used "
                    f"({len(data) * 100 // args.max_bytes} %) -- trim before adding more")
    doc = stats["document"]
    counts = ", ".join(f"{k}={v}" for k, v in doc["summary"]["counts"].items())
    if args.check:
        for line in warn:
            print(line, file=sys.stderr)
        print(f"check: ok {shown_in} -> {len(data)} bytes, {counts}")
        print(f"       {stats['scored']} scored instance(s), "
              f"{stats['citations']} citation(s) verified against their pointers")
        return 0
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(data)
    for line in warn:
        print(line, file=sys.stderr)
    print(f"wrote {_display_path(out)} ({len(data)} bytes, limit {args.max_bytes})")
    print(f"  {counts}")
    print(f"  {stats['scored']} scored instance(s), {stats['citations']} citation(s) verified")
    return 0


def cmd_verify(args) -> int:
    path = Path(args.verify)
    if not path.is_file():
        fail("E_SCHEMA", str(path), "file does not exist")
    raw = path.read_bytes()
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError as exc:
        fail("E_UTF8", str(path), f"the file must be UTF-8: {exc}")
    doc = strict_json_loads(text, str(path))
    stats = verify_document(doc, len(raw), args.max_bytes)
    print(f"verify: ok {path} ({len(raw)} bytes, {stats['scored']} scored instance(s), "
          f"{stats['citations']} citation(s) verified)")
    return 0


def cmd_stats(args) -> int:
    data, stats = build_bytes(Path(args.input), args.max_bytes)
    doc = stats["document"]
    print(f"input {_display_path(Path(args.input))} -> {len(data)} bytes of {args.max_bytes}")
    for dim in DIMENSIONS:
        n = doc["summary"]["per_dimension"].get(dim, 0)
        print(f"  {dim}: {n} record(s)")
    print(f"  scored instances: {stats['scored']}  citations: {stats['citations']}")
    print(f"  arithmetic score range: {stats['scored'] * 0.5:g} .. {stats['scored'] * 2:g}")
    return 0


def main(argv: list[str] | None = None) -> int:
    agent = _agent_dir()
    parser = argparse.ArgumentParser(
        prog="build_conversation.py",
        description="build and validate agent/conversation.json from agent/raw/ notes",
    )
    parser.add_argument("input", nargs="?", default=str(agent / "raw"),
                        help="directory of raw notes (default: agent/raw)")
    parser.add_argument("--out", default=str(agent / "conversation.json"),
                        help="output file (default: agent/conversation.json)")
    parser.add_argument("--check", action="store_true",
                        help="validate only: report and exit, write nothing")
    parser.add_argument("--verify", metavar="FILE",
                        help="validate an existing document instead of building one")
    parser.add_argument("--stats", action="store_true",
                        help="print per-dimension counts without writing")
    parser.add_argument("--max-bytes", type=int, default=MAX_BYTES_DEFAULT,
                        help=f"size budget in bytes (default {MAX_BYTES_DEFAULT} = 128 KiB)")
    args = parser.parse_args(argv)

    try:
        if args.verify:
            return cmd_verify(args)
        if args.stats:
            return cmd_stats(args)
        return cmd_build(args)
    except RuleError as exc:
        print(exc.render(), file=sys.stderr)
        if not (args.check or args.verify):
            print("refusing to write the document: fix the finding above and re-run", file=sys.stderr)
        return 1
    except BrokenPipeError:
        return 0


if __name__ == "__main__":
    sys.exit(main())
