# Changelog

## v0.5.1 — 2026-05-18

Follow-ups to v0.5.0. Three focused improvements; no API or
sorcery-schema change.

### `pjsip cisco status` — per-contact device attribution

Device facts (MAC, source host, device name, firmware loads)
were previously rendered as a single block keyed by endpoint
name. For an endpoint with multiple registered devices —
e.g. a 6003 with both a CP-8861 (LAN) and a CP-8865 (WAN)
registered concurrently — only one device's facts displayed,
conflating two physical phones under one block.

Each contact corresponds to a specific REGISTER, identified by
its Call-ID. `cisco_mac_info` now carries the Call-ID from the
producing REGISTER, and the status renderer nests a
`Cisco device:` subsection inside each `Contact #N` block,
looked up by that contact's Call-ID. The previous standalone
post-loop block is removed.

### Body-shape regression coverage — `test_blf_pidf`

The unsolicited-NOTIFY PIDF body builder
(`cisco_blf_build_pidf`, extracted to
`res/cisco_unsolicited_blf/pidf.c`) now has a golden-output unit
test. 9 cases cover every documented exten-state /
presence-state combination plus XML-special escaping in
exten + domain. Any drift in the assembled body — activity tag
order, attribute reorder, namespace edit, missing escape —
breaks the test before the build it on a real Cisco phone.

The test harness ships a minimal Asterisk-API shim under
`tests/unit/include/asterisk/` (`ast_str_*`,
`ast_sip_sanitize_xml`, `AST_EXTENSION_*`, `AST_PRESENCE_*`)
with implementations in `tests/unit/asterisk_stubs.c`. Future
body-builder helpers can follow the same pattern without
needing a real Asterisk runtime; `tests/unit/README.md`
documents the template.

### CISCO_* dialplan-function recipes

`conf-samples/extensions.conf.cisco-features.sample` grows
52 → 195 lines, adding four worked recipes for the `CISCO_DND` /
`CISCO_HUNTGROUP` / `CISCO_CALLFORWARD` dialplan functions:

- **Capture CFwdAll softkey input** — REQUIRED if you want the
  CFwdAll softkey to do anything server-side. The phone INVITEs
  a service-URI when the user enters / cancels the forward
  target; dialplan has to catch it and write
  `CISCO_CALLFORWARD`. DND and HuntGroup are auto-captured by
  the modules; CFwdAll is the odd one out — now flagged
  explicitly in the sample intro.
- **DND-aware Dial** — skip the Dial leg entirely when the
  target is on DND, branch to a different voicemail greeting.
- **CFwdAll-honoring Dial** — REQUIRED if you want a saved CF
  target to actually redirect calls.
- **Hunt-group bulk toggle** — close-of-business cron logging
  everyone out via channel originate into an admin extension.

## v0.5.0 — 2026-05-18

Diagnostics + tree refactor release. New `pjsip cisco status
<endpoint>` CLI surfaces the runtime picture for a Cisco endpoint
in one place — sorcery config, registration state, MWI counts, the
REGISTER Call-ID, the harvested MAC, and the device's self-reported
firmware versions. Plus a tree reorganisation to Asterisk's house
style: per-module subdirectories under `res/`, public headers under
`include/cisco/`, build outputs under `obj/`. No ABI or sorcery-
schema change — v0.5.0 installs as a drop-in replacement for v0.4.2.

### New behaviour

- **`pjsip cisco status <endpoint>` CLI**
  (`res_pjsip_cisco_service_control`). One-shot dump of everything
  the module set knows about a Cisco endpoint:
  - `type=cisco` sorcery fields (model, blf entries, …).
  - AOR / contact state + REGISTER expiry.
  - MWI new/old counts, delegated to the same compute path the
    unsolicited-NOTIFY flow uses (`cisco_endpoint_mwi_count`,
    lifted into the shared endpoint helpers).
  - REGISTER Call-ID, harvested by
    `res_pjsip_cisco_register_optionsind` at REGISTER time.
  - Device MAC address + firmware active / inactive load, parsed
    from the REGISTER `Reason:` header.
  - Aliases (multi-line phones REGISTERing the same MAC under
    several endpoint names).

