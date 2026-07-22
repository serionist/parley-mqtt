#!/usr/bin/env bash
# Parley layer-3 interop matrix driver (docs/testing.md). Builds the five pinned images, brings up the
# mosquitto broker (healthcheck-gated), then runs ALL 16 ordered commander x handler pairs serially on
# isolated topic prefixes, plus the 4 per-commander $noAck self-checks. Each pair gates on the handler
# printing READY (subscriptions landed) before the commander starts, and on a per-pair wall-clock
# timeout. Semantics: GREEN = passed first try; AMBER = passed only on the single retry (blocks the
# gate; carries the first-attempt diff); RED = failed twice (carries both diffs + a cross-attempt
# divergence flag). Exit 0 iff every cell is GREEN; nonzero otherwise (RED, or any AMBER).
#
# Usage: ./run-matrix.sh [run-label]   (default: r1). Run twice with distinct labels for the
# determinism gate — isolated prefixes mean the two runs never share broker or peer state.

set -uo pipefail
cd "$(dirname "$0")"

LABEL="${1:-r1}"
LANGS=(csharp js python cpp)
BROKER="mosquitto:1883"
SCEN="/app/scenarios.json"
READY_TIMEOUT=45          # seconds to wait for a handler's READY line
WALL=60                   # per-pair commander wall-clock cap (seconds) -> RED on breach
COMPOSE="docker compose"

