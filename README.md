# asterisk-pjsip-cisco

Out-of-tree Asterisk shared modules that make **Cisco Enterprise SIP
firmware** phones (7975, 8841, 8861, 8865, …) work natively with
`chan_pjsip` — including BLF Speed Dial buttons, Cisco service-control
(`check-sync`/`restart`/`reset`), and the per-line bulkupdate config
the firmware expects after a successful REGISTER.

**No Asterisk core patching.** Ten independent shared modules built
against the public `res_pjsip` API.

## Why this exists

Cisco's Enterprise SIP firmware is designed for CallManager and goes
beyond plain RFC 3261/3265/4235. Stock `chan_pjsip` registers a
Cisco phone fine and places calls correctly, but the line buttons
render as plain Speed Dial (keypad icon, no LED state) instead of BLF
Speed Dial (hook icon, lit/unlit LED). To get BLF working the
firmware requires four independent server-side behaviours:

1. NOTIFY bodies in **Cisco-flavoured PIDF** (modern PIDF + RFC 4480
   RPID + Cisco's private `xmlns:ce` rpid extensions).
2. The REGISTER 200 OK carrying a Cisco RemoteCC **optionsind** body
   that advertises `<presence usage="blf speed dial">…`.
3. An unsolicited REFER right after REGISTER carrying a multipart
   **bulkupdate** with per-line `<contact line="N">` config.
4. Server-pushed **unsolicited NOTIFYs** for each watched extension —
   not just NOTIFYs in response to the phone's own SUBSCRIBE.

The Cisco firmware's BLF UI logic checks all four. Missing any one
silently downgrades the buttons to plain Speed Dial.

These ten modules supply all four behaviours, plus service-control
and phone softkey handling. See [ARCHITECTURE.md](ARCHITECTURE.md) for
the detailed mapping.

## Modules

| Module | Purpose |
|---|---|
| `res_pjsip_cisco_endpoint` | Defines the `cisco` sorcery type. Existence of `[name] type=cisco` in `pjsip.conf` is the gating signal for all Cisco-specific behaviour. |
| `res_pjsip_cisco_pidf_body_generator` | Adds Cisco RPID extensions to PIDF NOTIFY bodies. Coexists with stock `res_pjsip_pidf_body_generator` via the body-supplement API; takes over `cpim-pidf+xml` from stock `res_pjsip_xpidf_body_generator` by load_pri ordering. |
| `res_pjsip_cisco_register_optionsind` | Attaches the Cisco RemoteCC optionsind body to outgoing REGISTER 200 OK responses for Cisco endpoints. |
| `res_pjsip_cisco_bulkupdate` | After a Cisco peer REGISTERs, sends an unsolicited REFER carrying multipart bulkupdate body (DND + hunt-group + per-line config). |
| `res_pjsip_cisco_unsolicited_blf` | After a Cisco peer REGISTERs, sends an unsolicited Event: presence NOTIFY for each `subscribe=` extension. |
| `res_pjsip_cisco_service_control` | CLI: `pjsip cisco {check-sync,restart,reset} <endpoint> [contact]` — `[contact]` (a URI substring or `@hash`) targets one phone of a shared line. |
| `res_pjsip_cisco_feature_events` | Handles Cisco DND/CFwdALL softkey state from SUBSCRIBE and PUBLISH (stored in astdb), and resolves the MAC-address From-URI Cisco firmware uses on device-level REFER/PUBLISH by harvesting a MAC→endpoint hint from each authenticated REGISTER. |
| `res_pjsip_cisco_call_extras` | Adds Cisco call signaling extras: `Call-Info` RemoteCC metadata, `Supported: X-cisco-sis-10.0.0`, callback number in RPID, and H.264 SDP hints. |
| `res_pjsip_cisco_remotecc` | Handles Cisco RemoteCC REFERs: token/alarm responses, HLog, MCID, and the **Park / ParkMonitor** softkeys (parks the call into `res_parking` and pushes the slot back to the phone — see [Call parking](#call-parking)). MCID resolves Cisco XML dialog IDs through PJSIP's native dialog lookup. Other softkeys (Confrn, Join, …) are logged and `603`-declined instead of falling through to normal REFER transfer handling. |
| `res_pjsip_cisco_conference` | ConfList softkey: read-only inventory of the bridge participants for the active call leg, surfaced as a Cisco `<CiscoIPPhoneMenu>` on the phone. Phase 1 — Mute/Remove/Update softkey actions and conference building (Confrn / Join) are not yet implemented. |

## Requirements

- Asterisk 20.x (LTS) or newer — CI builds against the latest 20.x,
  22.x, and 23.x releases. See [Compatibility](#compatibility).
- `asterisk-dev` package (or equivalent — provides
  `/usr/include/asterisk/*.h`).
- `libpjproject-dev` (Debian/Ubuntu) or equivalent pjproject headers.
- A working `chan_pjsip` setup (transports, endpoints, AORs, auth).

## Build & install

You need Asterisk's headers (`asterisk-dev`) plus pjproject's headers.
**pjproject does not ship as a Debian/Ubuntu system package** — it's
bundled inside Asterisk's source. Two ways to give the build the
headers it needs:

**Option A: point at an existing Asterisk source tree (recommended
for self-builds).** If you have the Asterisk source you already
build/install from, the bundled pjproject is at
`<asterisk-src>/third-party/pjproject/source/`. Use:

```sh
git clone https://github.com/s1mm01/asterisk-pjsip-cisco
cd asterisk-pjsip-cisco
make PJPROJECT_DIR=/path/to/asterisk-22.9.0
sudo make install
```

When `PJPROJECT_DIR` is set, the Makefile also derives
`ASTERISK_INCLUDE_DIR` from `<asterisk-src>/include/`. This is
**important if you self-build asterisk**: `asterisk-dev`'s
`/usr/include/asterisk/*.h` is frequently stale relative to a
locally-built asterisk binary, and any code (ours included) compiled
against the stale headers crashes at runtime when struct field
offsets diverge. Building against the source tree's headers keeps
struct layouts in lockstep with the running binary.

**Option B: fetch a standalone pjproject release.** If you don't keep
an Asterisk source tree around, download pjproject yourself:

```sh
sudo apt install asterisk-dev
wget https://github.com/pjsip/pjproject/archive/refs/tags/2.16.tar.gz
tar xzf 2.16.tar.gz && mv pjproject-2.16 pjproject
: > pjproject/pjlib/include/pj/config_site.h   # empty user-config
(cd pjproject && ./configure --disable-libwebrtc-aec >/dev/null)

git clone https://github.com/s1mm01/asterisk-pjsip-cisco
cd asterisk-pjsip-cisco
make PJPROJECT_INCLUDE="\
    -DPJ_AUTOCONF=1 \
    -I$PWD/../pjproject/pjlib/include \
    -I$PWD/../pjproject/pjlib-util/include \
    -I$PWD/../pjproject/pjnath/include \
    -I$PWD/../pjproject/pjmedia/include \
    -I$PWD/../pjproject/pjsip/include"
sudo make install
```

Pin pjproject's version to whatever your installed Asterisk bundles —
if you have the matching Asterisk source tree, check
`<asterisk-src>/third-party/versions.mak` for `PJPROJECT_VERSION`.
ABI mismatches between pjproject versions are real and will surface as
crashes at runtime, not compile errors.

No `modules.conf` changes required — our PIDF module supplements stock
`res_pjsip_pidf_body_generator` (additive, both run) and out-races
stock `res_pjsip_xpidf_body_generator` for `cpim-pidf+xml` via a
lower `load_pri`. You'll see one harmless WARNING at startup
("A body generator for application/cpim-pidf+xml is already
registered") — that's stock xpidf declining after we got there first.

Restart Asterisk:

```sh
sudo systemctl restart asterisk
```

Verify:

```sh
sudo asterisk -rx 'module show like cisco_'
# Expect ten modules in Running state.
```

## Configuration

For each Cisco endpoint, add a parallel `[name] type=cisco` section
in `pjsip.conf` (same name as the existing endpoint section):

```ini
; Existing endpoint config (already there)
[1010]
type            = endpoint
transport       = transport-tcp-lan
context         = local_sip_phone
disallow        = all
allow           = opus,g722
dtmf_mode       = rfc4733
mailboxes       = 1000@default
auth            = auth-1010
aors            = 1010
; ... etc.

[1010]
type            = cisco
line_index      = 1
subscribe       = 1001,1002,1003,1004,1005,1006,1007
subscribe_context = local_sip_phone
dnd_busy        = no
```

Reload PJSIP:

```sh
sudo asterisk -rx 'module reload res_pjsip.so'
```

Power-cycle the phone (a soft reboot via menu may keep stale
subscription state — pull power for a clean test) and watch the line
buttons re-render with hook icons.

## Service-control CLI

```sh
asterisk -rx 'pjsip cisco check-sync 1010'   # phone refetches TFTP config
asterisk -rx 'pjsip cisco restart    1010'   # soft restart
asterisk -rx 'pjsip cisco reset      1010'   # hard reboot
```

Each verb takes an optional trailing `[contact]` — a substring matched
against a registered contact's URI or its `@hash` — so on a shared line
(`max_contacts > 1`, several physical phones) you can target just one:

```sh
asterisk -rx 'pjsip cisco restart 1020 192.0.2.50'   # only the LAN phone
asterisk -rx 'pjsip cisco restart 1020 1b2c3d4e5f'       # ...or by @hash
asterisk -rx 'pjsip cisco restart 1020'                  # both/all contacts
```

Tab-completion works on both arguments (endpoint = `type=cisco` ids,
contact = that endpoint's registered contact URIs). The body of a
restart/reset carries the phone's own REGISTER `Call-ID` (read
per-contact from the registrar — the firmware validates it and 400s a
mismatch). Refuses gracefully on non-Cisco endpoints (no `[name]
type=cisco` section) and on non-existent endpoints.

## Bulkupdate CLI

```sh
asterisk -rx 'pjsip cisco bulkupdate 1010'
```

Use this after dialplan, AMI, or database tooling changes
`DND/<endpoint>`, `CF/<endpoint>`, or related astdb state outside the
phone softkeys. It queues the same Cisco bulkupdate REFER sent after
REGISTER, carrying current DND, hunt-group, call-forward, and MWI
state to every registered contact for that endpoint.

## Call parking

The Cisco **Park** softkey (and its `ParkMonitor` variant) works:
`res_pjsip_cisco_remotecc` handles the RemoteCC REFER by blind-
transferring the bridge peer to the parkext (default `700`, in the
parker's transfer context — `TRANSFER_CONTEXT` chan var, else the
endpoint's context), so Asterisk's stock `res_parking` allocates the
slot, arms comeback-to-origin and tears down the phone's leg. The phone
then gets a status toast announcing the slot (`Park`), or — for
`ParkMonitor` — an `Event: refer` / `dialog-info+xml` NOTIFY per
parked-call lifecycle event so a "Park slot N" line button tracks the
orbit.

Requirements:

- **`res_parking` configured** — a lot in `res_parking.conf` (e.g. the
  `[default]` lot with `parkext => 700`, `parkpos => 701-720`,
  `comebacktoorigin => yes`). Verify: `asterisk -rx 'parking show'`.
  Without it, the Park softkey just `503`s.
- **The parkext (and slots) reachable from the phones' context** so
  `res_parking`'s comeback dial and manual retrieval (dialling `701`-…)
  resolve. `include => parkedcalls` does this — **but** a `_X.` (or
  other broad) catch-all in that context shadows the include (Asterisk
  consults included contexts only after the current context's own
  patterns), so claim the park range explicitly with patterns that
  out-rank the catch-all, e.g.:
  ```ini
  exten => _70[0-9],1,Goto(parkedcalls,${EXTEN},1)
  exten => _71[0-9],1,Goto(parkedcalls,${EXTEN},1)
  exten => 720,1,Goto(parkedcalls,720,1)
  ```
- If a site uses a non-default parkext, set `parkext` on the `type=cisco`
  section(s) to match `res_parking.conf` (defaults to `700`).

See [`conf-samples/extensions.conf.cisco-features.sample`](conf-samples/extensions.conf.cisco-features.sample)
for a worked example.

## Hardware-button replacements (dialplan)

The Cisco firmware's **Conference** (Confrn / Join) and **GPickUp**
hardware buttons need CallManager-style server support that this project
doesn't (yet) ship — `res_pjsip_cisco_remotecc` `603`-declines those
RemoteCC softkeys. Equivalent feature codes that work via the standard
Trnsfer softkey are in
[`conf-samples/extensions.conf.cisco-features.sample`](conf-samples/extensions.conf.cisco-features.sample) —
group pickup (`*8`), ad-hoc ConfBridge meet-me (8001-8009), and a
CallManager-style chained-transfer conference (`*0<ext>`). The file also
has a print-and-stick reference card for end users. (Park is now native
— see [Call parking](#call-parking) above.)

The ConfList softkey works against any of these: in a meet-me or
chained-transfer conference, pressing ConfList shows the participant
list (read-only — see `res_pjsip_cisco_conference` in the module
table).

## Configuration reference

`[name] type=cisco` accepts:

| Field | Type | Default | Meaning |
|---|---|---|---|
| `line_index` | int | `1` | Cisco line button index for the primary line. |
| `subscribe` | string (CSV) | `""` | Comma-separated extensions to push state for via unsolicited NOTIFY at REGISTER time. |
| `subscribe_context` | string | `local_sip_phone` | Dialplan context to look up the `subscribe=` extensions in. |
| `dnd_busy` | bool | `no` | When DND is enabled, reject calls as busy (`yes`) or let them through with the ringer off (`no`). |
| `aliases` | string (CSV) | `""` | Other cisco-endpoint IDs that share the same physical phone (one per line button). Empty = single-line phone. When set, the bulkupdate REFER emits a `<contact line="N">` element per alias, sourcing `line_index` from each alias's own `[name] type=cisco` section. |
| `parkext` | string | `700` | Dialplan extension the RemoteCC Park softkey blind-transfers the call to — i.e. `res_parking`'s parkext. Change to match `parkext` in `res_parking.conf` if a site uses a different one. The context is resolved per call (`TRANSFER_CONTEXT` chan var, else this endpoint's context). See [Call parking](#call-parking). |

## Runtime state

Bulkupdate bodies use live state: DND from `DND/<endpoint>`,
hunt-group login from `HuntGroup/<endpoint>`, call-forward from
`CF/<endpoint>`, and MWI counts from endpoint/AOR mailbox settings.
The feature-events module updates the DND and CF keys when users press
the Cisco phone softkeys. The RemoteCC module updates `HuntGroup/<endpoint>`
when users press HLog and sends a matching `<hlogupdate>` REFER back
to registered contacts. MCID resolves the XML `<dialogid>` to a live
session via PJSIP's native `pjsip_ua_find_dialog` and queues
`AST_CONTROL_MCID` on the bridged peer.

## Compatibility

- **Asterisk 22.9.x** — primary target; what the test bench runs on.
- **Asterisk 20.x (LTS)** — builds clean; `res_pjsip_pubsub`'s body-
  generator/supplement path (which the PIDF coexistence relies on) is
  byte-identical to 22.9, and the `ast_sip_*` / bridging / parking /
  Stasis-filter APIs we use are all present in 20.x. CI'd; not yet
  bench-tested against a phone on 20.x.
- **Asterisk 23.x** — builds clean; `ast_sip_*` API signatures have
  been stable. CI'd; untested against a phone.
- **Asterisk 21 / ≤19** — 21 is EOL; ≤19 predates struct fields we use
  (`ast_sip_contact.call_id`, the `ast_channel_snapshot` `base`
  substruct). Not targeted.

## Testing recipe

See [tests/README.md](tests/README.md) for a step-by-step recipe
(parallel TCP transport on `:5160`, swap a single phone over by
editing its SEP file, capture and verify the four wire signals).

## License

GPL-2.0 — matches Asterisk's licence (we link against its headers).

## Acknowledgements

- The Cisco-firmware-behaviour reverse-engineering owes everything to
  Gareth Palmer's `cisco-usecallmanager` patch for chan_sip
  (https://usecallmanager.nz). The XML body shapes, REGISTER flow,
  and post-REGISTER REFER mechanics here are line-mapped from his
  patched chan_sip source. This project's contribution is porting
  those behaviours to chan_pjsip as out-of-tree modules.
