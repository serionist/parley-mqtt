#!/usr/bin/env bash
# Parley layer-3 interop matrix driver (docs/testing.md). Builds the five pinned images, brings up the
# mosquitto broker (healthcheck-gated), then runs three phases:
#
#   1. BASE grid   — all 16 ordered commander x handler pairs (9 scenarios each) at QoS 0, plus the 4
#                    per-commander $noAck self-checks.
#   2. DEEP phase  — driver-orchestrated real-world disruptions on a FIXED deterministic pair mapping
#                    (csharp->js, js->python, python->cpp, cpp->csharp), identical across runs: each pair
#                    runs stale-detect (SIGKILL the handler mid-execution -> $executionStale) and
#                    broker-restart (bounce mosquitto mid-execution -> adapter reconnect, {ok:true}).
#                    The commander prints a flushed PARLEY-BARRIER line at the rendezvous; THIS driver
#                    reacts by performing the container action, then waits for the commander's outcome.
#   3. QOS-1 pass  — the full 16-pair grid re-run with --qos 1 on both peers. This is ADAPTER-COMPATIBILITY
#                    coverage only: with clean sessions + broker persistence off there is no redelivery
#                    path, so it proves QoS-1 mechanics (PUBACK flow, no stall) — NOT redelivery/dedup
#                    (which stays a layer-2 concern; the over-the-wire dedup gap REMAINS recorded).
#
# Semantics (every phase): GREEN = passed first try; AMBER = passed only on the single retry (blocks the
# gate; carries the first-attempt diff); RED = failed twice (carries both diffs + a cross-attempt
# divergence flag). Exit 0 iff every cell across all phases is GREEN; nonzero otherwise (RED, or any AMBER).
#
# Usage: ./run-matrix.sh [run-label]   (default: r1). Run twice with distinct labels for the
# determinism gate — isolated prefixes mean the two runs never share broker or peer state; the deep pair
# mapping is fixed so the two runs are directly comparable.

set -uo pipefail
cd "$(dirname "$0")"

LABEL="${1:-r1}"
LANGS=(csharp js python cpp)
BROKER="mosquitto:1883"
SCEN="/app/scenarios.json"
DEEP_SCEN="/app/deep-scenarios.json"
READY_TIMEOUT=45          # seconds to wait for a handler's READY line
BARRIER_TIMEOUT=45        # seconds to wait for a deep commander's PARLEY-BARRIER line
WALL=60                   # per-pair commander wall-clock cap (seconds) -> RED on breach (base/qos1)
DEEP_WALL=90              # per-deep-run commander wall-clock cap (seconds); covers stale window + reconnect
COMPOSE="docker compose"

# Fixed, deterministic deep-phase pair mapping (identical across runs — the x2 determinism gate is
# meaningless otherwise). Each entry: "<commander> <handler>".
DEEP_PAIRS=("csharp js" "js python" "python cpp" "cpp csharp")
DEEP_SCENARIOS=(stale-detect broker-restart)