mkdir -p reports
rm -f reports/*.json 2>/dev/null || true

declare -A STATUS         # STATUS[C_H] = GREEN|AMBER|RED
declare -A DETAIL         # DETAIL[C_H] = failure narrative for non-GREEN
declare -A SC_STATUS      # SC_STATUS[C] = GREEN|AMBER|RED (self-check)
declare -A SC_DETAIL

log() { echo "[$(date +%H:%M:%S)] $*" >&2; }

cleanup() {
  # Remove any leftover detached handler containers from this label.
  docker ps -aq --filter "name=parley_h_${LABEL}_" 2>/dev/null | xargs -r docker rm -f >/dev/null 2>&1 || true
}
trap cleanup EXIT

wait_ready() {
  local name="$1" start=$SECONDS running
  while (( SECONDS - start < READY_TIMEOUT )); do
    if docker logs "$name" 2>&1 | grep -q PARLEY-PEER-READY; then return 0; fi
    running=$(docker inspect -f '{{.State.Running}}' "$name" 2>/dev/null || echo false)
    if [ "$running" != "true" ]; then
      log "handler $name exited before READY; last log lines:"
      docker logs "$name" 2>&1 | tail -15 >&2
      return 1
    fi
    sleep 0.3
  done
  log "timed out waiting for READY from $name"
  return 1
}

extract_diff() {
  local f="$1"
  [ -f "$f" ] || { echo "  (no report file — commander crashed or timed out)"; return; }
  python3 - "$f" <<'PY'
import json, sys
try:
    d = json.load(open(sys.argv[1]))
except Exception as e:
    print(f"  (unreadable report: {e})"); sys.exit()
fails = [r for r in d.get("results", []) if not r.get("pass")]
if not fails:
    print("  (nonzero exit but every recorded scenario passed)")
for r in fails:
    print(f"  - {r['scenario']}: {r['detail']}")
PY
}

fail_set() {
  local f="$1"
  [ -f "$f" ] || { echo "NO_REPORT"; return; }
  python3 - "$f" <<'PY'
import json, sys
try:
    d = json.load(open(sys.argv[1]))
except Exception:
    print("UNREADABLE"); sys.exit()
print(",".join(sorted(r["scenario"] for r in d.get("results", []) if not r.get("pass"))) or "NONE")
PY
}

# attempt <commander> <handler|-> <prefix> <report-host-path> <tag> <suite>
# Starts the handler (unless handler is "-"), gates on READY, runs the commander with a wall-clock cap.
# Returns the commander exit code (0 = all scenarios passed).
attempt() {
  local C="$1" H="$2" prefix="$3" report="$4" tag="$5" suite="$6"
  local hname="parley_h_${LABEL}_${tag}"
  local rc=0
  local extra=()
  [ "$suite" = "selfchecks" ] && extra=(--suite selfchecks)

  if [ "$H" != "-" ]; then
    docker rm -f "$hname" >/dev/null 2>&1 || true
    $COMPOSE run -d -T --name "$hname" "$H" \
      --role handler --broker "$BROKER" --prefix "$prefix" --scenarios "$SCEN" >/dev/null
    if ! wait_ready "$hname"; then
      docker rm -f "$hname" >/dev/null 2>&1 || true
      return 70
    fi
  fi

  timeout "$WALL" $COMPOSE run --rm -T "$C" \
    --role commander --broker "$BROKER" --prefix "$prefix" --scenarios "$SCEN" \
    --report "/reports/$(basename "$report")" --handler-lang "$H" "${extra[@]}" >/dev/null 2>&1 || rc=$?

  [ "$H" != "-" ] && docker rm -f "$hname" >/dev/null 2>&1 || true
  return $rc
}

# run_pair <commander> <handler> : attempt once, retry once, classify GREEN/AMBER/RED.
run_pair() {
  local C="$1" H="$2" key="${C}_${H}"
  local base="t/${LABEL}/${C}-${H}"
  local r1f="reports/${C}-${H}.json" r2f="reports/${C}-${H}-r2.json"

  log "pair ${C} -> ${H} : attempt 1"
  if attempt "$C" "$H" "$base" "$r1f" "${C}_${H}_a1" matrix; then
    STATUS[$key]=GREEN
    return
  fi
  local d1 f1; d1="$(extract_diff "$r1f")"; f1="$(fail_set "$r1f")"

  log "pair ${C} -> ${H} : attempt 1 FAILED -> retry"
  if attempt "$C" "$H" "${base}-r2" "$r2f" "${C}_${H}_a2" matrix; then
    STATUS[$key]=AMBER
    DETAIL[$key]="AMBER (passed only on retry). First-attempt failure:"$'\n'"$d1"
    return
  fi
  local d2 f2; d2="$(extract_diff "$r2f")"; f2="$(fail_set "$r2f")"
  STATUS[$key]=RED
  local div=""
  [ "$f1" != "$f2" ] && div=$'\n'"  ** CROSS-ATTEMPT DIVERGENCE (different failing scenarios: [$f1] vs [$f2]) — nondeterminism signal **"
  DETAIL[$key]="RED (failed twice).${div}"$'\n'"attempt 1:"$'\n'"$d1"$'\n'"attempt 2:"$'\n'"$d2"
}

# run_selfcheck <commander> : $noAck probe against a handler-less prefix, retry-once classified.
run_selfcheck() {
  local C="$1"
  local base="t/${LABEL}/selfcheck-${C}"
  local r1f="reports/selfcheck-${C}.json" r2f="reports/selfcheck-${C}-r2.json"

  log "self-check ${C} (\$noAck) : attempt 1"
  if attempt "$C" "-" "$base" "$r1f" "sc_${C}_a1" selfchecks; then
    SC_STATUS[$C]=GREEN
    return
  fi
  local d1; d1="$(extract_diff "$r1f")"
  log "self-check ${C} : attempt 1 FAILED -> retry"
  if attempt "$C" "-" "${base}-r2" "$r2f" "sc_${C}_a2" selfchecks; then
    SC_STATUS[$C]=AMBER
    SC_DETAIL[$C]="AMBER (passed only on retry). First attempt:"$'\n'"$d1"
    return
  fi
  local d2; d2="$(extract_diff "$r2f")"
  SC_STATUS[$C]=RED
  SC_DETAIL[$C]="RED (failed twice)."$'\n'"attempt 1:"$'\n'"$d1"$'\n'"attempt 2:"$'\n'"$d2"
}

# ---- build + broker ----
log "building the five pinned images (mosquitto pulled, four peers built)..."
if ! $COMPOSE build; then log "image build FAILED"; exit 3; fi
log "starting mosquitto (waiting for healthy)..."
if ! $COMPOSE up -d --wait mosquitto; then log "mosquitto did not become healthy"; exit 3; fi

# ---- the 16 ordered pairs ----
for C in "${LANGS[@]}"; do
  for H in "${LANGS[@]}"; do
    run_pair "$C" "$H"
  done
done

# ---- the 4 commander self-checks ----
for C in "${LANGS[@]}"; do
  run_selfcheck "$C"
done

# ---- grid ----
reds=0; ambers=0
echo
echo "======================================================================"
echo " Parley interop matrix — run '${LABEL}'   (rows = commander, cols = handler)"
echo "======================================================================"
printf "%-11s" "cmdr\\hdlr"
for H in "${LANGS[@]}"; do printf "%-9s" "$H"; done
echo
for C in "${LANGS[@]}"; do
  printf "%-11s" "$C"
  for H in "${LANGS[@]}"; do
    s="${STATUS[${C}_${H}]:-?}"
    printf "%-9s" "$s"
    [ "$s" = "RED" ] && reds=$((reds+1))
    [ "$s" = "AMBER" ] && ambers=$((ambers+1))
  done
  echo
done
echo
echo "self-checks (\$noAck, no handler):"
for C in "${LANGS[@]}"; do
  s="${SC_STATUS[$C]:-?}"
  printf "  %-8s %s\n" "$C" "$s"
  [ "$s" = "RED" ] && reds=$((reds+1))
  [ "$s" = "AMBER" ] && ambers=$((ambers+1))
done
echo "----------------------------------------------------------------------"
echo "totals: reds=${reds}  ambers=${ambers}"
echo "======================================================================"

# ---- details for any non-GREEN cell ----
if (( reds > 0 || ambers > 0 )); then
  echo
  echo "Non-GREEN detail:"
  for C in "${LANGS[@]}"; do
    for H in "${LANGS[@]}"; do
      key="${C}_${H}"
      if [ "${STATUS[$key]:-}" != "GREEN" ]; then
        echo "--- ${C} -> ${H} : ${STATUS[$key]:-?}"
        echo "${DETAIL[$key]:-}"
      fi
    done
  done
  for C in "${LANGS[@]}"; do
    if [ "${SC_STATUS[$C]:-}" != "GREEN" ]; then
      echo "--- self-check ${C} : ${SC_STATUS[$C]:-?}"
      echo "${SC_DETAIL[$C]:-}"
    fi
  done
fi

if (( reds > 0 )); then
  echo "RESULT: RED — matrix has hard failures." >&2
  exit 2
fi
if (( ambers > 0 )); then
  echo "RESULT: AMBER — a cell passed only on retry; the gate requires all-GREEN. Investigate." >&2
  exit 1
fi
echo "RESULT: ALL GREEN — 16 pairs + 4 self-checks passed on the first attempt." >&2
exit 0
