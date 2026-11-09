#!/usr/bin/env bash
# tools/ci.sh -- the unattended gate for MiniVectorDB.
#
#   build -> smoke -> build-context simulation -> harness cases -> --record
#   drift check -> packaging lint -> evidence check
#
# Runs in WSL/Linux with NO docker: every packaging property that `docker build`
# would have proven is proven here by compiling from a copied context and by
# linting the Dockerfile as text (tools/lint_packaging.py).
#
#   bash tools/ci.sh              # full gate (what you run before pushing)
#   bash tools/ci.sh --fast       # skip the second (context-simulation) build
#   bash tools/ci.sh --lint-only   # packaging + evidence only, no compile
#   bash tools/ci.sh --sync        # copy packaging files into ../scaffold, then gate
#
# Exit codes: 0 = green, 1 = a gate failed, 2 = usage / bad environment.
# Environment: JOBS (parallel build width), HADOLINT (path to hadolint),
#              MVDB_SCAFFOLD (default ../scaffold).
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"

# never leave __pycache__ behind in the tree we push
export PYTHONDONTWRITEBYTECODE=1

SCAFFOLD=${MVDB_SCAFFOLD:-$ROOT/../scaffold}
JOBS=${JOBS:-$( (nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4) )}
FAST=0
NO_BUILD=0
LINT_ONLY=0
SYNC=0
STEPS=0
STARTED=$SECONDS
TMP_DIR=$(mktemp -d "${TMPDIR:-/tmp}/minivec-ci.XXXXXX")
FAILED_GATE=""

# --------------------------------------------------------------------------
# output helpers
# --------------------------------------------------------------------------
if [ -t 1 ]; then
  B=$'\033[1m'; G=$'\033[32m'; R=$'\033[31m'; Y=$'\033[33m'; N=$'\033[0m'
else
  B=""; G=""; R=""; Y=""; N=""
fi

step() { STEPS=$((STEPS + 1)); printf '\n%s== %02d. %s ==%s\n' "$B" "$STEPS" "$*" "$N"; }
ok()   { printf '   %sok%s   %s\n' "$G" "$N" "$*"; }
info() { printf '   %s..%s   %s\n' "$Y" "$N" "$*"; }
die() {
  FAILED_GATE=${FAILED_GATE:-step $STEPS}
  printf '   %sFAIL%s %s\n' "$R" "$N" "$*" >&2
  exit 1
}
cleanup() {
  local rc=$?
  if [ "$rc" -ne 0 ] || [ "${FAILED_GATE:-}" != "" ]; then
    printf '\n%sbash tools/ci.sh FAILED%s at %s after %ss (logs: %s)\n' \
      "$R" "$N" "${FAILED_GATE:-a gate}" "$((SECONDS - STARTED))" "$TMP_DIR"
    [ -n "${LOG:-}" ] && [ -f "$LOG" ] && { printf '\n--- last 40 lines of %s ---\n' "$LOG"; tail -40 "$LOG"; }
  else
    printf '\n%sbash tools/ci.sh: green%s -- %s step(s) in %ss\n' "$G" "$N" "$STEPS" "$((SECONDS - STARTED))"
    rm -rf "$TMP_DIR"
  fi
  exit "$rc"
}
trap cleanup EXIT

usage() {
  awk 'NR>1 && /^#/ {sub(/^# ?/, ""); print; next} NR>1 {exit}' "${BASH_SOURCE[0]}" >&2
  exit 2
}

while [ $# -gt 0 ]; do
  case "$1" in
    --fast) FAST=1; shift ;;
    --no-build) NO_BUILD=1; shift ;;
    --lint-only) LINT_ONLY=1; FAST=1; NO_BUILD=1; shift ;;
    --sync) SYNC=1; shift ;;
    -h|--help) usage ;;
    *) printf 'ci.sh: unknown argument %s\n' "$1" >&2; usage ;;
  esac
done

# --------------------------------------------------------------------------
# 0. preflight
# --------------------------------------------------------------------------
step "preflight (tools, cases, layout)"
for tool in bash python3 make g++; do
  command -v "$tool" >/dev/null 2>&1 || die "$tool is not installed"
done
python3 - <<'PY' || die "python3 >= 3.8 is required"
import sys
raise SystemExit(0 if sys.version_info >= (3, 8) else 1)
PY
[ -f Makefile ] || die "run from the project root (no Makefile in $PWD)"
# The Dockerfile lives with the project in the dev tree, but one level up in the
# pushed repository (repo root, source in minivec/). Accept both.
if [ -f Dockerfile ] && [ -f .dockerignore ]; then
  PKG_ROOT=$ROOT
