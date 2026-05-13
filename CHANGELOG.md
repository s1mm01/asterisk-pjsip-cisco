# Changelog

## v0.1.0 — 2026-05-13

First tagged release — an early 0.x cut: works on the maintainer's
bench but wants real-world testing across more phone models and Asterisk
versions before it earns a 1.0. Ten shared modules adding Cisco
Enterprise SIP firmware support to chan_pjsip (built and CI'd against
Asterisk 20.x / 22.x / 23.x; bench-tested on 22.9.x):

- `res_pjsip_cisco_endpoint` — `cisco` sorcery type + per-endpoint
  gating (`line_index`, `subscribe`, `subscribe_context`, `aliases`,
  `parkext`).
- `res_pjsip_cisco_pidf_body_generator` — Cisco-flavoured PIDF +
  RPID + Cisco-private rpid namespace. Supplements stock `pidf+xml`
  output and wins the `cpim-pidf+xml` generator slot before stock
  xpidf.
- `res_pjsip_cisco_register_optionsind` — REGISTER 200 OK gets
  `application/x-cisco-remotecc-response+xml` optionsind body.
- `res_pjsip_cisco_bulkupdate` — post-REGISTER unsolicited REFER with
  multipart `application/x-cisco-remotecc-request+xml` carrying
  `<dndupdate>` + `<hlogupdate>` + `<bulkupdate><contact line="N">…`.
- `res_pjsip_cisco_unsolicited_blf` — post-REGISTER unsolicited
  Event: presence NOTIFY per watched extension; the trigger that
  flips Cisco firmware's line buttons into BLF Speed Dial mode.
- `res_pjsip_cisco_service_control` — CLI: `pjsip cisco
  {check-sync,restart,reset} <endpoint> [contact]` (optional
  `[contact]` — a URI substring or `@hash` — targets one phone of a
  shared line; both args tab-complete). The restart/reset body carries
  the phone's REGISTER `Call-ID`, sourced per contact.
- `res_pjsip_cisco_feature_events` — handles Cisco DND/CFwdALL
  softkey SUBSCRIBEs and writes state to astdb.
- `res_pjsip_cisco_call_extras` — adds Cisco call signaling extras:
  `Call-Info` RemoteCC metadata, `Supported: X-cisco-sis-10.0.0`,
  callback number in Remote-Party-ID, and H.264 SDP hints.
- `res_pjsip_cisco_remotecc` — claims Cisco RemoteCC REFERs and
  implements HLog, MCID, and the Park / ParkMonitor softkeys (parks
  the call by blind-transferring the peer to `res_parking`'s parkext —
  default `700`, overridable via `parkext=` on the `type=cisco`
  object — off the SIP rx thread on its own serializer, then pushes
  the slot back to the phone as a status toast / orbit-BLF NOTIFY).
  Requires `res_parking` configured (`res_parking.conf`).
- `res_pjsip_cisco_conference` — Phase 1 conference control: the
  ConfList softkey returns a read-only `<CiscoIPPhoneMenu>` listing
  the bridge participants for the active call leg.

Plus a dialplan sample
(`conf-samples/extensions.conf.cisco-features.sample`) wiring up
`res_parking` for the Park softkey + the dialplan-only hardware-button
replacements: group pickup (`*8`), ad-hoc ConfBridge meet-me, and a
CallManager-style chained-transfer conference (`*0<ext>`). The sample
includes a print-and-stick reference card for end users.

Verified against a Cisco 7975 running Enterprise SIP firmware 9.4.2
and Asterisk 22.9.0.

### Known issues

- Automatic pushes for DND/CF state changes made outside the phone
  softkeys are not implemented. Use `pjsip cisco bulkupdate
  <endpoint>` after external astdb writes, or let the next REGISTER
  refresh phone-side state.

### Fixes during pre-release

- **Service-control `RegisterCallId` is now per-contact**. The
  restart/reset NOTIFY body must carry the Call-ID of the *target
  phone's* REGISTER transaction (the firmware 400s a mismatch). It was
  captured into astdb keyed by endpoint id, so on a shared line
  (`max_contacts > 1`) whichever phone registered last owned the slot
  and every other phone got someone else's Call-ID and ignored the
  restart. Now read straight from `contact->call_id` (the registrar
  records it per contact); the REGISTER-capture supplement and the
  `RegisterCallId` astdb family are gone.

- **`LOCAL_DOMAIN` hardcode removed**. Cisco-generated URI fragments now
  reuse the From URI host/port that Asterisk/PJSIP built on the
  outgoing `pjsip_tx_data`; Cisco-specific code mutates only the user
  part where firmware requires it. `cisco_endpoint_local_domain()`
  still has endpoint-transport and `localhost` fallbacks for paths that
  do not have a tx_data yet.

- **PIDF coexistence rework**. The `noload =>
  res_pjsip_pidf_body_generator.so` /
  `res_pjsip_xpidf_body_generator.so` directives are no longer
  required:
  - For `application/pidf+xml` we now register a body **supplement**
    (`ast_sip_pubsub_register_body_supplement`) that decorates stock
    pidf's `pjpidf_pres` tree with `<dm:person>/<e:activities>` and
    Cisco rpid extensions, instead of replacing the generator. Stock
    pidf and our module run side-by-side.
  - For `application/cpim-pidf+xml` we still register a generator
    (Cisco firmware Accepts only this subtype, and stock xpidf would
    emit the older RFC 2779 format Cisco doesn't render as BLF), but
    with `load_pri = AST_MODPRI_CHANNEL_DEPEND - 5` so we register
    before stock xpidf. Stock xpidf logs one WARNING at startup
    ("already registered") and declines. Operator config
    requirement: zero.

- **Makefile auto-derives `ASTERISK_INCLUDE_DIR` from `PJPROJECT_DIR`**.
  Previously, building with `make PJPROJECT_DIR=/path/to/asterisk-src`
  used `/usr/include/asterisk/*.h` for asterisk headers - which is
  `asterisk-dev` package content if installed, or stale leftovers from
  an older `make install` if not. On boxes that self-build asterisk
  this routinely diverges from the running binary's struct layouts and
  causes runtime SEGV when our modules read `struct ast_sip_endpoint`
  fields past the first few. Now `PJPROJECT_DIR=...` implicitly sets
  `ASTERISK_INCLUDE_DIR=$PJPROJECT_DIR/include` (when that file
  exists), keeping struct layouts in lockstep with the running binary.