- **REGISTER Reason-header firmware / device harvest**
  (`res_pjsip_cisco_feature_events`). Cisco firmware tags REGISTER
  with a `Reason:` header carrying
  `DeviceName=...; ActiveLoadId=...; InactiveLoadId=...`. Parsed
  and stored on the `cisco_mac_info` entry alongside the existing
  MAC-from-From-URI extraction. Surfaced via the status CLI today;
  opens the door to fleet-firmware reporting later.

- **Alias-aware MAC lookup, with expired-entry skip**
  (`res/cisco_endpoint/device.c`). Multi-line phones REGISTER once
  per line, so the MAC→endpoint map sees several entries per
  device. Status lookup now walks aliases when the queried name
  is the alias rather than the primary, and skips entries whose
  REGISTER has expired.

### Internals

- **Tree refactor to Asterisk house style.**
  `res/res_pjsip_cisco_<feature>.c` keeps the `AST_MODULE_INFO`
  entry point only; everything else moves to a sibling
  subdirectory (`res/cisco_<feature>/*.c`) with private headers
  under `res/cisco_<feature>/include/`. Public / shared headers
  live under `include/cisco/`. Build outputs (modules + generated
  doc XML) live under `obj/`. Output paths are parameterised via
  the `OBJ_DIR`, `MODULE_BUILD_DIR`, and `DOC_XML` make variables
  (documented in the top-level Makefile).

- **`res_pjsip_cisco_remotecc` logging split**. Unsolicited-alarm
  processing and remotecc-response processing previously shared a
  log path; split so a noisy alarm phone doesn't drown out
  response-flow diagnostics.

### Tests + CI

- **`test_xml_bodies` unit test** (libxml2-linked, runs under
  `make tests`). Snprintfs each wire-format XML body template
  (DND part, HLog part, bulkupdate body, MCID feedback, Park
  toast / orbit, HLog update) with bench-realistic substitutions,
  then parses the result with libxml2. Catches sprintf typos
  (missing close tag, mismatched attribute quote) before they
  hit a real Cisco phone. Cisco-private `\200` glyph bytes
  embedded in MCID-status / Park-toast bodies are handled via
  libxml2's recovery mode.

- **CI module-load smoke**. The matrix runner installs apt's
  stock Asterisk, builds the modules against it, then loads all
  ten `.so` files into the running daemon and checks
  `module show like cisco_`. Gated on the apt major matching the
  build matrix cell. Today only the cell-20 build (noble carries
  asterisk-20.6) exercises the load path; cells 22 and 23 skip
  because no current Ubuntu release packages asterisk 22 or 23 in
  any pocket. The skip is logged, not silent; the build-time ABI
  check (compile against the resolved upstream tag) still applies
  to every cell, so a header-level regression still fails the
  build everywhere.

## v0.4.2 — 2026-05-16

Bug fix follow-up to v0.4.0's phantom-video-suppression work.
v0.4.0 covered the case where a video phone dials a voice-only
SIP endpoint (`6012 -> 6003 with no h264`); it missed the
adjacent case where Asterisk itself terminates the call —
MusicOnHold, VoiceMail, IVR / SayUnixTime applications,
ConfBridge meet-mes, anything that runs in dialplan without a
second SIP leg. Symptom: black video pane on the 8865 for the
duration of the playback, because the linkedid walk found zero
SIP siblings, `cisco_h264_peer_had_video` returned `-1`
("unknown — leave alone"), and the phantom `m=video <port> sendonly`
shipped to the caller.

Distinguish "no SIP peer at all" from "SIP peer exists but no
SDP yet". The per-candidate accumulator now counts only
candidates whose channel tech is `"PJSIP"` — Local/parking/
dialplan-app helpers share linkedid but contribute no SDP and
thus no video sink. Three outcomes:

- **`pjsip_siblings == 0`** → suppress. Asterisk is running a
  dialplan app on this chan_pjsip channel itself; no video sink
  exists.
- **`pjsip_siblings > 0`, none observed yet** → leave alone
  (pre-setup window; suppressing would race the late observation).
- **At least one observed** → use the aggregated `any_had_video`
  flag (unchanged from v0.4.0).

Bench-verified: video phone (CP-8865 at 6003) dialed `6666`
(MusicOnHold) and `1234` (Answer + SayUnixTime). Both 200 OK
responses back to 6003 carry `m=video 0`; the video pane closes
immediately instead of hanging. The previously-tested video ↔
voice-only path still suppresses correctly because the dial-leg
sibling is counted (`pjsip_siblings == 1`) and its observed
`had_video_media == 0`.

## v0.4.1 — 2026-05-15

Docs-only patch. The shipped
`conf-samples/extensions.conf.cisco-features.sample` was carrying
~140 lines of obsolete workarounds and generic-Asterisk dialplan
that pre-dated v0.3.0's native Confrn / Join / RmLastConf / action-
softkey support. Trimmed to just the Park integration with
`res_parking`, which is the one operator-config step the project
actually needs (the RemoteCC Park softkey blind-transfers to
`<parkext>@<context>`, and that needs to resolve to `Park()`).

Removed:

- Ad-hoc `ConfBridge` meet-me (8001-8009) and chained-transfer
  `*0NNNN` conference workarounds — superseded by the native
  Confrn softkey.
- `*8` group pickup and `*97` voicemail — generic Asterisk
  features that don't belong in a Cisco-specific sample.
- Stale "Phase 1: read-only — Mute/Remove softkeys are not yet
  implemented" comment — implemented in v0.3.0.

169 → 52 lines. No code or runtime change.

## v0.4.0 — 2026-05-15

Bench-debugged release: every behavioural change here was prompted
by a real failure observed on the live fleet (8865 / 8861 / 7975 /
2N IP Verso door) and verified back to working on the same wires.
Two headline video fixes (phantom-stream suppression on
video-to-voice-only calls; correct softmix routing in 3+ party
conferences with video), plus the H.264 imageattr propagation path
Gareth's chan_sip patch carried in core. Internals: the conference
module split into focused files, two rounds of compiler hardening
that the whole codebase now passes clean, and a `make tests`
harness with smoke checks wired into CI.

### New behaviour

- **H.264 imageattr propagation across the bridge**
  (`res_pjsip_cisco_call_extras`). When a Cisco endpoint's incoming
  SDP carries `a=imageattr` on H.264 payloads, it's stashed on the
  receiving leg's `ast_channel` via an `ast_datastore`. When the
  outgoing answer is built on the bridge peer leg, that value is
  mirrored (rewritten with the payload number of our own H.264
  format) instead of the static `[x=640,y=480,q=0.50]` fallback.
  SDP-level equivalent of Gareth's core-format-attribute widening;
  replacing the stock `res_format_attr_h264` interface from
  out-of-tree isn't viable
  (`__ast_format_interface_register` refuses a second registration
  for `"h264"`).

  Lookup uses a two-phase peer find: `ast_channel_bridge_peer()`
  when the bridge is formed (precise, handles 3+ party correctly
  by returning NULL), with a linkedid-walk + per-candidate
  aggregation fallback for the pre-bridge window — chan_pjsip
  emits the answering 200 OK during `ast_answer()`, which
  `app_dial` calls *before* `ast_bridge_call()`, so the bridge
  doesn't exist yet at SDP-patch time. Bench-verified after that
  race was caught in trace.