elif [ -f ../Dockerfile ] && [ -f ../.dockerignore ]; then
  PKG_ROOT=$(cd .. && pwd)
else
  die "no Dockerfile + .dockerignore here ($PWD) or one level up"
fi
command -v docker >/dev/null 2>&1 || info "docker not installed -- packaging is proven by lint + context simulation"
HSQL=$(find tests/cases -maxdepth 1 -name '*.sql' -type f | wc -l)
[ "$HSQL" -gt 0 ] || die "no tests/cases/*.sql found"
ok "python3 $(python3 -c 'import platform; print(platform.python_version())'), $(g++ --version | head -1 | cut -d' ' -f1-3), jobs=$JOBS, $HSQL harness case(s), package=$PKG_ROOT"

# --------------------------------------------------------------------------
# 1. sync to the pushed repo (optional)
# --------------------------------------------------------------------------
if [ "$SYNC" = 1 ]; then
  step "sync packaging files into $SCAFFOLD"
  [ -d "$SCAFFOLD" ] || die "scaffold repo not found at $SCAFFOLD"
  install -m 0644 "$PKG_ROOT/Dockerfile" "$SCAFFOLD/Dockerfile"
  [ -f tools/scaffold/dockerignore.root ] || die "tools/scaffold/dockerignore.root is missing"
  install -m 0644 tools/scaffold/dockerignore.root "$SCAFFOLD/.dockerignore"
  mkdir -p "$SCAFFOLD/minivec/tools" "$SCAFFOLD/docs" "$SCAFFOLD/agent"
  if [ -f run.sh ]; then install -m 0755 run.sh "$SCAFFOLD/minivec/run.sh"; fi
  for f in tools/lint_packaging.py tools/build_conversation.py tools/ci.sh; do
    [ -f "$f" ] && install -m 0755 "$f" "$SCAFFOLD/minivec/tools/$(basename "$f")"
  done
  [ -d tools/samples ] && cp -a tools/samples "$SCAFFOLD/minivec/tools/"
  [ -d tools/scaffold ] && { mkdir -p "$SCAFFOLD/minivec/tools/scaffold"; cp -a tools/scaffold/. "$SCAFFOLD/minivec/tools/scaffold/"; }
  [ -d docs ] && cp -a docs/. "$SCAFFOLD/docs/"
  if [ -f "$PKG_ROOT/agent/conversation.json" ]; then
    install -m 0644 "$PKG_ROOT/agent/conversation.json" "$SCAFFOLD/agent/conversation.json"
  fi
  if [ -d "$PKG_ROOT/agent/raw" ]; then
    mkdir -p "$SCAFFOLD/agent/raw"
    cp -a "$PKG_ROOT/agent/raw/." "$SCAFFOLD/agent/raw/"
  fi
  # the repository root also carries a 0-byte conversation.json (scaffold leftover):
  # a grader reading that path would get an empty file, so mirror the real one there
  if [ -f "$SCAFFOLD/agent/conversation.json" ]; then
    install -m 0644 "$SCAFFOLD/agent/conversation.json" "$SCAFFOLD/conversation.json"
  fi
  ok "Dockerfile, .dockerignore, minivec/tools/, docs/, agent/conversation.json refreshed (src/ and tests/ untouched)"
fi

# --------------------------------------------------------------------------
# 2. compile
# --------------------------------------------------------------------------
if [ "$NO_BUILD" = 0 ]; then
  step "make -j$JOBS"
  LOG="$TMP_DIR/build.log"
  if ! make -j"$JOBS" >"$LOG" 2>&1; then
    tail -40 "$LOG" >&2
    die "make failed"
  fi
  grep -iE 'warning:' "$LOG" | head -5 || true
  [ -x ./minivec ] || die "make produced no executable ./minivec"
  ok "built $(stat -c %s ./minivec) bytes ($(grep -c 'warning:' "$LOG" || true) warning(s))"
else
  step "compile (skipped)"
  if [ "$LINT_ONLY" = 0 ]; then
    [ -x ./minivec ] || die "no ./minivec and --no-build was given"
    ok "using the existing ./minivec ($(stat -c %s ./minivec) bytes)"
  else
    info "lint-only: no compile, no harness, no recording"
  fi
fi

