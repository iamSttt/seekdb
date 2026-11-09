#!/usr/bin/env python3
"""tools/lint_packaging.py -- docker-less packaging lint for MiniVectorDB.

`docker build` is often unavailable (WSL here has no docker), but a broken
Dockerfile or a bad .dockerignore only shows up at grading time.  This linter
re-reads the packaging as text and enforces the properties that make the image
buildable and runnable by the grader:

  * two stages, both ubuntu:24.04, build -> runtime
  * build stage: build-essential + make, `make -j`, `strip`, and a build-time
    smoke test that fails when the binary does not answer
  * runtime stage: binary + libstdc++ + /data only, exec-form ENTRYPOINT with
    `--data /data`, no EXPOSE, no CMD, no compiler, no full-context COPY
  * every apt-get block installs with --no-install-recommends and drops the
    apt lists in the same RUN layer
  * .dockerignore is a whitelist that covers Makefile + src/ and keeps .git,
    tests/, tools/, docs/ and the prebuilt binary out of the context
  * the copy of the Dockerfile at the repository root is byte-identical

Usage
-----
    python3 tools/lint_packaging.py                 # lint the current repo
    python3 tools/lint_packaging.py --repo ../scaffold
    python3 tools/lint_packaging.py --repo . --also ../scaffold

Exit codes: 0 = clean, 1 = findings (printed as `file:line: RULE: message`),
2 = usage error.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

# top-level entries under the source directory that are deliberately kept out
# of the build context (and therefore need no .dockerignore whitelist entry)
CONTEXT_EXCLUDED = {
    "tests", "tools", "docs", "agent", ".git", "build", "data",
    "__pycache__", "node_modules",
}
SOURCE_SUFFIXES = {".cpp", ".cc", ".c", ".cxx", ".h", ".hpp"}
BASE_IMAGE = "ubuntu:24.04"


# --------------------------------------------------------------------------
# Dockerfile reader
# --------------------------------------------------------------------------
class Instruction:
    __slots__ = ("line", "text")

    def __init__(self, line: int, text: str) -> None:
        self.line = line
        self.text = text

    @property
    def keyword(self) -> str:
        return self.text.split(None, 1)[0].upper() if self.text.strip() else ""

    @property
    def rest(self) -> str:
        parts = self.text.split(None, 1)
        return parts[1] if len(parts) > 1 else ""


class Stage:
    def __init__(self, name: str, base: str, line: int) -> None:
        self.name = name
        self.base = base
        self.line = line
        self.instructions: list[Instruction] = []

    def text(self) -> str:
        return "\n".join(i.text for i in self.instructions)

    def find(self, needle: str) -> Instruction | None:
        for ins in self.instructions:
            if needle in ins.text:
                return ins
        return None

    def any(self, *needles: str) -> bool:
        joined = self.text()
        return any(n in joined for n in needles)


def read_dockerfile(path: Path, findings: list[str]) -> list[Stage]:
    raw = path.read_bytes()
    if b"\r\n" in raw:
        findings.append(f"{path}:1: EOL: CRLF line endings break RUN continuations; use LF")
    if not raw.endswith(b"\n"):
        findings.append(f"{path}:1: EOL: file does not end with a newline")
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError as exc:
        findings.append(f"{path}:1: UTF8: {exc}")
        return []

    stages: list[Stage] = []
    pending = ""
    pending_line = 0
    for lineno, line in enumerate(text.splitlines(), start=1):
        stripped = line.strip()
        if not pending and (not stripped or stripped.startswith("#")):
            continue
        if not pending:
            pending_line = lineno
        if stripped.endswith("\\"):
            # a trailing backslash is a Dockerfile-level line continuation: the
            # physical lines become ONE shell line, so join with a space (not "\n")
            pending += stripped[:-1] + " "
            continue
        pending += stripped
        _dispatch(pending, pending_line, stages)
        pending = ""
    if pending:
        _dispatch(pending, pending_line, stages)
    return stages


def _dispatch(chunk: str, lineno: int, stages: list[Stage]) -> None:
    instruction = Instruction(lineno, chunk)
    if instruction.keyword == "FROM":
        parts = instruction.rest.split()
        base = parts[0] if parts else ""
        name = ""
        for i, part in enumerate(parts):
            if part.upper() == "AS" and i + 1 < len(parts):
                name = parts[i + 1].strip()
        stages.append(Stage(name or f"stage{len(stages)}", base, lineno))
        return
    if not stages:
        return  # ARG before the first FROM
    stages[-1].instructions.append(instruction)


# --------------------------------------------------------------------------
# checks
# --------------------------------------------------------------------------
def check_stages(path: Path, stages: list[Stage], findings: list[str]) -> None:
    if len(stages) != 2:
        findings.append(
            f"{path}:1: STAGES: expected exactly 2 stages (build, runtime), found {len(stages)}"
        )
    for stage in stages:
        if stage.base != BASE_IMAGE:
            findings.append(
                f"{path}:{stage.line}: BASE: stage '{stage.name}' uses {stage.base!r}, "
                f"expected {BASE_IMAGE!r}"
            )
    names = [s.name for s in stages]
    if "build" not in names:
        findings.append(f"{path}:1: STAGES: no stage named 'build' (found: {', '.join(names)})")
    if "runtime" not in names:
        findings.append(f"{path}:1: STAGES: no stage named 'runtime' (found: {', '.join(names)})")
    elif stages[-1].name != "runtime":
        findings.append(
            f"{path}:{stages[-1].line}: STAGES: the last stage must be 'runtime', "
            f"found '{stages[-1].name}'"
        )


def stage_named(stages: list[Stage], name: str) -> Stage | None:
    for stage in stages:
        if stage.name == name:
            return stage
    return None


def check_build_stage(path: Path, stage: Stage, findings: list[str]) -> None:
    text = stage.text()
    if "build-essential" not in text:
        findings.append(
            f"{path}:{stage.line}: BUILD: build stage must apt-get install build-essential"
        )
    if "make" not in text:
        findings.append(f"{path}:{stage.line}: BUILD: build stage never calls make")
    if "make -j" not in text:
        findings.append(
            f"{path}:{stage.line}: BUILD: use a parallel build (`make -j\\\"$(nproc)\\\"`)"
        )
    if "strip" not in text:
        findings.append(f"{path}:{stage.line}: BUILD: the binary is never stripped")
    if "SHOW TABLES" not in text:
        findings.append(
            f"{path}:{stage.line}: SMOKE: build-time smoke test must pipe `SHOW TABLES;`"
        )
    # the smoke test must be able to FAIL the build
    if "smoke FAILED" not in text and "exit 1" not in text:
        findings.append(
            f"{path}:{stage.line}: SMOKE: smoke test cannot fail the build "
            "(no `exit 1` on a failed probe)"
        )
    if "-n \"$out\"" not in text and "-s " not in text:
        findings.append(
            f"{path}:{stage.line}: SMOKE: smoke test does not assert that the answer "
            "is non-empty"
        )
    if "test -x" not in text:
        findings.append(f"{path}:{stage.line}: BUILD: build stage never checks the binary exists")


def check_runtime_stage(path: Path, stage: Stage, findings: list[str]) -> None:
    text = stage.text()
    entry = stage.find("ENTRYPOINT")
    if entry is None:
        findings.append(f"{path}:{stage.line}: RUNTIME: no ENTRYPOINT")
    else:
        rest = entry.rest.strip()
        if not rest.startswith("["):
            findings.append(
                f"{path}:{entry.line}: RUNTIME: ENTRYPOINT must be exec-form JSON, got {rest!r}"
            )
        for needle in ("minivec", "--data", "/data"):
            if needle not in rest:
                findings.append(
                    f"{path}:{entry.line}: RUNTIME: ENTRYPOINT must carry {needle!r}"
                )
    if "EXPOSE" in text:
        findings.append(f"{path}:{stage.line}: RUNTIME: no EXPOSE -- the protocol is stdin/stdout")
    cmd = stage.find("CMD")
    if cmd is not None:
        findings.append(
            f"{path}:{cmd.line}: RUNTIME: CMD would append argv to the ENTRYPOINT; "
            "keep the entrypoint alone"
        )
    if "COPY --from=" not in text:
        findings.append(
            f"{path}:{stage.line}: RUNTIME: must COPY --from=<build stage>, not rebuild"
        )
    # word-boundary match: `libgcc-s1` is the C++ runtime dependency, `gcc` is a compiler
    compiler_re = re.compile(r"(?<![\w-])(build-essential|gcc|g\+\+|clang|git|make)(?![\w-])")
    for ins in stage.instructions:
        match = compiler_re.search(ins.text)
        if match:
            findings.append(
                f"{path}:{ins.line}: RUNTIME: {match.group(1)!r} does not belong in the runtime image"
            )
    if "libstdc++6" not in text:
        findings.append(f"{path}:{stage.line}: RUNTIME: libstdc++6 is missing (C++ runtime)")
    if "/data" not in text:
        findings.append(f"{path}:{stage.line}: RUNTIME: /data is neither created nor declared")
    for line in (i.text for i in stage.instructions):
        if line.upper().startswith("COPY . ") or line.strip() == "COPY . .":
            findings.append(
                f"{path}:{stage.line}: RUNTIME: copying the whole context into the runtime "
                "image ships tests and sources; copy the binary only"
            )


def check_apt(path: Path, stages: list[Stage], findings: list[str]) -> None:
    for stage in stages:
        for ins in stage.instructions:
            if not ins.keyword == "RUN":
                continue
            if "apt-get install" not in ins.text:
                continue
            if "apt-get update" not in ins.text:
                findings.append(
                    f"{path}:{ins.line}: APT: `apt-get install` without `apt-get update` "
                    "in the same layer"
                )
            if "--no-install-recommends" not in ins.text:
                findings.append(
                    f"{path}:{ins.line}: APT: use --no-install-recommends to keep the layer small"
                )
            if "rm -rf /var/lib/apt/lists" not in ins.text:
                findings.append(
                    f"{path}:{ins.line}: APT: clean /var/lib/apt/lists in the same RUN layer"
                )


def check_dockerignore(repo: Path, source_dir: str, findings: list[str]) -> None:
    path = repo / ".dockerignore"
    if not path.is_file():
        findings.append(f"{path}:1: IGNORE: missing .dockerignore (context would ship .git and tests)")
        return
    text = path.read_text(encoding="utf-8", errors="replace")
    lines = [ln.strip() for ln in text.splitlines() if ln.strip() and not ln.strip().startswith("#")]
    if not lines:
        findings.append(f"{path}:1: IGNORE: empty .dockerignore")
        return
    if lines[0] != "*":
        findings.append(
            f"{path}:1: IGNORE: start with `*` (whitelist) so nothing unlisted can leak "
            f"into the context; first rule is {lines[0]!r}"
        )
    allow = [ln[1:] for ln in lines if ln.startswith("!")]
    if not allow:
        findings.append(f"{path}:1: IGNORE: no `!` whitelist rules -- the build context is empty")

    def covered(rel: str) -> bool:
        for pat in allow:
            if pat == rel or pat.rstrip("/") == rel.rstrip("/"):
                return True
            if pat.endswith("/**") and rel.startswith(pat[:-2]):
                return True
        return False

    prefix = f"{source_dir}/" if source_dir else ""
    for required in (f"{prefix}Makefile", f"{prefix}src"):
        if not covered(required):
            findings.append(
                f"{path}:1: IGNORE: {required!r} is not whitelisted -- the build stage would "
                "find no sources"
            )
    for leak in (".git", "tests", "tools", "docs", "agent"):
        target = f"{prefix}{leak}" if prefix else leak
        if covered(target):
            findings.append(
                f"{path}:1: IGNORE: {target!r} is whitelisted into the build context; "
                "it must stay out"
            )


def check_context_coverage(repo: Path, source_dir: str, findings: list[str]) -> None:
    """Every source file must be reachable by the .dockerignore whitelist."""
    ignore = repo / ".dockerignore"
    if not ignore.is_file():
        return
    allow = [
        ln.strip()[1:]
        for ln in ignore.read_text(encoding="utf-8", errors="replace").splitlines()
        if ln.strip().startswith("!")
    ]
    src = repo / source_dir / "src" if source_dir else repo / "src"
    if not src.is_dir():
        findings.append(f"{src}:1: LAYOUT: source directory src/ not found")
        return
    dirs = sorted({p.parent for p in src.rglob("*") if p.suffix in SOURCE_SUFFIXES})
    for directory in dirs:
        rel = directory.relative_to(repo).as_posix()
        ok = any(pat.rstrip("/*") == rel or pat == f"{rel}/**" for pat in allow)
        if not ok:
            findings.append(
                f"{ignore}:1: IGNORE: directory {rel!r} holds sources but no whitelist rule "
                f"reaches it (add `!{rel}` and `!{rel}/**`)"
            )


def check_run_syntax(path: Path, stages: list[Stage], findings: list[str]) -> None:
    """Every RUN body is handed to /bin/sh: parse it here instead of at grading time."""
    import shutil
    import subprocess
    import tempfile

    sh = shutil.which("sh")
    if not sh:
        return
    for stage in stages:
        for ins in stage.instructions:
            if ins.keyword != "RUN":
                continue
            with tempfile.NamedTemporaryFile("w", suffix=".sh", delete=False, encoding="utf-8") as handle:
                handle.write(ins.text[len("RUN"):].lstrip())
                tmp = handle.name
            try:
                result = subprocess.run([sh, "-n", tmp], capture_output=True, text=True)
            finally:
                Path(tmp).unlink(missing_ok=True)
            if result.returncode != 0:
                detail = (result.stderr or "").strip().splitlines()
                findings.append(
                    f"{path}:{ins.line}: SHELL: this RUN is not valid /bin/sh syntax -- "
                    f"{detail[0] if detail else 'sh -n failed'}"
                )


# the SQL literals the build-time smoke test must use, mirrored in tools/ci.sh
SMOKE_LITERALS = [
    "SHOW TABLES",
    "CREATE TABLE smoke",
    "INSERT INTO smoke VALUES (1, 10), (2, 20)",
    "SELECT * FROM smoke",
]


def check_smoke_sync(dockerfile: Path, stages: list[Stage], findings: list[str]) -> None:
    """The image smoke test and the local one must probe the same thing."""
    docker_text = "\n".join(s.text() for s in stages)
    for literal in SMOKE_LITERALS:
        if literal not in docker_text:
            findings.append(
                f"{dockerfile}:1: SMOKE: the build-time smoke test no longer contains {literal!r}"
            )
    ci = dockerfile.parent / "tools" / "ci.sh"
    if not ci.is_file():
        return
    ci_text = ci.read_text(encoding="utf-8", errors="replace")
    for literal in SMOKE_LITERALS:
        if literal not in ci_text:
            findings.append(
                f"{ci}:1: SMOKE: tools/ci.sh does not run the same probe as the Dockerfile "
                f"({literal!r} missing) -- keep the two in sync"
            )


def lint_repo(repo: Path) -> list[str]:
    findings: list[str] = []
    dockerfile = repo / "Dockerfile"
    if not dockerfile.is_file():
        findings.append(f"{dockerfile}:1: LAYOUT: no Dockerfile -- the grader builds from here")
        return findings
    stages = read_dockerfile(dockerfile, findings)
    check_stages(dockerfile, stages, findings)
    build = stage_named(stages, "build")
    runtime = stage_named(stages, "runtime")
    if build:
        check_build_stage(dockerfile, build, findings)
    if runtime:
        check_runtime_stage(dockerfile, runtime, findings)
    check_apt(dockerfile, stages, findings)
    check_run_syntax(dockerfile, stages, findings)
    check_smoke_sync(dockerfile, stages, findings)

    source_dir = "minivec" if (repo / "minivec" / "Makefile").is_file() else ""
    if not (repo / source_dir / "Makefile" if source_dir else repo / "Makefile").is_file():
        findings.append(
            f"{repo}:1: LAYOUT: no Makefile at {'minivec/' if source_dir else ''}Makefile"
        )
    check_dockerignore(repo, source_dir, findings)
    check_context_coverage(repo, source_dir, findings)

    run_sh = repo / "run.sh" if not source_dir else repo / source_dir / "run.sh"
    if not run_sh.is_file():
        findings.append(f"{run_sh}:1: LAYOUT: run.sh is missing")
    return findings


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="lint_packaging.py",
        description="docker-less lint for Dockerfile / .dockerignore / run.sh",
    )
    parser.add_argument("--repo", action="append", default=None, metavar="PATH",
                        help="repository root to lint (repeatable, default: cwd)")
    parser.add_argument("--also", action="append", default=None, metavar="PATH",
                        help="additional repo whose Dockerfile must be byte-identical (repeatable)")
    parser.add_argument("--quiet", action="store_true", help="only print findings and the verdict")
    args = parser.parse_args(argv)

    repos = [Path(p) for p in (args.repo or ["."])]
    findings: list[str] = []
    reference: Path | None = None
    for repo in repos:
        findings.extend(lint_repo(repo))
        if reference is None:
            reference = repo / "Dockerfile"
    for extra in args.also or []:
        other = Path(extra) / "Dockerfile"
        if not other.is_file():
            findings.append(f"{other}:1: DRIFT: no Dockerfile to compare against")
            continue
        if reference is None or not reference.is_file():
            continue
        if other.read_bytes() != reference.read_bytes():
            findings.append(
                f"{other}:1: DRIFT: differs from {reference} -- copy the canonical file over, "
                "the two contexts must build the same image"
            )

    for line in findings:
        print(line, file=sys.stderr)
    if findings:
        print(f"lint_packaging: {len(findings)} finding(s)", file=sys.stderr)
        return 1
    if not args.quiet:
        print(f"lint_packaging: ok ({', '.join(str(r) for r in repos)})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