- **Phantom video answer suppression on voice-only callees**
  (`res_pjsip_cisco_call_extras`). When an 8865 (or any video
  phone) dials a voice-only endpoint, chan_pjsip's default
  `codec_prefs_*_answer = keep:all` kept the offered video media
  line in the answer back to the caller. The 8865 opened its
  video-receive pane on the advertised port, never saw frames,
  and painted a black box for the duration of the call. Now
  rejected with `port=0` (RFC 3264 §6) when the bridge peer leg
  observably had no video. The lookup aggregates across every
  linkedid-matching candidate so helper channels (`Local;1`,
  parking) with no datastore are conservatively skipped, and
  multi-party "any peer has video" is handled correctly without
  misfiring against an audio-only trunk participant.

- **Conference bridge video routing mode**
  (`res_pjsip_cisco_conference`). Asterisk's softmix bridge
  defaults to `AST_BRIDGE_VIDEO_MODE_NONE`, which drops every
  video RTP frame between participants. Our Confrn/Join created
  the multimix bridge with the SMART flag (auto-promotes 2-party
  native → 3-party softmix the moment a third leg joins) but
  never called `ast_bridge_set_*_video_mode()`. Symptom observed
  live: door-camera video on an 8865 went black the instant a
  third (audio-only mobile) leg was Confrn'd into the call. Now
  auto-picks per actual participant capability:

  - Exactly one video-capable participant → `SINGLE_SRC` pinned
    to it. Right for the door-camera-into-audio-conference shape:
    forward the camera feed regardless of who's talking.
  - Two or more video-capable → `TALKER_SRC`. Standard multi-
    party video-conference behaviour. The door-intercom case
    falls into this branch (the 8865's own native formats include
    h264 too) and still works correctly because the door has a
    microphone — whenever audio is active at the door the bridge
    selects the door's video; sustained selection means the camera
    stays visible even when the user talks.
  - Zero video-capable → `TALKER_SRC` (harmless; keeps the bridge
    in a defined state for late-joining video peers).

### Internals

- **`res_pjsip_cisco_conference.c` split into focused files**.
  The single 2570-line .c was past the comfortable-navigation
  threshold. Split along the section seams the existing comments
  already pointed out:
  - `res_pjsip_cisco_conference.c` — module entry, REFER body
    parsing, softkey dispatch, load/unload (~570 lines).
  - `cisco_conf_state.c` — pending-action + selected-call ao2
    containers and the channel-state helpers shared across all
    action paths (~390 lines).
  - `cisco_conf_list.c` — ConfList menu build + send, plus the
    Mute/Remove/Update action two-step state machine (~530
    lines).
  - `cisco_conf_confrn.c` — Confrn 3-way build with post-merge
    wire dance, Join multi-call merge, RmLastConf (~1010 lines).

  Cross-file declarations in `cisco_conference.h` (internal — not
  exported across the .so boundary). Strict refactor, no
  behaviour change.

- **Two rounds of compiler hardening**. On top of the existing
  `-Wall -Werror -Wno-unused-function`, the Makefile now passes:
  `-Wstrict-prototypes`, `-Wmissing-prototypes`,
  `-Wmissing-declarations`, `-Wold-style-definition`,
  `-Wnested-externs`, `-Wshadow`, `-Wpointer-arith`,
  `-Wjump-misses-init`, `-Wlogical-op`, `-Wduplicated-cond`,
  `-Wduplicated-branches`, `-Wvla`, `-Wformat=2`. Builds clean
  across the 20.x / 22.x / 23.x matrix without any code changes
  required — the codebase was already disciplined enough to pass
  all of them. `-Wbad-function-cast` deliberately omitted: it
  fires inside Asterisk's own `include/asterisk/time.h` (cast of
  a double-returning function to long int), which is out of our
  hands.

- **`debian/control` refreshed for v0.3.0+** — description now
  covers Conference Phase 1, Record softkey, prt-report,
  presence-driven BLF, and the global `on_tx_request` NAT hook.
  CI-only Build-Depends (`wget`, `ca-certificates`) dropped from
  the real package metadata.

