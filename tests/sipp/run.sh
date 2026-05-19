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
SIPP_BASE_PORT="${SIPP_LOCAL_PORT:-15060}"
SIPP_PORT_STRIDE="${SIPP_PORT_STRIDE:-10}"
SIPP_TRACE_DIR="${SIPP_TRACE_DIR:-/tmp/sipp-traces}"
SIPP_RENDER_DIR="$SIPP_TRACE_DIR/rendered-scenarios"
SIPP_NEXT_PORT="$SIPP_BASE_PORT"

mkdir -p "$SIPP_TRACE_DIR" "$SIPP_RENDER_DIR"

# Advance SIPP_NEXT_PORT and stash the picked port in
# SIPP_ALLOCATED_PORT for the caller to read.
#
# DO NOT change this to `echo "$port"` + `port=$(next_sipp_port)`:
# $() runs the function in a subshell, the SIPP_NEXT_PORT increment
# is lost when the subshell exits, and every call returns the same
# (base) port. Result: scenarios all share one port and the next
# scenario's listener sees late REFER/NOTIFY traffic from the
# previous one's REGISTER.
next_sipp_port() {
    SIPP_ALLOCATED_PORT="$SIPP_NEXT_PORT"
    SIPP_NEXT_PORT=$((SIPP_NEXT_PORT + SIPP_PORT_STRIDE))
}

render_scenario() {
    local scenario="$1"
    local contact_port="${2:-}"
    local rendered="$SIPP_RENDER_DIR/$(basename "$scenario")"

    if [ -n "$contact_port" ]; then
        sed "s/__SIPP_CONTACT_PORT__/$contact_port/g" \
            "$scenario" > "$rendered"
        echo "$rendered"
    else
        echo "$scenario"
    fi
}

assert_no_unrendered_placeholders() {
    local scenario="$1"

    if grep -q "__SIPP_CONTACT_PORT__" "$scenario"; then
        echo "::error::$scenario still contains an unrendered contact-port placeholder"
        return 1
    fi
}

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
    local local_port

    name=$(basename "$scenario" .xml)
    next_sipp_port
    local_port="$SIPP_ALLOCATED_PORT"
    assert_no_unrendered_placeholders "$scenario"

    echo
    echo "=== SIPp scenario: $name ==="
    echo "  asterisk:   $ASTERISK_HOST:$ASTERISK_PORT"
    echo "  sipp local: 0.0.0.0:$local_port"
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
        -p "$local_port" \
        -t t1 \
        -nostdin \
        -trace_err -error_file "$SIPP_TRACE_DIR/$name.err" \
        -trace_screen -screen_file "$SIPP_TRACE_DIR/$name.screen" \
        -timeout 30s \
        -deadcall_wait 0 \
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
#   * UAS scenario binds the Contact URI's port, starts in receive
#     mode, and accepts new inbound dialogs by default in UAS mode.
#   * UAC scenario uses the next port for the REGISTER exchange.
#     The UAC's Contact header points at the UAS port, so asterisk
#     dispatches subsequent traffic there.
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
    local uas_port
    local uac_port
    local rendered_uac
    local rendered_uas
    local uac_rc
    local uas_rc

    name=$(basename "$uac" .uac.xml)
    next_sipp_port
    uas_port="$SIPP_ALLOCATED_PORT"
    uac_port=$((uas_port + 1))
    rendered_uac=$(render_scenario "$uac" "$uas_port")
    rendered_uas=$(render_scenario "$uas" "$uas_port")
    assert_no_unrendered_placeholders "$rendered_uac"
    assert_no_unrendered_placeholders "$rendered_uas"

    echo
    echo "=== SIPp paired scenario: $name ==="
    echo "  asterisk:   $ASTERISK_HOST:$ASTERISK_PORT"
    echo "  sipp UAS:   0.0.0.0:$uas_port"
    echo "  sipp UAC:   0.0.0.0:$uac_port"
    echo

    # Start UAS in background — needs to be bound and listening
    # before the UAC's REGISTER triggers asterisk's deferred task.
    timeout 60s sipp \
        -sf "$rendered_uas" \
        -m 1 \
        -p "$uas_port" \
        -t t1 \
        -nostdin \
        -trace_err -error_file "$SIPP_TRACE_DIR/$name.uas.err" \
        -trace_screen -screen_file "$SIPP_TRACE_DIR/$name.uas.screen" \
        -timeout 30s \
        -deadcall_wait 0 \
        "$ASTERISK_HOST:$ASTERISK_PORT" \
        < /dev/null &
    local uas_pid=$!

    # Brief delay so the UAS bind completes before we trigger
    # asterisk to push.
    sleep 1

    # UAC on a different port; sends REGISTER and waits for asterisk
    # to dispatch the out-of-dialog request to the UAS.
    set +e
    timeout 60s sipp \
        -sf "$rendered_uac" \
        -m 1 \
        -p "$uac_port" \
        -t t1 \
        -nostdin \
        -trace_err -error_file "$SIPP_TRACE_DIR/$name.uac.err" \
        -trace_screen -screen_file "$SIPP_TRACE_DIR/$name.uac.screen" \
        -timeout 30s \
        -deadcall_wait 0 \
        "$ASTERISK_HOST:$ASTERISK_PORT" \
        < /dev/null
    uac_rc=$?

    if [ $uac_rc -ne 0 ]; then
        kill "$uas_pid" 2>/dev/null || true
    fi

    # UAS should have completed its single inbound by now.
    wait $uas_pid
    uas_rc=$?
    set -e

    if [ $uac_rc -ne 0 ] || [ $uas_rc -ne 0 ]; then
        echo "::error::paired scenario $name failed (UAC=$uac_rc, UAS=$uas_rc)"
        return 1
    fi
}