# --------------------------------------------------------------------------
# 3. build-time smoke test -- the same probes the Dockerfile runs
#    (keep in sync with the `smoke` RUN block in Dockerfile)
# --------------------------------------------------------------------------
if [ "$NO_BUILD" = 0 ]; then
  step "smoke test (SHOW TABLES + CRUD round trip)"
  smoke() { # $1 = binary, $2 = label
    local bin=$1 label=$2 dir out rc
    dir=$(mktemp -d "$TMP_DIR/smoke.XXXXXX")
    out=$(printf 'SHOW TABLES;\n' | timeout 30 "$bin" --data "$dir" 2>"$dir/err") && rc=0 || rc=$?
    [ "$rc" -eq 0 ] || die "$label: SHOW TABLES exited $rc ($(head -2 "$dir/err" | tr '\n' ' '))"
    [ -n "$out" ] || die "$label: SHOW TABLES produced no stdout answer"
    printf '%s' "$out" | grep -qiE 'segmentation|core dumped|terminate called' \
      && die "$label: crash marker in the SHOW TABLES answer"
    rm -rf "$dir"; dir=$(mktemp -d "$TMP_DIR/smoke.XXXXXX")
    out=$(printf 'CREATE TABLE smoke (id BIGINT PRIMARY KEY, n BIGINT);\nINSERT INTO smoke VALUES (1, 10), (2, 20);\nSELECT * FROM smoke;\n' \
      | timeout 30 "$bin" --data "$dir" 2>"$dir/err") && rc=0 || rc=$?
    [ "$rc" -eq 0 ] || die "$label: CRUD exited $rc ($(head -2 "$dir/err" | tr '\n' ' '))"
    printf '%s' "$out" | grep -qiE '^ERROR [0-9]' && die "$label: CRUD answered with an error: $(printf '%s' "$out" | grep -i 'error' | head -1)"
    printf '%s' "$out" | grep -q '10' || die "$label: inserted row 1 is missing from SELECT"
    printf '%s' "$out" | grep -q '20' || die "$label: inserted row 2 is missing from SELECT"
    rm -rf "$dir"
  }
  smoke ./minivec "bin"
  ok "SHOW TABLES answers, CREATE/INSERT/SELECT returns both rows"
  # the runtime image only has libstdc++6 + libgcc-s1: catch a new dependency here
  ldd ./minivec >"$TMP_DIR/ldd.txt" 2>&1 || die "ldd failed on ./minivec"
  if grep -q 'not found' "$TMP_DIR/ldd.txt"; then
    die "unresolved shared library: $(grep 'not found' "$TMP_DIR/ldd.txt" | head -3 | tr '\n' ' ')"
  fi
  unexpected=$(grep -oE '/[^ ]+\.so[^ ]*' "$TMP_DIR/ldd.txt" | sed 's#.*/##' \
    | grep -vE '^(libstdc\+\+|libgcc_s|libm|libc|libpthread|libdl|librt|libatomic|ld-linux)' || true)
  if [ -n "$unexpected" ]; then
    die "the runtime image would miss: $unexpected (add it to the runtime stage)"
  fi
  ok "shared deps: $(grep -oE '/[^ ]+\.so[^ ]*' "$TMP_DIR/ldd.txt" | sed 's#.*/##' | sort -u | tr '\n' ' ')"
fi

# --------------------------------------------------------------------------
# 4. build-context simulation: does the image build from the files the
#    .dockerignore whitelist lets through? (this is what catches a source
#    file that exists locally but never reaches the grader's build)
# --------------------------------------------------------------------------
if [ "$FAST" = 0 ] && [ "$NO_BUILD" = 0 ]; then
  step "build-context simulation (Makefile + src/ only, like the image)"
  CTX="$TMP_DIR/ctx"
  mkdir -p "$CTX"
  cp Makefile "$CTX/Makefile"
  cp -a src "$CTX/src"
  LOG="$TMP_DIR/ctx-build.log"
  if ! ( cd "$CTX" && make -j"$JOBS" ) >"$LOG" 2>&1; then
    tail -30 "$LOG" >&2
    die "the image build would fail: this tree does not compile from Makefile + src/ alone"
  fi
  [ -x "$CTX/minivec" ] || die "context build produced no binary"
  out=$(printf 'SHOW TABLES;\n' | timeout 30 "$CTX/minivec" --data "$TMP_DIR/ctxdata" 2>/dev/null) \
    || die "the context-built binary did not answer SHOW TABLES"
  [ -n "$out" ] || die "the context-built binary printed nothing for SHOW TABLES"
  ok "context build ($(stat -c %s "$CTX/minivec") bytes) answers SHOW TABLES"
