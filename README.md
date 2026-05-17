# asterisk-pjsip-cisco

[![build](https://github.com/s1mm01/asterisk-pjsip-cisco/actions/workflows/ci.yml/badge.svg)](https://github.com/s1mm01/asterisk-pjsip-cisco/actions/workflows/ci.yml)
[![release](https://img.shields.io/github/v/release/s1mm01/asterisk-pjsip-cisco)](https://github.com/s1mm01/asterisk-pjsip-cisco/releases)
[![license](https://img.shields.io/badge/license-GPL--2.0-blue.svg)](LICENSE)

Use Cisco Enterprise SIP phones with stock Asterisk `chan_pjsip`,
without CallManager, `chan_sip`, or Asterisk core patches.

This repository builds out-of-tree Asterisk modules that add the
Cisco-specific SIP signalling expected by Enterprise firmware. It is
bench-tested against CP7975G, CP8841, CP8861 and CP8865 on Asterisk
20.19, 22.9 and 23.3.

**Quick links:** [Features](#features) | [Compatibility](#compatibility) |
[Requirements](#requirements) | [Build & install](#build--install) |
[Configuration](#configuration) | [Operator CLI](#operator-cli) |
[Dialplan integration](#dialplan-integration) |
[Further reading](#further-reading)

## Features

### Phone UI

- BLF Speed Dial line buttons: hook icons, lit/unlit LEDs, state-tracked.
- MWI: voicemail lamp and message-count updates on registration and on
  every state change.
- DND softkey: toggles state and redraws lamps on watching phones.
- CFwdALL softkey: call-forward set/cancel with the on-phone banner.
- HLog softkey: hunt-group login/logout.
- Hunt-group and call-forward state pushed to the phone via Cisco
  bulkupdate after every REGISTER, so the line UI matches server state.

### Call Control

- Park / ParkMonitor softkeys via stock `res_parking`.
- Record softkey: Start / Stop recording of the active call via
  MixMonitor; per-call filename overridable from dialplan with the
  `CISCO_RECORD_FILENAME` channel variable.
- Conference softkeys: Confrn (3-way build), Join (multi-call merge),
  RmLastConf (remove most-recent participant), ConfList (read-only
  participant list).
- MCID, callback-number RPID, Cisco-side H.264 SDP hints, Call-Info
  RemoteCC metadata and other call-signaling extras the firmware expects.

### Cisco Signalling

- Cisco RemoteCC `optionsind` response on REGISTER.
- Post-REGISTER bulkupdate REFER for DND, HLog and call-forward state.
- Cisco RPID plus private `ce:` PIDF extensions on NOTIFY bodies, so the
  firmware accepts state updates instead of silently dropping them.
- Server-pushed unsolicited Event: presence NOTIFYs for watched
  extensions.

### Operations

- Service control from the CLI: `check-sync`, `restart`, `reset`, and
  Problem-Report-Tool upload.
- Optional contact targeting for shared-line phones, by registered URI
  substring or contact `@hash`.
- Dialplan functions for DND, hunt-group and call-forward state that
  update astdb and push the matching phone refresh.

Group pickup, ad-hoc ConfBridge meet-me, and a CallManager-style
chained 3-way conference are provided as dialplan recipes in
[`conf-samples/extensions.conf.cisco-features.sample`](conf-samples/extensions.conf.cisco-features.sample),
not modules.

For why this is needed - including the four independent server-side
signals the Cisco firmware checks before promoting a button to BLF, the
per-module rationale, and the wire-format mapping back to Gareth
Palmer's `chan_sip` patch - see [ARCHITECTURE.md](ARCHITECTURE.md).

## Compatibility

The primary bench target is Asterisk 22.9. CI also builds against the
latest Asterisk 20.x, 22.x and 23.x release tags.

| Asterisk | Status                                                     |
|----------|------------------------------------------------------------|
| 23       | Builds clean, validated in CI, bench-tested on real phones. |
| 22 (LTS) | Primary target; bench-tested on real phones.               |
| 21       | Not supported (EOL).                                       |
| 20 (LTS) | Builds clean, validated in CI, bench-tested on real phones. |
| <=19     | Not supported; predates struct fields this project uses.   |

## Project Status and Scope

- Targets Cisco Enterprise SIP firmware, not SCCP firmware.
- Endpoints opt in with a matching `[name] type=cisco` object; endpoints
  without that object pass through every module unchanged.
- Build against headers matching the running Asterisk binary. Mismatched
  Asterisk or pjproject headers can compile successfully but crash at
  runtime.
- This is an early 0.x project tested on the maintainer's bench and in
  CI. Field reports from more phone models, firmware versions and
  Asterisk versions are useful.

## Requirements

- Asterisk 20 (LTS) or later.
- Asterisk dev headers (`asterisk-dev` or a self-built source tree).
- pjproject headers (`libpjproject-dev`, or the bundled pjproject tree at
  `<asterisk-src>/third-party/pjproject/source/`).
- A working `chan_pjsip` install: transports, endpoints, AORs and auth.

## Build & Install

```sh
git clone https://github.com/s1mm01/asterisk-pjsip-cisco
cd asterisk-pjsip-cisco
make PJPROJECT_DIR=/path/to/asterisk-22.9.0
sudo make install
sudo systemctl restart asterisk
asterisk -rx 'module show like cisco_'   # expect ten modules Running
```

`PJPROJECT_DIR` pulls both Asterisk and bundled-pjproject headers from
one source tree so struct layouts stay in lockstep with the running
binary. If you only have `asterisk-dev` plus a separately unpacked
pjproject, see the Makefile header comment for the `PJPROJECT_INCLUDE=...`
alternative.

A harmless WARNING - `A body generator for application/cpim-pidf+xml is
already registered` - is expected at startup. That is stock
`res_pjsip_xpidf_body_generator` declining the slot after the
Cisco-specific generator has already claimed it.

## Configuration

For each Cisco phone, add a parallel `[name] type=cisco` section in
`pjsip.conf` with the same section name as the existing endpoint:

```ini
[1010]
type              = cisco
line_index        = 1
subscribe         = 1001,1002,1003,1004,1005
subscribe_context = local_sip_phone
```

Reload PJSIP, then power-cycle the phone:

```sh
sudo asterisk -rx 'module reload res_pjsip.so'
```

Every available field, multi-line phone setup, park-extension config and
DND-lamp wiring is documented inline in
[`conf-samples/pjsip.conf.cisco-section.sample`](conf-samples/pjsip.conf.cisco-section.sample).
That file is the configuration reference.

## Operator CLI

Diagnostic dump (read-only — config + registration + astdb feature state
for one endpoint, the shape you want when debugging "why don't the BLF
lamps light?" or "why isn't DND registering?"):

```sh
asterisk -rx 'pjsip cisco status 1010'
```

Service control:

```sh
asterisk -rx 'pjsip cisco check-sync 1010'   # refetch TFTP config
asterisk -rx 'pjsip cisco restart    1010'   # soft restart
asterisk -rx 'pjsip cisco reset      1010'   # hard reboot
asterisk -rx 'pjsip cisco prt-report 1010'   # phone uploads a PRT bundle
```

Each verb takes an optional trailing `[contact]`: a substring of a
registered contact URI, or its `@hash`. On a shared line, this targets a
single physical phone:

```sh
asterisk -rx 'pjsip cisco restart 1020 192.0.2.50'   # only the LAN phone
asterisk -rx 'pjsip cisco restart 1020 1b2c3d4e5f'   # or by @hash
asterisk -rx 'pjsip cisco restart 1020'              # both/all contacts
```

Tab-completion works on both arguments.

Feature state mirrors the `sip donotdisturb / huntgroup / callforward`
family from the `chan_sip` cisco-usecallmanager patch. Each command
updates astdb and pushes a bulkupdate REFER so the phone redraws its
DND glyph, HLog softkey or CFwdALL banner:

```sh
asterisk -rx 'pjsip cisco donotdisturb on  1010'
asterisk -rx 'pjsip cisco huntgroup    off 1010'
asterisk -rx 'pjsip cisco callforward  on  1010 2000'
```

Force-push current state without changing it. This is useful after
dialplan, AMI or external tooling has touched astdb directly:

```sh
asterisk -rx 'pjsip cisco bulkupdate 1010'
```

## Dialplan Integration

The same verbs are exposed as dialplan functions, registered by
`res_pjsip_cisco_bulkupdate`:

```
exten => *78,1,Set(CISCO_DND(${CALLERID(num)})=YES)
 same =>     ,n,Playback(do-not-disturb&activated)
exten => *79,1,Set(CISCO_DND(${CALLERID(num)})=NO)
exten => *7,1,Set(CISCO_HUNTGROUP(${CALLERID(num)})=YES)
```

`CISCO_DND`, `CISCO_HUNTGROUP` and `CISCO_CALLFORWARD` write the astdb
key, push a bulkupdate REFER, and (for DND) fire the presence change so
BLF lamps update. A direct `Set(DB(DND/${EXTEN})=YES)` still updates the
key but skips the push and the lamp redraw.

Worked feature codes for group pickup (`*8`), ad-hoc meet-me conferences
(8001-8009), a CallManager-style chained 3-way (`*0<ext>`) and a
print-and-stick user reference card live in
[`conf-samples/extensions.conf.cisco-features.sample`](conf-samples/extensions.conf.cisco-features.sample).

## Further Reading

- [ARCHITECTURE.md](ARCHITECTURE.md): per-module rationale, the four BLF
  signals, body-shape mapping back to the chan_sip patch.
- [tests/README.md](tests/README.md): bench-test recipe (parallel TCP on
  `:5160`, swap a single phone over via its SEP file, capture and verify
  the wire signals).
- [`conf-samples/`](conf-samples/): annotated `pjsip.conf` and
  `extensions.conf` snippets.

## License

GPL-2.0 - matches Asterisk's licence.

## Acknowledgements

This project builds on Gareth Palmer's work. His
[`cisco-usecallmanager`](https://usecallmanager.nz) patch for `chan_sip`
has been maintained for years and is the basis for this implementation.
Cisco Enterprise SIP firmware is not documented publicly; the
optionsind, bulkupdate, RemoteCC and Cisco PIDF body shapes used here all
come from his careful work understanding what real phones expect on the
wire. The XML, REGISTER flow and post-REGISTER REFER mechanics in this
project map back to his patched `chan_sip` implementation. This
repository's contribution is the port to `chan_pjsip` as out-of-tree
modules, so deployments that have moved off `chan_sip` do not lose Cisco
phone support.