- **`AGENTS.md` repo-structure paragraph** updated to describe
  the six topical shared-helper headers in `res/` (split from
  the single header in v0.3.0).

### Tooling

- **`make tests` target** with two flavours:
  - `smoke` — `xmllint` on the generated `doc/res_pjsip_cisco-en_US.xml`
    plus `readelf`-based `__mod_info` / `load_module` /
    `unload_module` symbol checks for each `.so`. Catches
    "module silently refuses to register at startup" failure
    modes that previous build-only CI couldn't see.
  - `unit` — pjlib-linked standalone C programs with `assert()`s.
    Pins behaviour of pjlib primitives (`pj_stricmp2`, `pj_strchr`,
    `pj_str`) we rely on across pjproject versions. Ships
    asterisk-stub allocator wrappers (`__ast_repl_malloc`,
    `__ast_free`) so the test binary links without libasterisk.
  - Documents the NDEBUG-from-pjproject gotcha (`pjproject/config_site.h`
    defines `NDEBUG`, silently turning `assert()` into `((void)0)`
    unless test files `#undef NDEBUG` before including
    `<assert.h>`) and the `-O0` requirement so assertions actually
    evaluate at runtime instead of constant-folding away.

- **CI runs `smoke.sh`** after the build matrix produces .so files
  and the doc XML. Unit half deliberately not in CI — those tests
  link against pjproject static libs which CI doesn't build (it
  only `./configure`s pjproject for headers, never `make`s).

- **CI compatibility fix**: the new `-Wstrict-prototypes` flag
  fires on stock pjproject's `pj_thread_unregister()` K&R-style
  declaration (`pjlib/include/pj/os.h:267`). Asterisk's own build
  applies `0010-pj_thread_unregister_void.patch` from
  `third-party/pjproject/patches/`, but 20.x doesn't carry it,
  and CI resolves the asterisk tag at runtime. CI now sed-fixes
  the one-line declaration directly — idempotent and version-
  independent across pjproject 2.14 (asterisk 20.x) and 2.16
  (asterisk 22.x/23.x). Local builds were always fine because
  bundled pjproject ships the patch pre-applied.

## v0.3.0 — 2026-05-15

Headline additions are the Conference feature (Phase 1: Confrn-built
multimix bridge with phone-side UI promotion, ConfList read + action
softkeys, multi-call merge via Join / Select / Unselect, RmLastConf),
DND / CFwdALL / HuntGroup CLIs + dialplan functions wired to BLF
lamps, the Record softkey, and a service-control PRT report CLI.
Internals: helper bodies now live in `res_pjsip_cisco_endpoint.so` so
every other module shares one canonical implementation, and the
NAT'd-contact RURI rewrite moved out of `res_pjsip_cisco_unsolicited_blf`
into a global `on_tx_request` hook covering every outbound request.
Bench-tested on Asterisk 20.19, 22.9, and 23.3.

### New features

- **Conference softkey** (`res_pjsip_cisco_conference`). Phase 1
  end-to-end: the in-call Conference softkey builds a real multimix
  bridge and joins the held leg, with the post-Confrn wire dance —
  `holdretrievereq` + in-dialog Cisco-flavoured NOTIFY carrying an
  RFC 3515 sipfrag body + Event:refer NOTIFY — so the phone-side UI
  promotes to a Conference glyph and un-holds without the user
  touching anything. The Join / Select / Unselect softkeys merge
  multiple held calls into one bridge. RmLastConf removes the
  most-recently-joined participant (tracked per channel in
  `cisco_session`). The ConfList softkey returns the participant
  menu; its Mute / Remove / Update action softkeys are now wired
  through to the bridge. `cisco_keep_conference` controls whether
  the bridge survives initiator hangup.
