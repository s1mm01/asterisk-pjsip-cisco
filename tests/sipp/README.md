# tests/sipp — SIPp wire-format scenarios

End-to-end scenarios that drive a real asterisk (loaded with the
project's modules and `tests/ci/pjsip.conf`) through Cisco-flavoured
SIP flows, asserting both the wire-format response and the
post-flow side-effects we expect on the asterisk side.

Complementary to:

- **`tests/unit/test_xml_bodies`** — sprintf well-formedness of the
  body templates in isolation (no asterisk runtime).
- **`tests/unit/test_blf_pidf`** — golden-output check for a single
  body-builder helper compiled against a stub Asterisk API.
- **CI module-load smoke** — confirms .so files dlopen against apt's
  asterisk and sorcery accepts the type=cisco field set.

What this layer adds: the actual wire flows between a SIPp-driven
"phone" and a running asterisk produce the responses we expect, and
the server-side state (MAC harvest, Reason-header device facts) is
populated as a side-effect.

## Scenarios

### `register_optionsind.xml`

Cisco REGISTER round-trip (initial → 401 → auth → 200 OK → de-register),
with the Contact `+sip.instance` and `Reason:` headers a real
Cisco Enterprise SIP phone sends.

Asserts:

- 200 OK body carries `<x-cisco-remotecc-response>` with an inner
  `<optionsind>` element (the multipart shape `res_pjsip_cisco_register_optionsind`
  emits at REGISTER time).
- After the run, `pjsip cisco status 1010` reports:
  - `MAC: aabbccddeeff` (harvested from `+sip.instance` urn:uuid)
  - `Device name: SEPAABBCCDDEEFF` (parsed from `Reason:`)
  - `Active firmware load: sip8865.12-1-1-12`
  - `Inactive firmware load: sip8865.cert-2014`

### `subscribe_presence.xml`

SUBSCRIBE Event: presence round-trip (initial → 401 → auth → 200 OK →
initial NOTIFY → ACK), with `1010` watching `1050`'s presence state.

Asserts:

- asterisk's pushed NOTIFY body carries
  `xmlns:ce="urn:cisco:params:xml:ns:pidf:rpid"` — the Cisco-private
  RPID namespace `res_pjsip_cisco_pidf_body_generator` adds beyond
  stock `res_pjsip_pidf_body_generator`'s output.
- The body has a `<presence>` root element (structural shape sanity).

Together with `test_blf_pidf` (unit-level golden output for the
body builder in isolation) this catches both "body-builder code
regressed" and "body-generator slot was lost to stock xpidf at
load-priority resolution".

### `bulkupdate_refer.xml`

Fourth of the four BLF-identity signals. When a Cisco endpoint
registers, asterisk pushes a REFER carrying a multipart bulkupdate
body — one `<contact line="N">` per line on the physical phone,
with the current DND / HuntGroup / CFwdAll state from astdb.

Uses `1031` (a multi-line alias of `1030` + `1032`, no `subscribe=`)
so the body exercises the multi-line consolidation path and no
unsolicited NOTIFYs complicate the recv ordering.

Asserts:

- `Refer-To` header present.
- `x-cisco-remotecc-request` wrapper in the multipart body.
- `<bulkupdate>` root element.
- `line="1"`, `line="2"`, `line="3"` all present — the three lines
  of the multi-line phone consolidated into one REFER, ordered by
  `line_index` from each `[type=cisco]` section.

### `dnd_publish.xml`

PATH B inbound DND state. Cisco phones publish DND via Event:
presence PUBLISH carrying a PIDF body with `<ce:dnd/>` in the
activities list. `res_pjsip_cisco_feature_events` parses the body
and writes `DND/<endpoint>` to astdb.

Uses `1050` to keep DND state changes scoped to one endpoint that
no other scenario writes to.

Asserts:

- PUBLISH accepted (200 OK).
- After the run, `pjsip cisco status 1050` reports `DND/1050: on` —
  the astdb write fired.

### `unsolicited_blf.xml`

Cisco's BLF Speed-Dial classification depends on the server pushing
unsolicited Event: presence NOTIFYs to the phone — without a prior
SUBSCRIBE — for every extension in the type=cisco `subscribe=` list.
This scenario REGISTERs as `1010` (whose `subscribe=` list is
`1030,1050`), then expects two NOTIFYs and asserts each is
Cisco-shaped.

The UAC half is still SIPp. The UAS half uses
`collect_unsolicited_blf.py` because each unsolicited NOTIFY is a
separate out-of-dialog request with its own Call-ID; SIPp 3.7 cannot
reliably express "REFER plus two independent NOTIFYs" as one linear
UAS scenario.

Asserts (per NOTIFY, twice):

- `Event: presence` header (no SUBSCRIBE preceded; this confirms the
  unsolicited-push path is firing).
- `urn:cisco:params:xml:ns:pidf:rpid` namespace in body.
- The body's `<tuple id="...">` is one of the two `subscribe=` entries
  (`1030` or `1050`); guards against subscription leaks.

This is the third of the four signals Cisco firmware needs to flip a
line button from plain Speed Dial to BLF Speed Dial. The other three:
SEP file (provisioning-side, out of scope), the 200-OK optionsind body
(see `register_optionsind.xml`), and the post-REGISTER bulkupdate
REFER (see `bulkupdate_refer.xml`).

## Running locally

Requires:

- `sip-tester` (Debian/Ubuntu) installed — provides the `sipp`
  binary
- `python3` for the unsolicited-BLF collector
- asterisk running with `tests/ci/pjsip.conf` loaded (declares a
  TCP transport on 127.0.0.1:5160 — Cisco Enterprise SIP firmware
  is SIP-over-TCP only, so the test scenarios match)
- The modules from this project installed (`sudo make install`)

```sh
sudo cp tests/ci/pjsip.conf      /etc/asterisk/pjsip.conf
sudo cp tests/ci/extensions.conf /etc/asterisk/extensions.conf
sudo systemctl restart asterisk
sudo asterisk -rx 'core waitfullybooted'

./tests/sipp/run.sh
```

Override the target asterisk via `ASTERISK_HOST` / `ASTERISK_PORT`
env vars; default is `127.0.0.1:5160` (matches the test pjsip.conf's
transport bind).

`SIPP_LOCAL_PORT` is the base port for simulated phone sockets
(default `15060`). `run.sh` advances by `SIPP_PORT_STRIDE` (default
`10`) for each scenario so delayed REFER/NOTIFY traffic from one
registered contact cannot land on the next scenario's listener.

Traces (per-call summary, errors) are written to `/tmp/sipp-traces/`;
override with `SIPP_TRACE_DIR=...`.

## Running in CI

The build matrix's load-test path (cell 20 today, more cells when
Ubuntu ships newer asterisk in any pocket) installs `sip-tester` and
runs every scenario in this directory after the existing
`verify type=cisco sorcery config loaded` step. Failures bubble up
via SIPp's non-zero exit and the post-flow grep assertions in
`run.sh`.

## What this layer doesn't cover

- Phone-side rendering (softkeys, BLF lamps, glyph display) —
  SIPp drives the wire only.
- Media negotiation / RTP — REGISTER carries no SDP; future
  INVITE-based scenarios will need to handle this.
- The 4-signal BLF identity insight (SEP file + optionsind +
  bulkupdate REFER + unsolicited NOTIFY all needed before Cisco
  firmware classifies a button as BLF). SIPp can drive the
  server-to-phone side of this but can't verify the phone's
  classification decision; that stays bench-only.