run_unsolicited_blf() {
    local uac="$1"
    local name
    local uas_port
    local uac_port
    local rendered_uac
    local collector_pid
    local collector_rc
    local uac_rc

    name=$(basename "$uac" .uac.xml)
    next_sipp_port
    uas_port="$SIPP_ALLOCATED_PORT"
    uac_port=$((uas_port + 1))
    rendered_uac=$(render_scenario "$uac" "$uas_port")
    assert_no_unrendered_placeholders "$rendered_uac"

    echo
    echo "=== SIPp + collector scenario: $name ==="
    echo "  asterisk:   $ASTERISK_HOST:$ASTERISK_PORT"
    echo "  collector:  0.0.0.0:$uas_port"
    echo "  sipp UAC:   0.0.0.0:$uac_port"
    echo

    python3 "$SCRIPT_DIR/collect_unsolicited_blf.py" \
        --host 0.0.0.0 \
        --port "$uas_port" \
        --timeout 30 \
        --expected-tuple 1030 \
        --expected-tuple 1050 \
        > "$SIPP_TRACE_DIR/$name.collector.log" 2>&1 &
    collector_pid=$!

    # Brief delay so the collector bind completes before REGISTER
    # triggers asterisk's deferred unsolicited-NOTIFY task.
    sleep 1

    set +e
    timeout 60s sipp \
        -sf "$rendered_uac" \
        -m 1 \
        -p "$uac_port" \
        -t t1 \
        -nostdin \
        -trace_err -error_file "$SIPP_TRACE_DIR/$name.uac.err" \
        -trace_screen -screen_file "$SIPP_TRACE_DIR/$name.uac.screen" \
        -timeout 30s \
        -deadcall_wait 0 \
        "$ASTERISK_HOST:$ASTERISK_PORT" \
        < /dev/null
    uac_rc=$?

    if [ $uac_rc -ne 0 ]; then
        kill "$collector_pid" 2>/dev/null || true
    fi

    wait "$collector_pid"
    collector_rc=$?
    set -e

    if [ $uac_rc -ne 0 ] || [ $collector_rc -ne 0 ]; then
        echo "::error::collector scenario $name failed (UAC=$uac_rc, collector=$collector_rc)"
        echo "--- $SIPP_TRACE_DIR/$name.collector.log ---"
        cat "$SIPP_TRACE_DIR/$name.collector.log" 2>/dev/null || true
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
        */unsolicited_blf.uac.xml)
            run_unsolicited_blf "$scenario"
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
if ! echo "$status_1050" | grep -qE "DND/1050: +ON"; then
    echo "::error::1050 DND state was not set by PATH C PUBLISH"
    fail=1
fi

if [ "$fail" -ne 0 ]; then
    exit 1
fi

echo "OK: every scenario passed; all side-effect assertions hold."
