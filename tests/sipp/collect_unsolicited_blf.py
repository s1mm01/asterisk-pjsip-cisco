#!/usr/bin/env python3
"""Collect unsolicited BLF NOTIFYs for the SIPp wire-format test.

SIPp 3.7 cannot model "accept REFER plus two NOTIFY requests, each with
an independent Call-ID" as one reliable linear UAS scenario. This helper
only covers that UAS half: it listens on the registered Contact port,
ACKs inbound REFER/NOTIFY requests with 200 OK, and asserts that the
expected Cisco-shaped NOTIFY bodies arrived.
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


def process_request(raw, expected, seen, errors, lock):
    text = raw.decode("iso-8859-1", "replace")
    request_line = text.split("\r\n", 1)[0]
    method = request_line.split(" ", 1)[0]

    if method == "REFER":
        print(f"ACK REFER: {request_line}", flush=True)
        return

    if method != "NOTIFY":
        with lock:
            errors.append(f"Unexpected SIP method on collector port: {request_line}")
        return

    print(f"ACK NOTIFY: {request_line}", flush=True)

    if not re.search(r"(?im)^Event\s*:\s*presence(?:\s*;|\s*$)", text):
        with lock:
            errors.append("NOTIFY missing Event: presence")

    if "urn:cisco:params:xml:ns:pidf:rpid" not in text:
        with lock:
            errors.append("NOTIFY missing Cisco PIDF RPID namespace")

    tuple_match = re.search(r'<tuple id="([^"]+)"', text)
    if not tuple_match:
        with lock:
            errors.append("NOTIFY missing <tuple id=\"...\">")
        return

    tuple_id = tuple_match.group(1)
    if tuple_id not in expected:
        with lock:
            errors.append(f"Unexpected NOTIFY tuple id {tuple_id}")
        return

    with lock:
        seen.add(tuple_id)
        print(f"seen tuple ids: {','.join(sorted(seen))}", flush=True)


def handle_client(conn, addr, expected, seen, errors, lock, stop):
    del addr
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
                process_request(raw, expected, seen, errors, lock)
                conn.sendall(response_for(raw.decode("iso-8859-1", "replace")))
    finally:
        conn.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("--grace", type=float, default=1.0)
    parser.add_argument("--expected-tuple", action="append", required=True)
    args = parser.parse_args()

    expected = set(args.expected_tuple)
    seen = set()
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
        f"collecting unsolicited BLF NOTIFYs on {args.host}:{args.port}; "
        f"expecting {','.join(sorted(expected))}",
        flush=True,
    )

    try:
        while time.monotonic() < deadline:
            now = time.monotonic()
            with lock:
                complete = expected.issubset(seen)

            if complete:
                if success_deadline is None:
                    success_deadline = now + args.grace
                elif now >= success_deadline:
                    break

            try:
                conn, addr = server.accept()
            except socket.timeout:
                continue

            thread = threading.Thread(
                target=handle_client,
                args=(conn, addr, expected, seen, errors, lock, stop),
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
        missing = expected - seen
        final_errors = list(errors)

    if missing:
        final_errors.append(f"Missing expected NOTIFY tuple ids: {','.join(sorted(missing))}")

    if final_errors:
        for error in final_errors:
            print(f"ERROR: {error}", file=sys.stderr)
        return 1

    print(f"OK: received expected tuple ids {','.join(sorted(seen))}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
