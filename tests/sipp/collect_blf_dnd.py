#!/usr/bin/env python3
"""Collect BLF NOTIFYs and assert the activity-dedup clearing contract.

Companion to the unsolicited_blf_dedup SIPp scenario. The runner registers
endpoint 1060 (which watches its own hint) so its Contact points here, then
toggles its DND on and off via `pjsip cisco donotdisturb`. Each toggle is a
presence change on the watched hint, so asterisk pushes an unsolicited
Event: presence NOTIFY to this port. This collector records, in order, the
NOTIFYs for the target tuple and asserts the sequence:

    ... <ce:dnd/> present (DND on) ... then later ... absent (DND cleared)

i.e. the *clearing* NOTIFY must arrive after the DND NOTIFY. That is the
observable guarantee of the activity-dedup gate: a transition back to a
prior wire state is never suppressed. (It validates the contract, not the
concurrent-callback reorder that motivated the serializer fix — that race
is timing-dependent and not reliably reproducible in CI.)

Also ACKs the bulkupdate REFER the DND toggle queues at 1060's own contact;
that one is just answered 200 and otherwise ignored.
"""

import argparse
import re
import socket
import sys
import threading
import time


HEADER_END = b"\r\n\r\n"


def extract_message(buffer):
    header_end = buffer.find(HEADER_END)
    if header_end == -1:
        return None, buffer

    header_text = buffer[:header_end].decode("iso-8859-1", "replace")
    content_length = 0
    for match in re.finditer(r"(?im)^Content-Length\s*:\s*(\d+)\s*$", header_text):
        content_length = int(match.group(1))

    total_length = header_end + len(HEADER_END) + content_length
    if len(buffer) < total_length:
        return None, buffer

    return buffer[:total_length], buffer[total_length:]


def response_for(request_text):
    lines = request_text.split("\r\n")
    response = ["SIP/2.0 200 OK"]
    copy_headers = {"via", "v", "from", "f", "to", "t", "call-id", "i", "cseq"}

    for line in lines[1:]:
        if not line:
            break
        name = line.split(":", 1)[0].strip().lower()
        if name in copy_headers:
            response.append(line)

    response.append("Content-Length: 0")
    return ("\r\n".join(response) + "\r\n\r\n").encode("ascii", "replace")


def process_request(raw, tuple_id, events, errors, lock):
    text = raw.decode("iso-8859-1", "replace")
    request_line = text.split("\r\n", 1)[0]
    method = request_line.split(" ", 1)[0]

    if method == "REFER":
        # DND toggles also queue a bulkupdate REFER at 1060's own contact;
        # ack it and move on.
        print(f"ACK REFER: {request_line}", flush=True)
        return

    if method != "NOTIFY":
        with lock:
            errors.append(f"Unexpected SIP method on collector port: {request_line}")
        return

    print(f"ACK NOTIFY: {request_line}", flush=True)

    if "urn:cisco:params:xml:ns:pidf:rpid" not in text:
        with lock:
            errors.append("NOTIFY missing Cisco PIDF RPID namespace")

    match = re.search(r'<tuple id="([^"]+)"', text)
    if not match:
        with lock:
            errors.append('NOTIFY missing <tuple id="...">')
        return

    if match.group(1) != tuple_id:
        # NOTIFYs for other watched extensions are uninteresting here.
        return

    has_dnd = "<ce:dnd/>" in text
    with lock:
        events.append("dnd" if has_dnd else "nodnd")
        print(f"tuple {tuple_id} sequence: {' -> '.join(events)}", flush=True)


def handle_client(conn, tuple_id, events, errors, lock, stop):
    buffer = b""
    conn.settimeout(0.2)
    try:
        while not stop.is_set():
            try:
                chunk = conn.recv(8192)
            except socket.timeout:
                continue
            if not chunk:
                break
            buffer += chunk
            while True:
                raw, buffer = extract_message(buffer)
                if raw is None:
                    break
                process_request(raw, tuple_id, events, errors, lock)
                conn.sendall(response_for(raw.decode("iso-8859-1", "replace")))
    finally:
        conn.close()


def sequence_satisfied(events):
    """True once a DND NOTIFY has been followed by a later clearing NOTIFY."""
    if "dnd" not in events:
        return False
    first_dnd = events.index("dnd")
    return "nodnd" in events[first_dnd + 1:]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("--grace", type=float, default=1.0)
    parser.add_argument("--tuple", required=True, help="watched tuple id, e.g. 1060")
    args = parser.parse_args()

    events = []
    errors = []
    lock = threading.Lock()
    stop = threading.Event()
    threads = []

    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind((args.host, args.port))
    server.listen()
    server.settimeout(0.2)

    deadline = time.monotonic() + args.timeout
    success_deadline = None
    print(
        f"collecting BLF DND NOTIFYs on {args.host}:{args.port}; "
        f"watching tuple {args.tuple} for <ce:dnd/> set then cleared",
        flush=True,
    )

    try:
        while time.monotonic() < deadline:
            now = time.monotonic()
            with lock:
                done = sequence_satisfied(events)
            if done:
                if success_deadline is None:
                    success_deadline = now + args.grace
                elif now >= success_deadline:
                    break
            try:
                conn, _ = server.accept()
            except socket.timeout:
                continue
            thread = threading.Thread(
                target=handle_client,
                args=(conn, args.tuple, events, errors, lock, stop),
                daemon=True,
            )
            thread.start()
            threads.append(thread)
    finally:
        stop.set()
        server.close()

    for thread in threads:
        thread.join(timeout=1.0)

    with lock:
        final_errors = list(errors)
        seq = list(events)

    if "dnd" not in seq:
        final_errors.append(
            f"no DND NOTIFY (<ce:dnd/>) ever seen for tuple {args.tuple}; "
            f"sequence={seq or '(none)'}"
        )
    elif not sequence_satisfied(seq):
        final_errors.append(
            f"DND NOTIFY seen but no clearing NOTIFY followed it for tuple "
            f"{args.tuple} — the cleared state was suppressed; sequence={seq}"
        )

    if final_errors:
        for error in final_errors:
            print(f"ERROR: {error}", file=sys.stderr)
        return 1

    print(f"OK: tuple {args.tuple} DND set then cleared on the wire; "
          f"sequence={seq}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
