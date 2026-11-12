#!/usr/bin/env python3
"""Validate this repository's attributed conversation material locally.

Checks content integrity and provenance references. This is a local checker,
not the organizer's evaluator, and it assigns no score.
"""

import argparse
import hashlib
import json
import math
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path


MAX_BYTES = 128 * 1024
SENSITIVE_PATTERNS = (
    re.compile(r"\bgh[pousr]_[A-Za-z0-9]{20,}\b"),
    re.compile(r"\bgithub_pat_[A-Za-z0-9_]{20,}\b"),
    re.compile(r"https?://[^/\s:@]+:[^/\s@]+@"),
    re.compile(r"-----BEGIN (?:[A-Z]+ )?PRIVATE KEY-----"),
    re.compile(r"(?i)\bauthorization\s*[:=]\s*bearer\s+\S+"),
    re.compile(r"\bsshpass\s+-p\s+\S+"),
)
SENSITIVE_KEY = re.compile(
    r"(?i)^(?:password|passwd|api[_-]?key|access[_-]?token|private[_-]?key|client[_-]?secret)$"
)


def require(condition, message):
    if not condition:
        raise ValueError(message)


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, f"duplicate JSON key {key!r}")
        result[key] = value
    return result


def reject_constant(token):
    raise ValueError(f"nonstandard JSON number {token}")


def pointer_child(pointer, key):
    return pointer + "/" + str(key).replace("~", "~0").replace("/", "~1")


def resolve_pointer(document, pointer):
    require(isinstance(pointer, str) and pointer.startswith("/"), "invalid evidence JSON pointer")
    value = document
    for encoded in pointer[1:].split("/"):
        require(not re.search(r"~(?![01])", encoded), f"invalid pointer escape at {pointer}")
        key = encoded.replace("~1", "/").replace("~0", "~")
        if isinstance(value, list):
            require(re.fullmatch(r"0|[1-9][0-9]*", key), f"invalid array index at {pointer}")
            index = int(key)
            require(index < len(value), f"array index out of range at {pointer}")
            value = value[index]
        else:
            require(isinstance(value, dict) and key in value, f"unresolved pointer {pointer}")
            value = value[key]
    return value


def inspect_value(document, value, pointer=""):
    if isinstance(value, str):
        # JSON escape sequences can otherwise smuggle unpaired surrogates into
        # an ASCII file that passes the initial UTF-8 byte decoding.
        value.encode("utf-8")
        for pattern in SENSITIVE_PATTERNS:
            require(not pattern.search(value), f"possible sensitive credential at {pointer}")
    elif isinstance(value, float):
        require(math.isfinite(value), f"non-finite number at {pointer}")
    elif isinstance(value, list):
        for index, item in enumerate(value):
            inspect_value(document, item, pointer_child(pointer, index))
    elif isinstance(value, dict):
        if "json_pointer" in value or "quote" in value:
            reference, quote = value.get("json_pointer"), value.get("quote")
            require(isinstance(quote, str) and quote, f"missing evidence quote at {pointer}")
            target = resolve_pointer(document, reference)
            require(isinstance(target, str) and quote in target,
                    f"evidence quote differs from its source at {pointer}")
        for key, item in value.items():
            child = pointer_child(pointer, key)
            require(not SENSITIVE_KEY.fullmatch(key), f"sensitive credential field at {child}")
            inspect_value(document, item, child)


def inspect_attribution(document):
    messages = document.get("messages")
    require(isinstance(messages, list) and messages, "messages must be a nonempty array")
    indexed = {}
    for message in messages:
        require(isinstance(message, dict), "message must be an object")
        ident = message.get("id")
        require(isinstance(ident, str) and ident and ident not in indexed, "missing/duplicate message id")
        require(message.get("role") in ("user", "assistant"), f"invalid role for message {ident}")
        require(isinstance(message.get("content"), str) and message["content"],
                f"missing content for message {ident}")
        indexed[ident] = message
    prompts = document.get("user_prompts")
    require(isinstance(prompts, list), "user_prompts must be an array")
    prompt_ids = set()
    for prompt in prompts:
        require(isinstance(prompt, dict), "user prompt must be an object")
        message = indexed.get(prompt.get("message_id"))
        require(message is not None and message["role"] == "user", "user prompt source is missing")
        require(message["id"] not in prompt_ids, "duplicate user prompt source")
        prompt_ids.add(message["id"])
        require(prompt.get("prompt") == message["content"], "user prompt differs from source message")
    require(prompt_ids == {ident for ident, message in indexed.items() if message["role"] == "user"},
            "user_prompts must preserve every selected user message")
    conversations = document.get("conversations")
    require(isinstance(conversations, list), "conversations must be an array")
    for conversation in conversations:
        require(isinstance(conversation, dict), "conversation summary must be an object")
        for field in ("user_message_ids", "assistant_message_ids"):
            require(isinstance(conversation.get(field), list), f"missing {field}")
            role = "user" if field == "user_message_ids" else "assistant"
            for ident in conversation[field]:
                require(ident in indexed and indexed[ident]["role"] == role,
                        f"incorrect message attribution for {ident}")
    require(document.get("synthetic") is False, "synthetic must explicitly be false")
    require(isinstance(document.get("summary"), str) and document["summary"].strip(),
            "summary must be a nonempty string")
    for name in ("experiments", "retrospective", "limitations"):
        require(isinstance(document.get(name), list), f"{name} must be an array")
    evidence = document.get("evidence_index")
    require(isinstance(evidence, list) and evidence, "evidence_index must be a nonempty array")
    for item in evidence:
        require(isinstance(item, dict) and "json_pointer" in item and "quote" in item,
                "evidence_index entry must contain a pointer and quote")