- **DND / CFwdALL / HuntGroup CLIs and dialplan functions**
  (`res_pjsip_cisco_feature_events` + shared helpers). New CLIs
  `pjsip cisco {dnd,huntgroup,cfwdall} <endpoint> [on|off|number]`
  and dialplan functions `CISCO_DND()`, `CISCO_HUNTGROUP()`,
  `CISCO_CFWDALL()` (read + write) drive the same astdb state the
  softkey SUBSCRIBEs maintain; a write also pushes a bulkupdate
  REFER so the phone-side toggle/icon updates in real time. DND
  state additionally drives BLF lamps via a Cisco presence-state
  provider — hints can watch a DND'd line and the lamp goes red.
- **Record softkey** (`StartRecording` / `StopRecording`) in
  `res_pjsip_cisco_remotecc`. Resolves the softkey REFER's
  `<dialogid>` to the phone's channel and runs `MixMonitor` (or
  `StopMixMonitor`) on it off the SIP rx thread, defaulting to
  filename `cisco-<endpoint>-<uniqueid>.wav` under the configured
  MixMonitor directory. Override per call by setting
  `CISCO_RECORD_FILENAME` on the channel from dialplan. chan_sip's
  `cisco-usecallmanager` patch approaches the same softkey by
  creating a second SIP dialog and dispatching to extension
  `record`; in chan_pjsip a direct MixMonitor on the bridged
  channel gives the same result.
- **`pjsip cisco prt-report <endpoint> [contact]` CLI**
  (`res_pjsip_cisco_service_control`). Triggers the phone's
  Problem Report Tool upload via the same service-control NOTIFY
  channel as `check-sync` / `restart` / `reset`.

### Internals

- **Shared helpers compiled into `res_pjsip_cisco_endpoint.so`**.
  `cisco_endpoint.h` was split into six topical headers
  (`cisco_endpoint.h`, `cisco_rdata.h`, `cisco_register.h`,
  `cisco_refer.h`, `cisco_session.h`, `cisco_orig_host.h`) carrying
  declarations only; bodies live in sibling `.c` files that compile
  into the endpoint `.so` and are exported globally so every other
  module resolves them via the loader's `RTLD_GLOBAL` pass on
  `AST_MODFLAG_GLOBAL_SYMBOLS`. Same pattern stock Asterisk uses
  for `ast_sip_*` in `res_pjsip.so`. Before this, helpers were
  inline in headers and each consumer compiled its own copy.
- **NAT'd-contact RURI/To rewrite moved to a global `on_tx_request`
  hook** (`cisco_orig_host` inside `res_pjsip_cisco_endpoint.so`).
  When `res_pjsip_nat`'s `rewrite_contact=yes` leaves an
  `x-ast-orig-host` URI parameter on the Contact-derived RURI, the
  hook rewrites the Request-URI and To-URI back to the phone's
  self-advertised host:port. Cisco firmware on NAT'd phones rejected
  unsolicited NOTIFYs / REFERs with `400 Bad Request` because the
  public NAT mapping in the RURI didn't match what the phone knew
  about itself. The hook is a strict no-op for any URI without the
  parameter — LAN-registered contacts, upstream trunks, non-NAT'd
  targets all pass through unchanged. The earlier per-module
  rewrite that lived in `res_pjsip_cisco_unsolicited_blf` (and
  never covered REFER) is gone.
- **`res_pjsip_cisco_feature_events` PATH A removed**. The
  `as-feature-event` SUBSCRIBE handler turned out to be dead in
  chan_pjsip — firmware uses RemoteCC REFERs instead. The only
  remaining path is PATH B (astdb-backed feature state).
- **`Makefile` propagates `SELF_SYM` into helper objects** so the
  helper `.c` files compiled into `res_pjsip_cisco_endpoint.so` link
  against the parent module's self-symbol instead of an undefined
  one.

### Docs

- README rewritten as a user-facing feature tour with a
  compatibility table covering Asterisk 20.x → 23.x and bench-tested
  status per release.
- ARCHITECTURE.md tracks the landed Conference + RemoteCC work, the
  shared-helpers `.so` pattern, and `cisco_orig_host`'s role.

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
