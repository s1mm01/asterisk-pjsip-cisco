#!/bin/bash
#
# Drive the Cisco-flavoured SIPp scenarios against a running asterisk
# that has tests/ci/pjsip.conf + tests/ci/extensions.conf loaded (or
# any equivalent config that defines endpoint 1010 with auth=1010-auth
# / password=ci-no-secret, endpoint 1050 in the same auth realm, and
# hints for both in the [ci-test] context).
#
# Runs every *.xml scenario under tests/sipp/ in sorted order, then
# does cross-scenario side-effect checks (PATH C MAC + Reason-header
# harvest landed in the cisco_mac map; query via 'pjsip cisco status').
#
# Local-bench use:
#   sudo cp tests/ci/pjsip.conf       /etc/asterisk/pjsip.conf
#   sudo cp tests/ci/extensions.conf  /etc/asterisk/extensions.conf
#   sudo systemctl restart asterisk
#   sudo asterisk -rx 'core waitfullybooted'
#   ./tests/sipp/run.sh
#
# CI use: invoked by .github/workflows/ci.yml after the existing
# module-load + sorcery-config verify steps.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ASTERISK_HOST="${ASTERISK_HOST:-127.0.0.1}"
ASTERISK_PORT="${ASTERISK_PORT:-5160}"
SIPP_LOCAL_PORT="${SIPP_LOCAL_PORT:-15060}"
SIPP_TRACE_DIR="${SIPP_TRACE_DIR:-/tmp/sipp-traces}"

mkdir -p "$SIPP_TRACE_DIR"

# Run one scenario. Trace files land per-scenario in SIPP_TRACE_DIR
# so CI can upload them as a failure artifact without overlap.
#
# -m 1            run a single call (one REGISTER / SUBSCRIBE cycle)
# -trace_err      capture validation failures inline
# -trace_screen   per-call summary (pass/fail counters)
# -timeout 30s    SIPp-internal call-duration cap
# -bg             batch mode: after the -m calls complete, SIPp exits
#                 instead of dropping into its interactive UI loop
#                 ("Press [q] to exit"). Without this, sip-tester 3.7
#                 hangs forever in non-interactive environments.
# -nostdin        don't read stdin (defensive belt for runners that
#                 do attach a TTY to bash subprocesses).
# < /dev/null     same purpose as -nostdin via the shell side.
#
# Wrapped in `timeout 60s` as an outer safety net: if SIPp deadlocks
# anyway (kernel buffer wedge, lost packet, broken DNS), the shell
# kills it and run.sh fails fast instead of timing out the whole CI
# job at the 6-hour mark.
#
# Digest credentials are embedded in each scenario's [authentication
# username=... password=...] macro rather than passed via -au / -ap,
# because the scenarios drive different endpoints (1010 / 1031 /
# 1050) with per-endpoint auth sections in tests/ci/pjsip.conf.
run_scenario() {
    local scenario="$1"
    local name
    name=$(basename "$scenario" .xml)

    echo
    echo "=== SIPp scenario: $name ==="
    echo "  asterisk:   $ASTERISK_HOST:$ASTERISK_PORT"
    echo "  sipp local: 0.0.0.0:$SIPP_LOCAL_PORT"
    echo

    timeout 60s sipp \
        -sf "$scenario" \
        -m 1 \
        -p "$SIPP_LOCAL_PORT" \
        -t t1 \
        -bg \
        -nostdin \
        -trace_err -error_file "$SIPP_TRACE_DIR/$name.err" \
        -trace_screen -screen_file "$SIPP_TRACE_DIR/$name.screen" \
        -timeout 30s \
        "$ASTERISK_HOST:$ASTERISK_PORT" \
        < /dev/null
}

# -t t1 — TCP transport, single connection. Cisco Enterprise SIP
# firmware on real CP-78xx / 88xx phones is SIP-over-TCP only, and
# tests/ci/pjsip.conf accordingly declares a TCP-only transport
# bound to 127.0.0.1:5160. UDP fallback is intentionally absent —
# if the modules ever regress to assuming UDP-only retransmit
# semantics or short body buffers (Cisco bulkupdate REFER bodies
# routinely exceed 1.5 KB), CI surfaces it here instead of bench.

# Sorted iteration so register_optionsind runs first (subscribe_presence
# happens to follow alphabetically; if a future scenario needs an
# explicit ordering, rename to add a numeric prefix).
for scenario in "$SCRIPT_DIR"/*.xml; do
    [ -f "$scenario" ] || continue
    run_scenario "$scenario"
done

echo
echo "=== Cross-scenario side-effect checks ==="
fail=0

# register_optionsind -> PATH C MAC + Reason-header harvest landed
# in the cisco_mac map keyed by 1010's REGISTER.
echo "--- 1010: PATH C harvest (set by register_optionsind) ---"
status_1010=$(sudo asterisk -rx 'pjsip cisco status 1010')
echo "$status_1010"
echo
if ! echo "$status_1010" | grep -qE "MAC: +aabbccddeeff"; then
    echo "::error::1010 MAC was not harvested from +sip.instance"
    fail=1
fi
if ! echo "$status_1010" | grep -qE "Device name: +SEPAABBCCDDEEFF"; then
    echo "::error::1010 device name was not parsed from Reason header"
    fail=1
fi
if ! echo "$status_1010" | grep -qE "Active firmware load: +sip8865\.12-1-1-12"; then
    echo "::error::1010 active firmware load was not parsed from Reason header"
    fail=1
fi
if ! echo "$status_1010" | grep -qE "Inactive firmware load: +sip8865\.cert-2014"; then
    echo "::error::1010 inactive firmware load was not parsed from Reason header"
    fail=1
fi

# dnd_publish -> DND/1050 = on in astdb.
echo "--- 1050: DND state (set by dnd_publish) ---"
status_1050=$(sudo asterisk -rx 'pjsip cisco status 1050')
echo "$status_1050"
echo
if ! echo "$status_1050" | grep -qE "DND/1050: +on"; then
    echo "::error::1050 DND state was not set by PATH B PUBLISH"
    fail=1
fi

if [ "$fail" -ne 0 ]; then
    exit 1
fi

echo "OK: every scenario passed; all side-effect assertions hold."