elif [ "$FAST" = 1 ]; then
  step "build-context simulation (skipped by --fast)"
fi

# --------------------------------------------------------------------------
# 5. harness cases
# --------------------------------------------------------------------------
if [ "$NO_BUILD" = 0 ]; then
  step "harness: $HSQL case(s) in tests/cases"
  LOG="$TMP_DIR/harness.log"
  set +e
  python3 tools/harness.py run tests/cases >"$LOG" 2>"$TMP_DIR/harness.err"
  rc=$?
  set -e
  sed -e 's/^/   /' "$LOG" | tail -20
  if [ "$rc" -ne 0 ] || grep -q 'FAILED' "$LOG"; then
    grep -n 'FAILED\|^- \|^+ ' "$LOG" | head -30 >&2 || true
    die "harness reported a failing case (see above; full log: $LOG)"
  fi
  passed=$(grep -oE '^[0-9]+ case\(s\) passed' "$LOG" | grep -oE '^[0-9]+' || echo 0)
  [ "$passed" -eq "$HSQL" ] || die "harness passed $passed of $HSQL case(s)"
  ok "$passed/$HSQL case(s) passed"
fi

# --------------------------------------------------------------------------
# 6. --record drift check: re-record into a scratch copy and compare with the
#    committed .result files. Committed expectations must equal reality.
# --------------------------------------------------------------------------
if [ "$NO_BUILD" = 0 ]; then
  step "--record drift check (committed .result == recorded output)"
  REC="$TMP_DIR/record"
  mkdir -p "$REC"
  cp -a tests/cases/. "$REC/"
  LOG="$TMP_DIR/record.log"
  if ! python3 tools/harness.py record "$REC" >"$LOG" 2>&1; then
    tail -20 "$LOG" >&2
    die "harness record failed"
  fi
  set +e
  python3 - "$REC" tests/cases <<'PY'
import difflib
import sys
from pathlib import Path

sys.path.insert(0, "tools")
from harness import normalize_trailing_horizontal_whitespace as norm  # noqa: E402

recorded_dir, committed_dir = Path(sys.argv[1]), Path(sys.argv[2])
problems = []
compared = 0
for committed in sorted(committed_dir.glob("*.result")):
    fresh = recorded_dir / committed.name
    if not fresh.is_file():
        problems.append(f"{committed.name}: record produced no output for this case")
        continue
    compared += 1
    want = norm(committed.read_bytes())
    got = norm(fresh.read_bytes())
    if want != got:
        diff = difflib.unified_diff(
            want.decode("utf-8", "replace").split("\n"),
            got.decode("utf-8", "replace").split("\n"),
            fromfile=f"committed/{committed.name}",
            tofile=f"recorded/{committed.name}",
            lineterm="",
            n=2,
        )
        problems.append("\n".join(list(diff)[:40]))
missing = [p.name for p in sorted(committed_dir.glob("*.sql")) if not p.with_suffix(".result").is_file()]
for name in missing:
    problems.append(f"{name}: no committed .result (run `python3 tools/harness.py record tests/cases`)")
if problems:
    for item in problems:
        print(item, file=sys.stderr)
    raise SystemExit(1)
print(f"   {compared} result file(s) match a fresh recording")
PY
  rc=$?
  set -e
  [ "$rc" -eq 0 ] || die "committed .result files are stale -- re-record after confirming the new output is correct"
  ok "no record drift"
fi

# --------------------------------------------------------------------------
# 7. packaging lint (hadolint when present, plus the Python rule pass)
# --------------------------------------------------------------------------
step "packaging lint"
if command -v "${HADOLINT:-hadolint}" >/dev/null 2>&1; then
  LOG="$TMP_DIR/hadolint.log"
  if ! "${HADOLINT:-hadolint}" --failure-threshold warning Dockerfile >"$LOG" 2>&1; then
    cat "$LOG" >&2
    die "hadolint rejected Dockerfile"
  fi
  ok "hadolint: clean"
else
  info "hadolint not installed -- using the built-in rule pass"
fi
LOG="$TMP_DIR/lint.log"
ALSO=()
if [ "$PKG_ROOT" = "$ROOT" ] && [ -f "$SCAFFOLD/Dockerfile" ]; then
  ALSO=(--also "$SCAFFOLD")
