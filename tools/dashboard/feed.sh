#!/usr/bin/env bash
# feed.sh — run the sim (or just mirror a CSV) and live-sync its metrics to the
# nginx host, so the balance dashboard animates in near-real-time.
#
# The sim writes the metrics CSV locally; a background rsync loop mirrors the
# (growing) file to the dashboard's web root on the nginx host every --interval
# seconds. rsync sends only the appended delta, so it's cheap and matches the
# dashboard's ~1.5s poll. Works for headless or the GUI.
#
# Usage:
#   tools/dashboard/feed.sh [options] [-- sim-args...]
#
# Options:
#   --remote USER@HOST:PATH  destination CSV (default: the home-ubuntu instance)
#   --local PATH             local CSV the sim writes (default: a temp file)
#   --bin PATH               sim binary to run (default: ./csim/build/csim_headless)
#   --interval SECS          mirror cadence (default: 1)
#   --mirror-only            don't run a sim; just mirror an existing --local file
#                            (e.g. a GUI you already launched with CSIM_METRICS=that)
#   -h, --help               this help
#
# Examples:
#   tools/dashboard/feed.sh -- --days 40                 # headless 40-day run, live
#   tools/dashboard/feed.sh --bin ./csim/build/csim --   # GUI run, live
#   CSIM_METRICS=/tmp/m.csv ./csim/build/csim &          # GUI writing /tmp/m.csv
#   tools/dashboard/feed.sh --mirror-only --local /tmp/m.csv
set -euo pipefail

REMOTE="xor@10.0.0.134:/opt/docker/deploy/emergent-city/web/metrics.csv"
LOCAL=""
BIN=""
INTERVAL=1
MIRROR_ONLY=0
SIM_ARGS=()

# repo root = two levels up from this script (tools/dashboard/ -> repo)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"

while [ $# -gt 0 ]; do
  case "$1" in
    --remote)      REMOTE="$2"; shift 2;;
    --local)       LOCAL="$2"; shift 2;;
    --bin)         BIN="$2"; shift 2;;
    --interval)    INTERVAL="$2"; shift 2;;
    --mirror-only) MIRROR_ONLY=1; shift;;
    -h|--help)     sed -n '2,32p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0;;
    --)            shift; SIM_ARGS=("$@"); break;;
    *)             echo "unknown option: $1 (sim args go after --)" >&2; exit 2;;
  esac
done

command -v rsync >/dev/null || { echo "feed.sh: rsync is required" >&2; exit 1; }

: "${LOCAL:=$(mktemp -t ec-metrics.XXXXXX.csv)}"
: "${BIN:=$REPO/csim/build/csim_headless}"

echo "feed.sh: local  = $LOCAL"
echo "feed.sh: remote = $REMOTE"
echo "feed.sh: mirror every ${INTERVAL}s  (Ctrl-C to stop)"

# --- background mirror loop ------------------------------------------------
MIRROR_PID=""
mirror_loop() {
  while :; do
    [ -f "$LOCAL" ] && rsync -q "$LOCAL" "$REMOTE" 2>/dev/null || true
    sleep "$INTERVAL"
  done
}
cleanup() {
  [ -n "$MIRROR_PID" ] && kill "$MIRROR_PID" 2>/dev/null || true
  # one last sync so the final rows land
  [ -f "$LOCAL" ] && rsync -q "$LOCAL" "$REMOTE" 2>/dev/null || true
  echo; echo "feed.sh: final sync done."
}
trap cleanup EXIT INT TERM

mirror_loop & MIRROR_PID=$!

# --- run the sim (unless mirror-only) --------------------------------------
if [ "$MIRROR_ONLY" -eq 1 ]; then
  echo "feed.sh: mirror-only — waiting on $LOCAL (start/leave your sim writing it)…"
  # idle until interrupted; the mirror loop does the work
  while :; do sleep "$INTERVAL"; done
else
  [ -x "$BIN" ] || { echo "feed.sh: sim binary not found/executable: $BIN" >&2; exit 1; }
  echo "feed.sh: running $BIN --metrics $LOCAL ${SIM_ARGS[*]:-}"
  if [ "${#SIM_ARGS[@]}" -gt 0 ]; then
    "$BIN" --metrics "$LOCAL" "${SIM_ARGS[@]}"
  else
    "$BIN" --metrics "$LOCAL"
  fi
fi
