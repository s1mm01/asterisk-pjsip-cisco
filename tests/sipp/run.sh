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

# On any exit (clean or via set -e from a failed scenario), copy
# asterisk's own log + a snapshot of its sorcery state into the
# trace dir so the CI upload-artifact step gets both sides of the
# conversation. Without this, only SIPp's view is captured and
# server-side decisions are invisible.
trap 'capture_asterisk_state' EXIT
capture_asterisk_state() {
    for f in /var/log/asterisk/*.log; do
        [ -r "$f" ] || continue
        sudo cp -f "$f" "$SIPP_TRACE_DIR/$(basename "$f")" 2>/dev/null || true
        sudo chmod a+r "$SIPP_TRACE_DIR/$(basename "$f")" 2>/dev/null || true
    done
    sudo asterisk -rx 'pjsip show endpoints' \
        > "$SIPP_TRACE_DIR/pjsip-endpoints.txt" 2>&1 || true
}

# Turn on pjsip wire logging so the SIP-level decisions for the
# inbound PUBLISH / SUBSCRIBE / REFER are visible in messages.log.
# Off by default in apt's asterisk; harmless to flip per run since
# the runner is throwaway.
sudo asterisk -rx 'pjsip set logger on' >/dev/null 2>&1 || true
sudo asterisk -rx 'core set verbose 5' >/dev/null 2>&1 || true
# core set debug 3 mirrors the live-PBX bench setup. Brings the
# distributor / endpoint-identifier / authenticator_digest /
# registrar debug into full.log so we can diff CI against the
# known-good live-PBX flow line by line.
sudo asterisk -rx 'core set debug 3' >/dev/null 2>&1 || true

# Run one scenario. Trace files land per-scenario in SIPP_TRACE_DIR
# so CI can upload them as a failure artifact without overlap.
#
# -m 1            run a single call (one REGISTER / SUBSCRIBE cycle)
# -trace_err      capture validation failures inline
# -trace_screen   per-call summary (pass/fail counters)
# -timeout 30s    SIPp-internal call-duration cap
# -nostdin        don't poll stdin for interactive UI keystrokes
#                 ("Press [q] to exit"); without this, sip-tester 3.7
#                 sits in the UI loop after -m calls complete.
# < /dev/null     defensive shell-side belt for the same purpose.
#
# Do NOT use -bg: it forks SIPp into the background and exits the
# foreground process with code 99 to signal "spawned ok". `set -e`
# in run.sh then aborts. -nostdin is the right flag for batch mode
# in foreground.
#
# Wrapped in `timeout 60s` as an outer safety net: if SIPp deadlocks
# anyway (kernel buffer wedge, lost packet, TCP handshake failure),
# the shell kills it and run.sh fails fast instead of timing out the
# whole CI job at the 6-hour mark.
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

    # Pre-scenario state dump — useful for understanding whether
    # prior scenarios populated the cisco_mac map before this one
    # runs (PATH C MAC identifier needs prior REGISTER harvest).
    case "$name" in
        dnd_publish)
            echo "--- pjsip cisco status 1050 (pre-scenario) ---"
            sudo asterisk -rx 'pjsip cisco status 1050' 2>&1 | head -25
            echo "--- (end pre-scenario state) ---"
            echo
            ;;
    esac

    timeout 60s sipp \
        -sf "$scenario" \
        -m 1 \
        -p "$SIPP_LOCAL_PORT" \
        -t t1 \
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

# Paired UAC+UAS runner for out-of-dialog inbound scenarios.
#
# When asterisk pushes an out-of-dialog request to the registered
# Contact (bulkupdate REFER, unsolicited NOTIFY), it carries a NEW
# Call-ID. SIPp 3.7 maps inbound by Call-ID and discards anything
# that doesn't match an active call — so a UAC-only scenario can't
# observe these.
#
# Workaround: split into two SIPp processes.
#   * UAS scenario binds the Contact URI's port (15060), starts in
#     receive mode. SIPp accepts new inbound dialogs by default in
#     UAS mode.
#   * UAC scenario uses a different local port (15061) for the
#     REGISTER exchange. The UAC's Contact header points at the
#     UAS port, so asterisk dispatches subsequent traffic there.
#   * Drop reg-id from the UAC's Contact to avoid RFC 5626 outbound
#     flow reuse — without it, asterisk does classic Contact-URI
#     dispatch and the inbound request lands on the UAS socket.
#
# Both scenarios run -m 1, so each handles exactly one inbound /
# outbound and exits.
run_paired() {
    local uac="$1"
    local uas="$2"
    local name
    name=$(basename "$uac" .uac.xml)

    echo
    echo "=== SIPp paired scenario: $name ==="
    echo "  asterisk:   $ASTERISK_HOST:$ASTERISK_PORT"
    echo "  sipp UAS:   0.0.0.0:$SIPP_LOCAL_PORT"
    echo "  sipp UAC:   0.0.0.0:$((SIPP_LOCAL_PORT + 1))"
    echo

    # Start UAS in background — needs to be bound and listening
    # before the UAC's REGISTER triggers asterisk's deferred task.
    timeout 60s sipp \
        -sf "$uas" \
        -m 1 \
        -p "$SIPP_LOCAL_PORT" \
        -t t1 \
        -nostdin \
        -trace_err -error_file "$SIPP_TRACE_DIR/$name.uas.err" \
        -trace_screen -screen_file "$SIPP_TRACE_DIR/$name.uas.screen" \
        -timeout 30s \
        "$ASTERISK_HOST:$ASTERISK_PORT" \
        < /dev/null &
    local uas_pid=$!

    # Brief delay so the UAS bind completes before we trigger
    # asterisk to push.
    sleep 1

    # UAC on a different port; sends REGISTER and waits for asterisk
    # to dispatch the out-of-dialog request to the UAS.
    timeout 60s sipp \
        -sf "$uac" \
        -m 1 \
        -p $((SIPP_LOCAL_PORT + 1)) \
        -t t1 \
        -nostdin \
        -trace_err -error_file "$SIPP_TRACE_DIR/$name.uac.err" \
        -trace_screen -screen_file "$SIPP_TRACE_DIR/$name.uac.screen" \
        -timeout 30s \
        "$ASTERISK_HOST:$ASTERISK_PORT" \
        < /dev/null
    local uac_rc=$?

    # UAS should have completed its single inbound by now.
    wait $uas_pid
    local uas_rc=$?

    if [ $uac_rc -ne 0 ] || [ $uas_rc -ne 0 ]; then
        echo "::error::paired scenario $name failed (UAC=$uac_rc, UAS=$uas_rc)"
        return 1
    fi
}

# Iterate scenarios. *.uac.xml files have a matching *.uas.xml and
# run as paired tests; *.uas.xml files are picked up via that pairing
# (skip them here). Everything else runs as a single SIPp UAC scenario.
for scenario in "$SCRIPT_DIR"/*.xml; do
    [ -f "$scenario" ] || continue
    case "$scenario" in
        *.uas.xml)
            continue
            ;;
        *.uac.xml)
            uas="${scenario%.uac.xml}.uas.xml"
            if [ ! -f "$uas" ]; then
                echo "::error::$scenario has no matching ${uas##*/}"
                exit 1
            fi
            run_paired "$scenario" "$uas"
            ;;
        *)
            run_scenario "$scenario"
            ;;
    esac
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
    echo "::error::1050 DND state was not set by PATH C PUBLISH"
    fail=1
fi

if [ "$fail" -ne 0 ]; then
    exit 1
fi

echo "OK: every scenario passed; all side-effect assertions hold."