elif [ "$PKG_ROOT" != "$ROOT" ]; then
  OTHER=${MVDB_DEV:-$ROOT/../minivec}
  [ -f "$OTHER/Dockerfile" ] && ALSO=(--also "$OTHER")
fi
python3 tools/lint_packaging.py --repo "$PKG_ROOT" "${ALSO[@]}" >"$LOG" 2>&1 \
  || { cat "$LOG" >&2; die "packaging lint failed"; }
if [ -f "$SCAFFOLD/.dockerignore" ] && [ -f "$PKG_ROOT/Dockerfile" ]; then
  [ -f tools/scaffold/dockerignore.root ] \
    || die "tools/scaffold/dockerignore.root is missing -- it is the source for the repo-root .dockerignore"
  cmp -s tools/scaffold/dockerignore.root "$SCAFFOLD/.dockerignore" \
    || die "$SCAFFOLD/.dockerignore differs from tools/scaffold/dockerignore.root -- run bash tools/ci.sh --sync"
fi
ok "packaging lint: $(basename "$PKG_ROOT")/Dockerfile + .dockerignore (context $PKG_ROOT)"

# --------------------------------------------------------------------------
# 8. script/python syntax
# --------------------------------------------------------------------------
step "shell + python syntax"
bash -n run.sh || die "run.sh has a syntax error"
[ "$(head -1 run.sh)" = "#!/usr/bin/env bash" ] || info "run.sh does not start with a bash shebang"
[ -x run.sh ] || die "run.sh is not executable"
bash -n tools/ci.sh || die "tools/ci.sh has a syntax error"
# ast.parse, not py_compile: py_compile drops __pycache__ into the tree we push
python3 - tools/*.py <<'PY' || die "a python tool has a syntax error"
import ast, sys
bad = 0
for path in sys.argv[1:]:
    try:
        ast.parse(open(path, encoding="utf-8").read(), path)
    except SyntaxError as exc:
        print(f"{path}:{exc.lineno}: {exc.msg}", file=sys.stderr)
        bad = 1
raise SystemExit(bad)
PY
ok "run.sh, tools/ci.sh, $(ls tools/*.py | wc -l) python tool(s)"

# --------------------------------------------------------------------------
# 8. pushable tree: the grader builds a CLONE, so anything the build needs that
#    .gitignore swallows (upstream seekdb ignores every `Makefile`) is fatal.
# --------------------------------------------------------------------------
step "pushable tree (build inputs are not git-ignored)"
if git -C "$ROOT" rev-parse --show-toplevel >/dev/null 2>&1; then
  TOP=$(git -C "$ROOT" rev-parse --show-toplevel)
  REL=$(python3 -c 'import os,sys; r=os.path.relpath(os.path.realpath(sys.argv[1]), sys.argv[2]); print("" if r in (".", "") else r.replace(os.sep,"/") + "/")' "$ROOT" "$TOP")
  ignored=""
  untracked=""
  checked=0
  add_path() {
    local p=$1
    [ -n "$p" ] || return 0
    [ -e "$TOP/$p" ] || return 0
    checked=$((checked + 1))
    if git -C "$TOP" check-ignore -q -- "$p"; then
      ignored="$ignored $p"
    elif [ -n "$(git -C "$TOP" status --porcelain -- "$p" 2>/dev/null | grep '^??' || true)" ]; then
      untracked="$untracked $p"
    fi
  }
  add_path "Dockerfile"
  add_path ".dockerignore"
  add_path "${REL}Makefile"
  add_path "${REL}run.sh"
  add_path "${REL}tools/harness.py"
  for f in "$ROOT"/src/*.cpp "$ROOT"/src/*.h "$ROOT"/tools/*.py "$ROOT"/tools/*.sh; do
    [ -e "$f" ] || continue
    add_path "${REL}${f#"$ROOT"/}"
  done
  if [ -n "$ignored" ]; then
    die "these build inputs are git-ignored and would never be pushed:$ignored (add a !exception to .gitignore)"
  fi
  if [ -n "$untracked" ]; then
    info "not committed yet (git add):$(printf '%s' "$untracked" | cut -c1-200)"
  fi
  ok "$checked build input(s) checked, none ignored by .gitignore (repo: $TOP)"
else
  info "$ROOT is not inside a git worktree -- cannot check .gitignore rules here"
fi

# --------------------------------------------------------------------------
# 9. evidence pipeline (agent/conversation.json)
# --------------------------------------------------------------------------
if [ -f tools/build_conversation.py ]; then
  step "evidence pipeline (agent/conversation.json)"
  expect_pass() { # $1 = label, $2 = dir
    LOG="$TMP_DIR/evidence-$1.log"
    if ! python3 tools/build_conversation.py --check "$2" >"$LOG" 2>&1; then
      cat "$LOG" >&2
      die "build_conversation --check failed on $2 (it should pass)"
    fi
    printf '   ok   %s\n        -> %s\n' "$3" "$(head -1 "$LOG")"
  }
  expect_fail() { # $1 = label, $2 = dir, $3 = expected error code, $4 = description
    LOG="$TMP_DIR/evidence-$1.log"
    if python3 tools/build_conversation.py --check "$2" >"$LOG" 2>&1; then
      die "build_conversation --check accepted $2 -- it must be rejected"
    fi
    grep -q "$3" "$LOG" || { cat "$LOG" >&2; die "$2 was rejected, but not with $3"; }
    printf '   ok   %s\n        -> %s\n' "$4" "$(head -1 "$LOG")"
  }
  [ -d tools/samples/good ] || die "tools/samples/good is missing"
  [ -d tools/samples/broken ] || die "tools/samples/broken is missing"
  [ -d tools/samples/broken-no-result ] || die "tools/samples/broken-no-result is missing"
  expect_pass good tools/samples/good "sample good/ passes --check"
  expect_fail broken tools/samples/broken E_QUOTE_NOT_FOUND \
    "sample broken/ is rejected: quote not found at its pointer"
  expect_fail no-result tools/samples/broken-no-result E_MISSING_RESULT_EVIDENCE \
    "sample broken-no-result/ is rejected: cited as evidence with no measured result"

  # third refusal rule: a valid document that would blow the 128 KiB budget
  OVER="$TMP_DIR/oversized-raw"
  mkdir -p "$OVER"
  python3 - "$OVER/01_oversized.md" <<'PY'
import sys
body = "示例长文本" * 9000          # ~108 KiB on its own, under the limit
text = (
    "# generated fixture: valid records, oversized document\n\n"
    "```record\nid: big1\nkind: conversation\ndimension: Prompt 设计与迭代\n"
    "prompt: |\n  " + body + "\n"
    "decision: |\n  先复制再校验，避免手抄失真。\n"
    "evidence:\n  - target: big1#decision\n    quote: 先复制再校验\n"
    "result_evidence:\n  summary: " + body + "\n```\n"
)
open(sys.argv[1], "w", encoding="utf-8").write(text)
PY
  expect_fail oversized "$OVER" E_TOO_LARGE \
    "an oversized document (${OVER##*/}) is rejected instead of written"

  if [ -d "$PKG_ROOT/agent/raw" ]; then EVID="$PKG_ROOT/agent"; else EVID="$ROOT/agent"; fi
  if [ -d "$EVID/raw" ] && [ -n "$(find "$EVID/raw" -type f \( -name '*.md' -o -name '*.jsonl' \) 2>/dev/null | head -1)" ]; then
    expect_pass raw "$EVID/raw" "agent/raw/ passes --check"
    if [ -f "$EVID/conversation.json" ]; then
      LOG="$TMP_DIR/evidence-verify.log"
      if ! python3 tools/build_conversation.py --verify "$EVID/conversation.json" >"$LOG" 2>&1; then
        cat "$LOG" >&2
        die "agent/conversation.json does not satisfy the evidence rules -- regenerate it"
      fi
      printf '   ok   %s\n        -> %s\n' "agent/conversation.json verifies ($(stat -c %s "$EVID/conversation.json") bytes)" "$(head -1 "$LOG")"
      # committed file must also be what the generator would produce right now
      if ! python3 - "$EVID" <<'PY'
import sys
from pathlib import Path
sys.path.insert(0, "tools")
import build_conversation as bc
evid = Path(sys.argv[1])
data, _ = bc.build_bytes(evid / "raw", bc.MAX_BYTES_DEFAULT)
committed = (evid / "conversation.json").read_bytes()
if data != committed:
    print("conversation.json is stale: regenerate with "
          "`python3 tools/build_conversation.py`", file=sys.stderr)
    raise SystemExit(1)
PY
      then
        die "agent/conversation.json does not match a fresh build from agent/raw/"
      fi
      ok "agent/conversation.json is up to date with agent/raw/"
    else
      info "agent/conversation.json not generated yet (run: python3 tools/build_conversation.py)"
    fi
  else
    info "agent/raw/ is empty -- evidence check limited to the samples"
  fi
fi

printf '\n'