mkdir -p reports
rm -f reports/*.json 2>/dev/null || true

declare -A STATUS         # STATUS[C_H] = GREEN|AMBER|RED  (base grid)
declare -A DETAIL
declare -A SC_STATUS      # SC_STATUS[C] = GREEN|AMBER|RED (self-check)
declare -A SC_DETAIL
declare -A QOS_STATUS     # QOS_STATUS[C_H] = GREEN|AMBER|RED (qos-1 grid)
declare -A QOS_DETAIL
declare -A DEEP_STATUS    # DEEP_STATUS[scenario_C_H] = GREEN|AMBER|RED (deep phase)
declare -A DEEP_DETAIL

log() { echo "[$(date +%H:%M:%S)] $*" >&2; }

cleanup() {
  # Remove any leftover detached handler/commander containers from this label.
  docker ps -aq --filter "name=parley_h_${LABEL}_" 2>/dev/null | xargs -r docker rm -f >/dev/null 2>&1 || true
  docker ps -aq --filter "name=parley_c_${LABEL}_" 2>/dev/null | xargs -r docker rm -f >/dev/null 2>&1 || true
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

# wait_barrier <commander-container> <scenario-name> : block until the commander prints its flushed
# PARLEY-BARRIER <scenario> line (the driver rendezvous), or fail if it exits early / times out.
wait_barrier() {
  local name="$1" scenario="$2" start=$SECONDS running
  while (( SECONDS - start < BARRIER_TIMEOUT )); do
    if docker logs "$name" 2>&1 | grep -qF "PARLEY-BARRIER ${scenario}"; then return 0; fi
    running=$(docker inspect -f '{{.State.Running}}' "$name" 2>/dev/null || echo false)
    if [ "$running" != "true" ]; then
      # Exited before the barrier — final check, then report.
      docker logs "$name" 2>&1 | grep -qF "PARLEY-BARRIER ${scenario}" && return 0
      log "commander $name exited before barrier '${scenario}'; last log lines:"
      docker logs "$name" 2>&1 | tail -15 >&2
      return 1
    fi
    sleep 0.3
  done
  log "timed out waiting for barrier '${scenario}' from $name"
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

# attempt <commander> <handler|-> <prefix> <report-host-path> <tag> <suite> <qos> <scen>
# Starts the handler (unless handler is "-"), gates on READY, runs the commander with a wall-clock cap.
# Returns the commander exit code (0 = all scenarios passed).
attempt() {
  local C="$1" H="$2" prefix="$3" report="$4" tag="$5" suite="$6" qos="$7" scen="$8"
  local hname="parley_h_${LABEL}_${tag}"
  local rc=0
  local extra=()
  [ "$suite" = "selfchecks" ] && extra=(--suite selfchecks)

  if [ "$H" != "-" ]; then
    docker rm -f "$hname" >/dev/null 2>&1 || true
    $COMPOSE run -d -T --name "$hname" "$H" \
      --role handler --broker "$BROKER" --prefix "$prefix" --scenarios "$scen" --qos "$qos" >/dev/null
    if ! wait_ready "$hname"; then
      docker rm -f "$hname" >/dev/null 2>&1 || true
      return 70
    fi
  fi

  timeout "$WALL" $COMPOSE run --rm -T "$C" \
    --role commander --broker "$BROKER" --prefix "$prefix" --scenarios "$scen" --qos "$qos" \
    --report "/reports/$(basename "$report")" --handler-lang "$H" "${extra[@]}" >/dev/null 2>&1 || rc=$?

  [ "$H" != "-" ] && docker rm -f "$hname" >/dev/null 2>&1 || true
  return $rc
}

# run_pair <commander> <handler> <phase> <status-array> <detail-array>
# phase = base | qos1  (selects topic namespace, report prefix, and QoS). Retry-once classified.
run_pair() {
  local C="$1" H="$2" phase="$3"
  local -n _ST="$4"
  local -n _DT="$5"
  local qos topicns
  case "$phase" in
    qos1) qos=1; topicns="q" ;;
    *)    qos=0; topicns="t" ;;
  esac
  local key="${C}_${H}"
  local base="${topicns}/${LABEL}/${C}-${H}"
  local r1f="reports/${phase}-${C}-${H}.json" r2f="reports/${phase}-${C}-${H}-r2.json"

  log "[$phase] pair ${C} -> ${H} : attempt 1"
  if attempt "$C" "$H" "$base" "$r1f" "${phase}_${C}_${H}_a1" matrix "$qos" "$SCEN"; then
    _ST[$key]=GREEN
    return
  fi
  local d1 f1; d1="$(extract_diff "$r1f")"; f1="$(fail_set "$r1f")"

  log "[$phase] pair ${C} -> ${H} : attempt 1 FAILED -> retry"
  if attempt "$C" "$H" "${base}-r2" "$r2f" "${phase}_${C}_${H}_a2" matrix "$qos" "$SCEN"; then
    _ST[$key]=AMBER
    _DT[$key]="AMBER (passed only on retry). First-attempt failure:"$'\n'"$d1"
    return
  fi
  local d2 f2; d2="$(extract_diff "$r2f")"; f2="$(fail_set "$r2f")"
  _ST[$key]=RED
  local div=""
  [ "$f1" != "$f2" ] && div=$'\n'"  ** CROSS-ATTEMPT DIVERGENCE (different failing scenarios: [$f1] vs [$f2]) — nondeterminism signal **"
  _DT[$key]="RED (failed twice).${div}"$'\n'"attempt 1:"$'\n'"$d1"$'\n'"attempt 2:"$'\n'"$d2"
}

# run_selfcheck <commander> : $noAck probe against a handler-less prefix, retry-once classified.
run_selfcheck() {
  local C="$1"
  local base="t/${LABEL}/selfcheck-${C}"
  local r1f="reports/selfcheck-${C}.json" r2f="reports/selfcheck-${C}-r2.json"

  log "self-check ${C} (\$noAck) : attempt 1"
  if attempt "$C" "-" "$base" "$r1f" "sc_${C}_a1" selfchecks 0 "$SCEN"; then
    SC_STATUS[$C]=GREEN
    return
  fi
  local d1; d1="$(extract_diff "$r1f")"
  log "self-check ${C} : attempt 1 FAILED -> retry"
  if attempt "$C" "-" "${base}-r2" "$r2f" "sc_${C}_a2" selfchecks 0 "$SCEN"; then
    SC_STATUS[$C]=AMBER
    SC_DETAIL[$C]="AMBER (passed only on retry). First attempt:"$'\n'"$d1"
    return
  fi
  local d2; d2="$(extract_diff "$r2f")"
  SC_STATUS[$C]=RED
  SC_DETAIL[$C]="RED (failed twice)."$'\n'"attempt 1:"$'\n'"$d1"$'\n'"attempt 2:"$'\n'"$d2"
}

# deep_attempt <commander> <handler> <scenario> <prefix> <report-host-path> <tag>
# One driver-orchestrated deep run: start handler (deep config), gate READY, start the commander DETACHED
# in single-scenario mode, wait for its PARLEY-BARRIER line, perform the container action, then wait for
# the commander to exit (DEEP_WALL cap). Returns the commander exit code (0 = the scenario passed).
deep_attempt() {
  local C="$1" H="$2" scenario="$3" prefix="$4" report="$5" tag="$6"
  local hname="parley_h_${LABEL}_${tag}" cname="parley_c_${LABEL}_${tag}"
  docker rm -f "$hname" "$cname" >/dev/null 2>&1 || true

  $COMPOSE run -d -T --name "$hname" "$H" \
    --role handler --broker "$BROKER" --prefix "$prefix" --scenarios "$DEEP_SCEN" >/dev/null
  if ! wait_ready "$hname"; then
    docker rm -f "$hname" "$cname" >/dev/null 2>&1 || true
    return 70
  fi

  $COMPOSE run -d -T --name "$cname" "$C" \
    --role commander --broker "$BROKER" --prefix "$prefix" --scenarios "$DEEP_SCEN" \
    --scenario "$scenario" --report "/reports/$(basename "$report")" --handler-lang "$H" >/dev/null

  if ! wait_barrier "$cname" "$scenario"; then
    docker rm -f "$hname" "$cname" >/dev/null 2>&1 || true
    return 71
  fi

  # The driver rendezvous fired — perform the disruption.
  case "$scenario" in
    stale-detect)
      log "[deep:$scenario] SIGKILL handler ${hname} (compose kill / rm -f — never stop)"
      docker rm -f "$hname" >/dev/null 2>&1 || true
      ;;
    broker-restart)
      log "[deep:$scenario] restarting mosquitto (docker compose restart -t 1)"
      $COMPOSE restart -t 1 mosquitto >/dev/null 2>&1 || true
      ;;
  esac

  # Wait for the commander's real terminal outcome within the deep wall-clock cap.
  local waited twrc rc
  waited=$(timeout "$DEEP_WALL" docker wait "$cname" 2>/dev/null); twrc=$?
  if [ $twrc -ne 0 ]; then
    log "[deep:$scenario] ${C}->${H} : commander did not exit within ${DEEP_WALL}s"
    rc=124
  else
    rc=${waited:-1}
  fi

  docker rm -f "$hname" "$cname" >/dev/null 2>&1 || true
  return $rc
}

# run_deep_pair <commander> <handler> <scenario> : retry-once classified deep run.
run_deep_pair() {
  local C="$1" H="$2" scenario="$3" key="${scenario}_${C}_${H}"
  local base="d/${LABEL}/${scenario}/${C}-${H}"
  local r1f="reports/deep-${scenario}-${C}-${H}.json" r2f="reports/deep-${scenario}-${C}-${H}-r2.json"

  log "[deep:$scenario] pair ${C} -> ${H} : attempt 1"
  if deep_attempt "$C" "$H" "$scenario" "$base" "$r1f" "deep_${scenario}_${C}_${H}_a1"; then
    DEEP_STATUS[$key]=GREEN
    return
  fi
  local d1 f1; d1="$(extract_diff "$r1f")"; f1="$(fail_set "$r1f")"

  log "[deep:$scenario] pair ${C} -> ${H} : attempt 1 FAILED -> retry"
  if deep_attempt "$C" "$H" "$scenario" "${base}-r2" "$r2f" "deep_${scenario}_${C}_${H}_a2"; then
    DEEP_STATUS[$key]=AMBER
    DEEP_DETAIL[$key]="AMBER (passed only on retry). First-attempt failure:"$'\n'"$d1"
    return
  fi
  local d2 f2; d2="$(extract_diff "$r2f")"; f2="$(fail_set "$r2f")"
  DEEP_STATUS[$key]=RED
  local div=""
  [ "$f1" != "$f2" ] && div=$'\n'"  ** CROSS-ATTEMPT DIVERGENCE (different failing scenarios: [$f1] vs [$f2]) — nondeterminism signal **"
  DEEP_DETAIL[$key]="RED (failed twice).${div}"$'\n'"attempt 1:"$'\n'"$d1"$'\n'"attempt 2:"$'\n'"$d2"
}

# ---- build + broker ----
T_START=$SECONDS
log "building the five pinned images (mosquitto pulled, four peers built)..."
if ! $COMPOSE build; then log "image build FAILED"; exit 3; fi
log "starting mosquitto (waiting for healthy)..."
if ! $COMPOSE up -d --wait mosquitto; then log "mosquitto did not become healthy"; exit 3; fi

# ---- phase 1: the 16 ordered base pairs (9 scenarios each) ----
for C in "${LANGS[@]}"; do
  for H in "${LANGS[@]}"; do
    run_pair "$C" "$H" base STATUS DETAIL
  done
done

# ---- phase 1b: the 4 commander self-checks ----
for C in "${LANGS[@]}"; do
  run_selfcheck "$C"
done

# ---- phase 2: deep phase on the fixed pair mapping ----
for scenario in "${DEEP_SCENARIOS[@]}"; do
  for pair in "${DEEP_PAIRS[@]}"; do
    read -r C H <<<"$pair"
    run_deep_pair "$C" "$H" "$scenario"
  done
done

# ---- phase 3: QoS-1 adapter-compatibility pass (full 16 pairs) ----
for C in "${LANGS[@]}"; do
  for H in "${LANGS[@]}"; do
    run_pair "$C" "$H" qos1 QOS_STATUS QOS_DETAIL
  done
done

T_ELAPSED=$(( SECONDS - T_START ))

# ---- grid ----
reds=0; ambers=0

print_grid() {
  local title="$1"; shift
  local -n _M="$1"
  echo
  echo "======================================================================"
  echo " ${title}   (rows = commander, cols = handler)"
  echo "======================================================================"
  printf "%-11s" "cmdr\\hdlr"
  for H in "${LANGS[@]}"; do printf "%-9s" "$H"; done
  echo
  for C in "${LANGS[@]}"; do
    printf "%-11s" "$C"
    for H in "${LANGS[@]}"; do
      s="${_M[${C}_${H}]:-?}"
      printf "%-9s" "$s"
      [ "$s" = "RED" ] && reds=$((reds+1))
      [ "$s" = "AMBER" ] && ambers=$((ambers+1))
    done
    echo
  done
}

print_grid "Parley interop matrix — BASE (9 scenarios) — run '${LABEL}'" STATUS

echo
echo "self-checks (\$noAck, no handler):"
for C in "${LANGS[@]}"; do
  s="${SC_STATUS[$C]:-?}"
  printf "  %-8s %s\n" "$C" "$s"
  [ "$s" = "RED" ] && reds=$((reds+1))
  [ "$s" = "AMBER" ] && ambers=$((ambers+1))
done

echo
echo "======================================================================"
echo " DEEP phase — driver-orchestrated disruptions (fixed pairs) — run '${LABEL}'"
echo "======================================================================"
for scenario in "${DEEP_SCENARIOS[@]}"; do
  echo "  ${scenario}:"
  for pair in "${DEEP_PAIRS[@]}"; do
    read -r C H <<<"$pair"
    s="${DEEP_STATUS[${scenario}_${C}_${H}]:-?}"
    printf "    %-8s -> %-8s %s\n" "$C" "$H" "$s"
    [ "$s" = "RED" ] && reds=$((reds+1))
    [ "$s" = "AMBER" ] && ambers=$((ambers+1))
  done
done

print_grid "QoS-1 ADAPTER-COMPATIBILITY (PUBACK flow / no stall; NOT redelivery dedup) — run '${LABEL}'" QOS_STATUS

echo
echo "----------------------------------------------------------------------"
echo "totals: reds=${reds}  ambers=${ambers}   wall-clock: ${T_ELAPSED}s"
echo "======================================================================"

# ---- details for any non-GREEN cell ----
if (( reds > 0 || ambers > 0 )); then
  echo
  echo "Non-GREEN detail:"
  for C in "${LANGS[@]}"; do
    for H in "${LANGS[@]}"; do
      key="${C}_${H}"
      if [ "${STATUS[$key]:-}" != "GREEN" ]; then
        echo "--- [base] ${C} -> ${H} : ${STATUS[$key]:-?}"
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
  for scenario in "${DEEP_SCENARIOS[@]}"; do
    for pair in "${DEEP_PAIRS[@]}"; do
      read -r C H <<<"$pair"
      key="${scenario}_${C}_${H}"
      if [ "${DEEP_STATUS[$key]:-}" != "GREEN" ]; then
        echo "--- [deep:${scenario}] ${C} -> ${H} : ${DEEP_STATUS[$key]:-?}"
        echo "${DEEP_DETAIL[$key]:-}"
      fi
    done
  done
  for C in "${LANGS[@]}"; do
    for H in "${LANGS[@]}"; do
      key="${C}_${H}"
      if [ "${QOS_STATUS[$key]:-}" != "GREEN" ]; then
        echo "--- [qos1] ${C} -> ${H} : ${QOS_STATUS[$key]:-?}"
        echo "${QOS_DETAIL[$key]:-}"
      fi
    done
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
echo "RESULT: ALL GREEN — base 16 pairs (9 scenarios) + 4 self-checks + 8 deep runs + qos-1 16 pairs, first try." >&2
exit 0
