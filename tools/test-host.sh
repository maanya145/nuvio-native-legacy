#!/bin/bash
# Focused host test runner for this fork (see AGENTS.md, "Tests").
#
#   bash tools/test-host.sh                 # group "quick"
#   bash tools/test-host.sh pgs buffer      # one or more groups
#   bash tools/test-host.sh tests/pgs.sh    # explicit scripts
#   bash tools/test-host.sh --list          # show groups
#
# Each script runs with its own timeout (TEST_TIMEOUT seconds, default 300) and
# an empty stdin, so a test that reads stdin cannot swallow the next name.
# macOS has no `timeout`: this falls back to `gtimeout` (coreutils), then perl.
# Exit status is non-zero if any script failed. A script that prints SKIP is
# reported as not verified, not as passed.
set -u
cd "$(dirname "$0")/.." || exit 1

group() {
  case "$1" in
    pgs)    echo tests/pgs.sh tests/mkvass_pgs.sh tests/imgsub_gl.sh ;;
    buffer) echo tests/moviebuffer.sh tests/video_http_buffer.sh ;;
    dts)    echo tests/dts_tv.sh ;;
    mkvass) ls tests/mkvass*.sh tests/mkv_legendas.sh ;;
    quick)  echo "$(group pgs) $(group buffer) $(group dts)" ;;
    *) return 1 ;;
  esac
}

if [ "${1:-}" = --list ]; then
  for g in quick pgs buffer dts mkvass; do echo "$g: $(group $g | tr '\n' ' ')"; done
  exit 0
fi

scripts=()
[ $# -eq 0 ] && set -- quick
for arg in "$@"; do
  if [ -f "$arg" ]; then scripts+=("$arg")
  elif list=$(group "$arg"); then for s in $list; do scripts+=("$s"); done
  else echo "unknown group or file: $arg (try --list)" >&2; exit 2
  fi
done

limit=${TEST_TIMEOUT:-300}
run_limited() {
  if command -v timeout >/dev/null 2>&1; then timeout -k 5 "$limit" "$@"
  elif command -v gtimeout >/dev/null 2>&1; then gtimeout -k 5 "$limit" "$@"
  else perl -e 'alarm shift; exec @ARGV or die "exec: $!"' "$limit" "$@"
  fi
}

logs=$(mktemp -d "${TMPDIR:-/tmp}/nuvio-host-tests.XXXXXX") || exit 1
pass=0 fail=0 skip=0 seen=" "
for s in "${scripts[@]}"; do
  case "$seen" in *" $s "*) continue ;; esac
  seen="$seen$s "
  name=$(basename "$s" .sh)
  start=$(date +%s)
  run_limited bash "$s" >"$logs/$name.log" 2>&1 </dev/null
  rc=$?
  secs=$(( $(date +%s) - start ))
  if [ $rc -eq 0 ] && grep -Eq '(^|[^A-Za-z])SKIP([^A-Za-z]|$)' "$logs/$name.log"; then
    echo "SKIP  $name (${secs}s, not verified: $(grep -Em1 SKIP "$logs/$name.log" | cut -c1-80))"
    skip=$((skip + 1))
  elif [ $rc -eq 0 ]; then
    echo "ok    $name (${secs}s)"; pass=$((pass + 1))
  else
    [ $rc -ge 124 ] && [ $rc -le 142 ] && [ $secs -ge "$limit" ] && note=" timeout" || note=""
    echo "FAIL  $name (rc=$rc${note}, ${secs}s)"; tail -12 "$logs/$name.log" | sed 's/^/      /'
    fail=$((fail + 1))
  fi
done
echo "host tests: $pass passed, $fail failed, $skip skipped; logs: $logs"
[ $fail -eq 0 ]
