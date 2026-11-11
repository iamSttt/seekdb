#!/usr/bin/env python3
"""Validate this repository's attributed conversation material locally.

Checks content integrity and provenance references. This is a local checker,
not the organizer's evaluator, and it assigns no score.
"""

import argparse
import hashlib
import json
import math
import re
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


def validate(repo):
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
    return len(contents[0]), hashlib.sha256(contents[0]).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
    args = parser.parse_args()
    try:
        size, digest = validate(args.repo)
    except (OSError, UnicodeError, ValueError) as exc:
        print(f"FAIL material integrity: {exc}", file=sys.stderr)
        return 1
    print(f"PASS material integrity: synchronized UTF-8 JSON, {size} bytes, sha256={digest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