def artifact_path(path, pointer):
    # Use one portable repository-relative spelling. In particular, reject
    # Windows drive/ADS syntax and backslashes even when running on Linux.
    require(isinstance(path, str) and path and not path.startswith("/")
            and not any(c in path for c in ("\\", ":", "\0"))
            and all(part not in ("", ".", "..") for part in path.split("/")),
            f"unsafe artifact path at {pointer}")
    return path


def inspect_artifacts(document, repo):
    """Verify only current-repository experiment artifacts, not source locators.

    Git mode hashes the immutable, regular-file blob at the declared commit,
    independently of Windows checkout line endings. An exported archive has no
    commit database: its UTF-8 files can only receive an explicitly qualified
    LF-normalized content check, never a claim of verified commit provenance.
    """
    repo = Path(repo).resolve()
    defaults = document.get("artifact_integrity", {})
    require(isinstance(defaults, dict), "artifact_integrity must be an object")
    references = []
    for index, experiment in enumerate(document.get("experiments", [])):
        require(isinstance(experiment, dict), f"experiment {index} must be an object")
        artifacts = experiment.get("artifacts", [])
        require(isinstance(artifacts, list), f"experiment {index} artifacts must be an array")
        for number, item in enumerate(artifacts):
            pointer = f"/experiments/{index}/artifacts/{number}"
            require(isinstance(item, dict), f"artifact must be an object at {pointer}")
            path = artifact_path(item.get("path"), pointer)
            digest = item.get("sha256")
            require(isinstance(digest, str) and re.fullmatch(r"[0-9a-f]{64}", digest),
                    f"invalid artifact sha256 at {pointer}")
            basis = item.get("hash_basis", defaults.get("hash_basis"))
            require(basis == "git_blob", f"artifact hash_basis must be git_blob at {pointer}")
            revision = item.get("artifact_revision", defaults.get("artifact_revision"))
            require(isinstance(revision, str)
                    and re.fullmatch(r"(?:[0-9a-f]{40}|[0-9a-f]{64})", revision),
                    f"artifact_revision must be a full immutable commit ID at {pointer}")
            references.append((pointer, path, digest, revision))
    if not references:
        return {"count": 0, "mode": "none", "git_revisions_verified": False}

    # A .git file is normal for a worktree. Do not accidentally use an ancestor
    # repository when checking an archive extracted somewhere inside it.
    git_metadata = os.path.lexists(repo / ".git")
    git = shutil.which("git") if git_metadata else None
    require(not git_metadata or git is not None,
            "Git metadata exists but git is unavailable; cannot verify artifact revisions")
    revisions = {}
    unavailable = set()
    head = None
    cached = {}

    def run_git(*arguments, missing_ok=False):
        env = dict(os.environ, GIT_NO_LAZY_FETCH="1")
        try:
            result = subprocess.run(
                [git, "--no-pager", "--no-replace-objects", "-c", f"safe.directory={repo.as_posix()}",
                 "-C", str(repo), *arguments], capture_output=True, timeout=30, env=env)
        except (OSError, subprocess.TimeoutExpired) as exc:
            raise ValueError(f"cannot verify artifact Git object: {exc}") from exc
        if result.returncode != 0 and missing_ok:
            return None
        require(result.returncode == 0,
                "cannot verify artifact Git object: " + result.stderr.decode("utf-8", "replace").strip())
        return result.stdout

    if git_metadata:
        top = Path(os.fsdecode(run_git("rev-parse", "--show-toplevel")).strip()).resolve()
        require(top == repo, "artifact Git root differs from the material repository")

    for pointer, path, expected, revision in references:
        key = (revision, path)
        if key not in cached:
            if git_metadata:
                if revision not in revisions:
                    kind = run_git("cat-file", "-t", revision, missing_ok=True)
                    if kind is None:
                        # A normal depth=1 checkout may omit the historical
                        # provenance commit. Verify current committed bytes,
                        # but never label that as verification of the old ID.
                        if head is None:
                            head = run_git("rev-parse", "--verify", "HEAD^{commit}").decode("ascii").strip()
                        revisions[revision] = head
                        unavailable.add(revision)
                    else:
                        require(kind.strip() == b"commit",
                                f"artifact_revision does not name a commit at {pointer}")
                        commit = run_git("rev-parse", "--verify", revision + "^{commit}").decode("ascii").strip()
                        require(commit == revision, f"artifact revision is not an exact commit at {pointer}")
                        revisions[revision] = revision
                resolved_revision = revisions[revision]
                tree = run_git("ls-tree", "-z", resolved_revision, "--", path).split(b"\0")
                require(len(tree) == 2 and not tree[1], f"artifact is missing at {resolved_revision}:{path}")
                metadata, recorded_path = tree[0].split(b"\t", 1)
                mode, kind, _ = metadata.split()
                require(recorded_path == path.encode("utf-8") and kind == b"blob"
                        and mode in (b"100644", b"100755"),
                        f"artifact is not a regular Git file at {pointer}")
                payload = run_git("cat-file", "blob", resolved_revision + ":" + path)
            else:
                target = (repo / path).resolve(strict=True)
                require(target.is_relative_to(repo) and target.is_file(),
                        f"artifact resolves outside the archive or is not a file at {pointer}")
                payload = target.read_bytes()
                # Archive fallback is deliberately limited to these text
                # reports: never rewrite arbitrary binary bytes silently.
                payload.decode("utf-8")
                payload = payload.replace(b"\r\n", b"\n")
            cached[key] = hashlib.sha256(payload).hexdigest()
        require(cached[key] == expected,
                f"artifact sha256 mismatch at {pointer}: {path} ({revision})")
    return {"count": len(references),
            "mode": ("git_blob_current_head_fallback" if unavailable else "git_blob")
                    if git_metadata else "archive_lf_content_only",
            "git_revisions_verified": bool(git_metadata) and not unavailable,
            "revisions": sorted({item[3] for item in references}),
            "unverified_revisions": sorted(unavailable) if git_metadata
                                    else sorted({item[3] for item in references}),
            "resolved_revisions": revisions}


def validate(repo, artifact_report=None):
    paths = (repo / "conversation.json", repo / "agent" / "conversation.json")
    contents = []
    for path in paths:
        raw = path.read_bytes()
        require(0 < len(raw) <= MAX_BYTES, f"{path.name} must be 1..{MAX_BYTES} bytes")
        document = json.loads(raw.decode("utf-8"), object_pairs_hook=unique_object,
                              parse_constant=reject_constant)
        require(isinstance(document, dict), f"{path} must contain a JSON object")
        inspect_value(document, document)
        inspect_attribution(document)
        contents.append(raw)
    require(contents[0] == contents[1], "root and agent conversation.json differ")
    result = inspect_artifacts(document, repo)
    if artifact_report is not None:
        artifact_report.update(result)
    return len(contents[0]), hashlib.sha256(contents[0]).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
    args = parser.parse_args()
    artifacts = {}
    try:
        size, digest = validate(args.repo, artifacts)
    except (OSError, UnicodeError, ValueError) as exc:
        print(f"FAIL material integrity: {exc}", file=sys.stderr)
        return 1
    print(f"PASS material integrity: synchronized UTF-8 JSON, {size} bytes, sha256={digest}")
    if artifacts["mode"] == "git_blob":
        print(f"PASS artifact content: {artifacts['count']} immutable Git blobs verified")
    elif artifacts["mode"] == "git_blob_current_head_fallback":
        print(f"PASS artifact content: {artifacts['count']} committed Git blobs match")
        print("LIMITATION: historical revision objects unavailable; their commit identities were NOT verified")
        print("Artifact resolution: " + json.dumps(artifacts["resolved_revisions"], sort_keys=True))
    elif artifacts["mode"] == "archive_lf_content_only":
        print(f"PASS artifact content: {artifacts['count']} LF-normalized archive files match")
        print("LIMITATION: no Git metadata; declared commit identities were NOT verified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
