# Architecture

How and why these ten modules add up to working BLF for Cisco
Enterprise SIP firmware.

## The four-behaviour insight

Cisco firmware classifies a line button as **BLF Speed Dial** (hook
icon, lit/unlit LED, reacts to monitored extension state) versus
**plain Speed Dial** (keypad icon, no state) based on a combination
of **four independent server signals at REGISTER time and beyond**:

1. The phone's SEP `<featureID>21</featureID>` per button. (Existing
   provisioning — not our concern.)
2. The REGISTER 200 OK response carrying a Cisco RemoteCC
   **optionsind** body that explicitly advertises support:
   ```xml
   <presence usage="blf speed dial"><unot/><sub/></presence>
   ```
3. An unsolicited REFER carrying a multipart **bulkupdate** body
   with per-line config (`<contact line="N">…`).
4. **Unsolicited** Event: presence NOTIFYs from the server for each
   monitored extension. These are distinct from NOTIFYs the server
   sends in response to the phone's own SUBSCRIBE.

The non-obvious part is #4: Cisco firmware specifically distinguishes
between "NOTIFY in response to my SUBSCRIBE" (treated as background
BLF info, button stays plain Speed Dial) and "unsolicited NOTIFY from
server" (treated as server-managed BLF, button flips to BLF Speed Dial
mode). Same body content; different trigger.

If any of (2), (3), or (4) is missing, the firmware silently
downgrades the button display to keypad icon and ignores subsequent
state updates, even though it does send and accept SUBSCRIBE/NOTIFY
on the wire.

## Module map

```
                          REGISTER from phone
                                  |
                                  v
               +--------------------------+
               |  res_pjsip_cisco_endpoint  |    sorcery type 'cisco'
               +--------------------------+    keyed by endpoint name
                       |              |
   has [name] type=    | yes          | no -> standard PJSIP, no Cisco logic
   cisco section?      v
                  +-----------+
                  | gating ok |
                  +-----------+
                  /     |      \
                 /      |       \
                v       v        v
   +----------------+  +-----------+  +-----------------+
   | optionsind     |  | bulkupdate |  | unsolicited_blf |
   | -> 200 OK body |  | -> REFER   |  | -> NOTIFY/ext   |
   +----------------+  +-----------+  +-----------------+

                          phone SUBSCRIBE for line buttons
                                       |
                                       v
                    +-----------------------------------+
                    | res_pjsip_cisco_pidf_body_generator |
                    | -> NOTIFY body (Cisco PIDF)         |
                    +-----------------------------------+

   plus orthogonally, on operator/user command:
	                    +-------------------------------+
	                    | res_pjsip_cisco_service_control |
	                    | -> NOTIFY (check-sync/restart)  |
	                    +-------------------------------+

	                    +-------------------------------+
	                    | res_pjsip_cisco_feature_events |
	                    | -> DND/CF astdb updates        |
	                    +-------------------------------+

	                    +-------------------------------+
	                    | res_pjsip_cisco_call_extras   |
	                    | -> Call-Info/RPID/SDP quirks  |
	                    +-------------------------------+

	                    +-------------------------------+
	                    | res_pjsip_cisco_remotecc      |
	                    | -> RemoteCC softkeys          |
	                    +-------------------------------+
```

## Module-by-module rationale

### res_pjsip_cisco_endpoint

Adds a parallel `cisco` sorcery type keyed by endpoint name, taking
advantage of PJSIP's same-name-different-type sections:

```
[1010]
type    = endpoint
...

[1010]
type    = cisco
...
```

This avoids needing to extend `struct ast_sip_endpoint` (which would
require patching Asterisk core). Existence of a `[name] type=cisco`
section is the gating signal for every other module.

`cisco_endpoint_get()` and the other shared `cisco_*` helpers are
declared in `include/cisco/endpoint.h` / `rdata.h` / `register.h` /
`refer.h` / `session.h` / `orig_host.h` and defined in the matching
`.c` files under `res/cisco_endpoint/`. All six `.c` files are compiled
into `res_pjsip_cisco_endpoint.so`, which carries `AST_MODFLAG_GLOBAL_SYMBOLS`
so its `cisco_*` exports (controlled by `res/res_pjsip_cisco_endpoint.exports`)
are visible to every other module at load time. Same pattern stock
`res_pjsip.so` uses to publish `ast_sip_*` to every PJSIP submodule.

This module also registers the global `PJSIP` presence-state
provider — the one a BLF hint reaches as `PJSIP:<endpoint>` in its
presence component (second comma-separated half), e.g.
`exten => 1010,hint,PJSIP/1010,PJSIP:1010`. The callback reads
`DND/<endpoint>` from astdb and returns `AST_PRESENCE_DND` /
`NOT_SET`; `cisco_dnd_set()` fires `ast_presence_state_changed` so
toggles propagate to live watchers. Mirrors the chan_sip
cisco-usecallmanager patch's `sip_presencestate`, but reachable from
an out-of-tree module: the colon form (not `PJSIP/1010`) is required
because core chan_pjsip's `ast_channel_tech` has no `.presencestate`
callback and this project doesn't patch core. Non-Cisco endpoints
report `NOT_SET` so the provider is inert for non-Cisco peers.

A third registration: `cisco_orig_host_register()` (from
`res/cisco_endpoint/orig_host.c`) installs a global `pjsip_module` whose
`on_tx_request` callback rewrites Request-URI and To-URI host:port
back to the phone's self-advertised contact host whenever
`res_pjsip_nat` has left an `x-ast-orig-host` URI parameter on the
Contact-derived RURI (i.e. for a NAT'd registered contact). Without
this rewrite, Cisco Enterprise firmware (verified against
CP8861/14.1.1 and CP7975G/9.4.2) rejects unsolicited NOTIFYs and
unsolicited REFERs to its WAN-side contact with `400 Bad Request` —
the phone's own alarm payload echoes the rejected RURI verbatim, so
the firmware is matching on it. The hook fires after PJSIP has
selected the transport but before serialisation, so changing the
URIs only affects the wire bytes, not where they're routed (the
public TCP connection stays in use). Strict no-op when the URI has
no `x-ast-orig-host` parameter, so LAN-registered contacts and
trunk-bound traffic pass through untouched and no consumer module
opts in — sending an out-of-dialog request to a NAT'd Cisco-phone
contact is enough.

This means `res_pjsip_cisco_unsolicited_blf`'s presence NOTIFYs and
`res_pjsip_cisco_bulkupdate`'s REFERs (and any future Cisco-* module
that targets a NAT'd contact) all inherit the rewrite by simply
sending requests. The rewrite is wired structurally rather than
plumbed per-module.

### res_pjsip_cisco_pidf_body_generator

Two registrations under one `.so`:

- A **body supplement** for `application/pidf+xml` that augments
  stock `res_pjsip_pidf_body_generator`'s output. Stock builds the
  basic `<presence>/<note>/<tuple>/<status>` body; we add
  `<dm:person>/<e:activities>` with the RPID and Cisco-rpid
  elements. Pubsub runs supplements right after the generator (and
  before serialisation), so they share the same `pjpidf_pres` tree.
  Stock pidf and us coexist — no `noload`, no slot conflict.

- A **body generator** for `application/cpim-pidf+xml` (plus a
  matching supplement). Cisco firmware advertises only
  `cpim-pidf+xml` in its SUBSCRIBE Accept header; stock
  `res_pjsip_xpidf_body_generator` would respond with the older
  RFC 2779 XPIDF format which doesn't carry RPID. We hijack the
  subtype label and emit the same modern PIDF body (built via
  pjpidf the same way stock pidf does). Pubsub allows only one
  generator per content type, so we set `load_pri` lower than
  `AST_MODPRI_CHANNEL_DEPEND` to register first; stock xpidf logs
  one WARNING at startup ("already registered") and declines.

Net body shape on the wire:

```xml
<presence xmlns="urn:ietf:params:xml:ns:pidf"
          xmlns:dm="urn:ietf:params:xml:ns:pidf:data-model"
          xmlns:e="urn:ietf:params:xml:ns:pidf:status:rpid"
          xmlns:ce="urn:cisco:params:xml:ns:pidf:rpid"
          entity="sip:1004@host">
  <dm:person>
    <e:activities>
      <e:on-the-phone/>     <!-- INUSE | BUSY | ONHOLD -->
      <ce:alerting/>        <!-- RINGING (Cisco-only) -->
      <e:busy/>             <!-- BUSY -->
      <ce:dnd/>             <!-- DND (Cisco-only) -->
    </e:activities>
  </dm:person>
  <tuple id="1004">
    <status><basic>open|closed</basic></status>
  </tuple>
</presence>
```

Stock res_pjsip_pidf_body_generator emits only the `<tuple>` part —
the firmware accepts that body (200 OK) but doesn't recognise it as
BLF. With our supplement loaded the same body now also carries the
`<dm:person>/<e:activities>` block, which is what flips the
firmware's BLF parser.

### res_pjsip_cisco_register_optionsind

Hooks `on_tx_response` via `ast_sip_register_service` and attaches
`application/x-cisco-remotecc-response+xml` optionsind body to every
outgoing REGISTER 200 OK for endpoints flagged Cisco. Body shape
mirrors the chan_sip patch's
`channels/sip/response.c:276-310 sip_response_send_with_options_ind`.

Endpoint id is extracted from the To-header user part of the
response, then the cisco sorcery object is looked up.

### res_pjsip_cisco_bulkupdate

Hooks REGISTER via `ast_sip_supplement` and queues a deferred task
to send an unsolicited REFER. The REFER carries multipart
`application/x-cisco-remotecc-request+xml` with three parts:
`<dndupdate>`, `<hlogupdate>`, and `<bulkupdate><contact
line="N">…</contact>`.

Body shape mirrors chan_sip patch's
`channels/sip/peers.c:2824 sip_peer_send_bulk_update`.

The line index and DND busy behavior come from the cisco sorcery
object. DND, hunt-group, and call-forward state come from astdb, while
MWI counts come from endpoint/AOR mailbox configuration.

Note the deliberate two-channel split for DND: `dnd_busy` is a static
operator preference (reject calls vs ring silently when DND is on),
configured in `pjsip.conf` on the cisco sorcery object; the on/off
state itself lives at runtime in astdb (`DND/<endpoint>`) so a
dialplan feature code or the phone's softkey can toggle it without a
config reload.

The module also exposes `pjsip cisco bulkupdate <endpoint>` for
operators or dialplan/AMI tooling that mutates `DND/<endpoint>` or
`CF/<endpoint>` directly and wants to push the refreshed state without
waiting for the next REGISTER.

### res_pjsip_cisco_unsolicited_blf

Same hooking pattern as bulkupdate. After REGISTER, walks the
endpoint's `subscribe = …` field from the cisco sorcery object and
sends an unsolicited Event: presence NOTIFY for each watched
extension. Body uses the same Cisco-flavoured PIDF as the
body-generator module (the build inlines that XML rather than
factoring it out — keeps each module self-contained and avoids
inter-module symbol exports).

This is the module that decisively flips the firmware into BLF
Speed Dial mode for the line buttons. Without it, the optionsind
body alone is necessary but not sufficient.

### res_pjsip_cisco_service_control

CLI commands that send manual unsolicited NOTIFYs for operational
purposes:

| Command | Event | Subscription-State | Body |
|---|---|---|---|
| `pjsip cisco check-sync <ep> [contact]` | `check-sync` | `terminated` | (none) |
| `pjsip cisco restart <ep> [contact]` | `service-control` | `active` | `action=restart` + `RegisterCallId={…}` + version stamps |
| `pjsip cisco reset <ep> [contact]` | `service-control` | `active` | `action=reset` + `RegisterCallId={…}` + version stamps |

Replaces chan_sip's `sip notify cisco-{check-cfg,restart,reset}`
templates from `sip_notify.conf`.

The `RegisterCallId` field is the Call-ID of the phone's own REGISTER
transaction; the firmware validates it and 400s a mismatch. It's read
per-contact from `contact->call_id` (the registrar records it on every
REGISTER), not from a single per-endpoint slot — a shared line
(`max_contacts > 1`) has several phones with distinct REGISTER
Call-IDs, and each NOTIFY must carry the Call-ID of the phone it's
addressed to. The version stamps are advisory and sent all-zero.

The optional `[contact]` is a substring matched against a registered
contact's URI or its `@hash`: present → only matching contacts get the
NOTIFY (restart one phone of a shared line); absent → every registered
contact. Both arguments tab-complete (endpoint = `type=cisco` object
ids; contact = the endpoint's registered contact URIs).

### res_pjsip_cisco_feature_events

Handles Cisco phone-originated DND softkey signaling. Cisco firmware
on the live fleet (CP7975/9.4.2, CP8861/14.1.1) emits an `Event:
presence` PUBLISH (`application/pidf+xml` carrying a `<ce:dnd/>`
activity) on every DND on/off press. The handler runs after PJSIP
authentication and before stock pubsub, and writes to the same
`DND/<endpoint>` astdb key bulkupdate reads. Also registers a
`cisco_auth` endpoint identifier that matches the post-401 PUBLISH by
its `Authorization` digest username, since Cisco's PUBLISH carries
the device MAC (not the line id) in its From-URI and stock
identifiers can't match it.

The same module closes a related gap for device-level REFER (RemoteCC
token registration, alarm reports, RemoteCC responses) and some PUBLISH
traffic, which Cisco also sources from a `sip:<mac>@phone-ip` From-URI:
on every authenticated REGISTER from a Cisco endpoint it harvests the
device MAC out of the Contact header parameters (`+sip.instance`'s
urn:uuid node, `+u.sip!devicename.ccm.cisco.com="SEPxxxx"`, or a bare
12-hex value) into an in-memory `MAC -> {endpoint id, source IP, expiry}`
map, and a `cisco_mac` endpoint identifier resolves a later request whose
From-URI user is one of those MACs back to the endpoint — gated on the
request arriving from the same source IP the REGISTER did. That gets the
request past the distributor (it would otherwise be logged "No matching
endpoint found" and 401'd before `res_pjsip_cisco_remotecc` sees it);
whether it then survives auth depends on the firmware's REFER re-auth
behaviour.

### res_pjsip_cisco_remotecc

Handles Cisco phone-originated RemoteCC REFERs with proprietary
`application/x-cisco-remotecc-*` bodies. It accepts token registration,
alarm, remotecc-response, and x-cisco-location notifications. It also
parses `<softkeyeventmsg>` and implements HLog by toggling
`HuntGroup/<endpoint>` in astdb, then sending a `<hlogupdate>` REFER
back to registered contacts. MCID resolves the supplied `<dialogid>`
via PJSIP's native `pjsip_ua_find_dialog` (through the shared
`cisco_dialog_session_lookup` helper in `cisco/session.h`), queues
`AST_CONTROL_MCID` on the matched channel when the call is bridged,
and sends Cisco statusline/tone feedback REFERs.

The module entry + dispatch live in `res/res_pjsip_cisco_remotecc.c`;
per-feature handlers (MCID, Park/ParkMonitor, StartRecording/
StopRecording) sit in sibling `res/cisco_remotecc/{mcid,park,record}.c`
compiled into the same `.so`. Same pattern as
`res_pjsip_cisco_conference.so`; nothing exported across module
boundaries (the `.exports` script keeps the internal `handle_*`
symbols local).

**res_parking is optional.** The parking symbols (`ast_parking_topic`,
`ast_parked_call_type`, `ast_parking_is_exten_park`) live in the
asterisk binary itself, not `res_parking.so`, so this module links and
loads cleanly without `res_parking`. `handle_park` gates on
`ast_module_check("res_parking.so")` and returns `501` (Not
Implemented) when it isn't loaded — so MCID / HLog / Record / softkey
dispatch all continue to work on a deployment that intentionally omits
`res_parking`. Declared via `.optional_modules = "res_parking"` so the
Asterisk loader orders `res_parking` before us when it IS configured.

**Park / ParkMonitor.** The softkey REFER carries `<dialogid>` for the
call to park; `handle_park` resolves it to the phone's channel + its
bridge peer (synchronously, so the REFER response is meaningful — `400`
malformed, `481` call gone/unbridged, `404` parkext not in dialplan,
`202` accepted), then queues a task on its own serializer
(`pjsip/cisco-park`) that blind-transfers the *peer* to `<parkext>@<the
parker's transfer context>` via `ast_bridge_transfer_blind()` — the
same path a plain "transfer to 700" takes, so `res_parking` owns slot
allocation, comeback-to-origin (from the `BLINDTRANSFER` var the blind
transfer sets) and tearing down the phone's leg. No bridge surgery and
no SIP-side work runs on the rx thread. `parkext` defaults to `700` and
is overridable per endpoint (`parkext=` on the `type=cisco` object);
the context is `TRANSFER_CONTEXT` chan var, else the endpoint's
configured context — matching stock `res_pjsip_refer`. Before the
transfer runs, the task re-checks the parker's current bridge peer
against the one captured at REFER time (a peer change in the meantime →
abort with feedback, don't park the wrong call). A subscription to
`ast_parking_topic()` (selective filter: `ast_parked_call_type()` +
`stasis_subscription_change_type()`) learns the slot and drives the
phone feedback: for `Park`, one `<statuslineupdatereq>` REFER toast
announcing the slot (then unsubscribe); for `ParkMonitor`, an
`Event: refer` / `application/dialog-info+xml` NOTIFY (Cisco's
`xmlns:call=…callinfo-dialog`, `<call:park><event>parked|retrieved|
forwarded|abandoned|error</event>`) on every parked-call
lifecycle event so a "Park slot N" line button tracks the orbit,
unsubscribing on the terminal event. Mirrors chan_sip's
`handle_remotecc_park` / `park_thread` / `remotecc_park_notify`, but
parks via the blessed transfer API rather than `chan_sip`'s
`sip_pvt`-internal park machinery.

**StartRecording / StopRecording.** The softkey REFER carries
`<dialogid>` for the call; `handle_record` resolves it to the phone's
channel (same `cisco_dialog_session_lookup` → `cisco_session_channel_ref`
path as Park), validates the channel is `AST_STATE_UP`, and queues an
`ast_pbx_exec_application("MixMonitor", …)` (or `"StopMixMonitor"`)
task on the shared `pjsip/cisco-remotecc` serializer so it never runs
on the SIP rx thread. Default filename is
`cisco-<endpoint>-<uniqueid>.wav` (resolved against the configured
MixMonitor directory, normally `/var/spool/asterisk/monitor/`); the
chan var `CISCO_RECORD_FILENAME` overrides it per call when set in
dialplan before the softkey is pressed. The `b` MixMonitor option
pauses recording when the channel isn't bridged. chan_sip's patch
takes a different route — it creates a second SIP dialog back to the
phone (connected-line "Record") and dispatches to dialplan extension
`record` — which is necessary on chan_sip because there's no bridge to
attach an audiohook to; on chan_pjsip the call is already on a bridge,
so a direct MixMonitor on the phone's channel is the natural fit.

Conference-related softkeys (Confrn / ConfList / Mute / Remove /
Update / Select / Unselect / Join) are claimed by
`res_pjsip_cisco_conference` at an earlier priority slot — see that
module's section below. `Cancel` is currently accepted as a no-op
(server-side cancel of an in-progress operation is not yet wired —
see the TODO in `handle_softkey_event`). Any other RemoteCC softkey
this module sees and doesn't recognise gets `603 Decline`. Claiming
those REFERs (rather than letting them fall through) is what stops
stock PJSIP REFER transfer handling from trying to process Cisco
RemoteCC bodies as ordinary call transfers.

### res_pjsip_cisco_call_extras

Adds Cisco-specific call signaling details to regular INVITE/UPDATE
traffic for Cisco endpoints. It appends `X-cisco-sis-10.0.0` to
`Supported`, sends `Call-Info:
<urn:x-cisco-remotecc:callinfo>;orientation=...;security=...`, carries
`CISCO_CALLBACK_NUMBER` as `x-cisco-callback-number` in
Remote-Party-ID, and adds Cisco-side H.264 video SDP hints
`b=TIAS:4000000` plus default `imageattr` when an outgoing Cisco
INVITE/200 carries H.264 without imageattr. `CISCO_HUNTPILOT` is also
represented as `huntpiloturi` in Call-Info for outbound calls to the
phone.

This is intentionally narrower than the cisco-usecallmanager patch's
core H.264 support. Gareth's patch changes `res_format_attr_h264.c` so
`imageattr` becomes an Asterisk H.264 format attribute, parses incoming
`a=imageattr` into that attribute in chan_sip SDP handling, and emits it
again from the negotiated format on the other bridge leg. This
out-of-tree module only rewrites the outgoing SDP body on Cisco endpoint
sessions; it does not make `imageattr` survive Asterisk's media
capability negotiation or propagate it to a non-Cisco channel. Full
parity would require core/PJSIP SDP changes, not just another Cisco
supplement.

The module entry + supplement registrations + header-decoration code
(Call-Info, RPID, Conference identity, Supported) live in
`res_pjsip_cisco_call_extras.c`; the H.264 SDP hints and the
phantom-video suppression datastore — including the two-phase
peer-channel walker — sit in `res/cisco_call_extras/video.c`, compiled
into the same `.so`. Same pattern as the conference and remotecc modules; the
`.exports` script keeps everything local.

### res_pjsip_cisco_conference

Cisco RemoteCC conference control. Hooks into the same incoming-REFER
stream as `res_pjsip_cisco_remotecc` but at a slot earlier in the
pjsip module priority chain (`PJSIP_MOD_PRIORITY_APPLICATION - 2`),
so it gets first crack at parsing the body. Claims the
conference-family softkey REFERs; everything else falls through to
`res_pjsip_cisco_remotecc` unchanged.

Resolves the XML `<dialogid>` on every REFER to an `ast_sip_session`
via the shared `cisco_dialog_session_lookup` helper (in
`cisco/session.h`, wrapping `pjsip_ua_find_dialog` +
`ast_sip_dialog_get_session`), independent of the remotecc dialog
registry — no cross-module symbol export needed.

**ConfList + participant-pick action softkeys.** When ConfList
arrives the module walks the channel's bridge with `ast_bridge_peers`
and sends an unsolicited REFER back to the phone with a
`<datapassthroughreq applicationid="1">` echo and a
`<CiscoIPPhoneMenu>` listing each peer's caller-id name. Body shapes
mirror chan_sip's `channels/sip/conference.c sip_conference_participants`.
Bridge-agnostic — works against any Asterisk bridge containing the
phone's channel, including `ConfBridge()`. Mute / Remove / Update /
participant-pick action softkeys land via the chan_sip-style two-step
state machine: a sticky softkey REFER sets the pending action, the
next participant-pick REFER applies it; default action when no
softkey was pressed first is Mute, matching the patch.

**Confrn** (build a 3-way conference from the active 2-party call) is
fully wired: holdretrieve REFER + Cisco-flavoured completion NOTIFY,
the connected-line "Conference" display token, the explicit
consult-anchor softhangup, and the `cisco_keep_conference` knob that
controls what happens when the initiator hangs up first.

**Select / Unselect / Join** (multi-call merge) — Select adds a
dialog to a per-endpoint selected-calls list, Unselect removes it,
Join (pressed on the active call) merges the active call's phone-side
plus each selected call's remote-side into a single multimix bridge
and softhangs the selected calls' phone-side anchors. Cleanup is a
single OOB REFER with `<notifyreq><feature>Join</feature>
<status>Complete</status>` targeting the active dialog.

**RmLastConf** — remove the most-recently-joined participant. Track
join order via a per-channel timestamp datastore attached at every
remote-leg `ast_bridge_move` (helpers `cisco_conf_mark_joined()` /
`cisco_conf_find_last_joined()` in `res/cisco_endpoint/session.c`). The handler
resolves the REFER's `<dialogid>` to the phone's session → channel →
bridge, picks the tracked channel with the latest timestamp
(excluding the phone's own anchor, which isn't marked), and
`ast_bridge_remove()`s it. No-op with a NOTICE log when the bridge
has no tracked participants (a 2-party call, a stock ConfBridge we
didn't build, or all conference participants have already left).

## Build system

`Makefile` (no autoconf — too much ceremony for ten modules). It can
source pjproject headers from `pkg-config`, an explicit
`PJPROJECT_INCLUDE`, or an Asterisk source tree via `PJPROJECT_DIR`.
When `PJPROJECT_DIR` is set, Asterisk headers are derived from that
same source tree so struct layouts match the running binary.

`AST_MODULE_SELF_SYM` is hand-derived per-module by the generated
Make rules. `OBJ_DIR` defaults to `obj`, with module artifacts under
`$(MODULE_BUILD_DIR)` and the generated XML under `$(DOC_XML)`.
The linker script (`.exports` file) is the standard Asterisk one —
every symbol is local except `_IO_stdin_used` — so we don't have a
visibility fight on every Asterisk minor.

## XML documentation

Asterisk's strict sorcery validator demands a `<configObject>` match
in some `*-en_US.xml` file under `/var/lib/asterisk/documentation/`
before it'll accept field registrations. The Makefile extracts every
`/*** DOCUMENTATION ... ***/` block from `res/*.c` and
`res/cisco_*/*.c` into `$(DOC_XML)` at build time
(`obj/doc/res_pjsip_cisco-en_US.xml` by default). `make install`
copies that to `$(ASTERISK_DOC_DIR)`.

## Wire format reference

For implementers porting to other UAs or maintaining over time, the
exact wire content of every server→phone message is captured in
`/* … */` block comments at the top of each module source file. The
chan_sip patch is the source of truth — every body shape here is
line-mapped back to a function in
`channels/sip/{request,response,peers}.c` of
[`cisco-usecallmanager`](https://usecallmanager.nz).

## What's deliberately NOT here

`CallBack` (auto-callback when a busy party becomes free) — the
chan_sip patch implements a small state machine that watches a target
peer's registration / device-state and pushes an `<initiatecallreq>`
REFER back to the requester when it transitions. Useful but rarely
used in practice; ports as an additive module without redesign when
needed. Same supplement / register-service / pubsub pattern as the
existing modules.

Within CallBack, the phone's `usercalldata="Cancel"` sub-action would
destroy the queued callback — a feature-level cancel that lives
inside the CallBack handler. (The dispatch-level `softkeyevent="Cancel"`
is a bare 202 in chan_sip too — no server-side action there.)

### Removed: `as-feature-event` SUBSCRIBE handler (PATH A)

The chan_sip patch (patch line 6413 in `channels/sip/handlers.c`)
adds an `as-feature-event` SUBSCRIBE dispatcher that parses
`application/x-as-feature-event+xml` bodies carrying
`<SetDoNotDisturb>` / `<SetForwarding>` requests. The dispatcher is
unconditional in chan_sip — not gated by `cisco_usecallmanager`.

Originally ported into `res_pjsip_cisco_feature_events`. Removed
2026-05-14 after determining it was unused on the live fleet:

- Zero `as-feature-event` traffic observed in 7 days of journalctl.
- DND on/off softkey presses observed arriving via `Event: presence`
  PUBLISH (handled by PATH B).
- CFwdAll softkey behaviour confirmed out-of-band by the deployment
  owner as not depending on PATH A.

If a future deployment ships with phones that *do* emit
`as-feature-event` SUBSCRIBEs, the chan_sip patch is the wire-format
reference. The earlier PJSIP port (visible in `git log` before the
removal commit) is a starting point but only had DND wired through —
the SetForwarding branch was present but never lived-tested.

### Investigated and skipped: `buggymwi` urgent-count suffix stripping

Stock asterisk emits MWI bodies as `Voice-Message: NEW/OLD (URG_NEW/URG_OLD)`
— the trailing `(0/0)` is the urgent-message-count tuple per RFC 3458.
The chan_sip patch (line 1741) overrides `chan_sip.c`'s body builder so
the suffix is omitted when the recipient endpoint is Cisco-flagged,
inlining the old `buggymwi=yes` per-peer flag into the global
`cisco_usecallmanager=yes` flag. Workaround for 79xx-era firmware
that couldn't parse the trailing tuple and would fail to light the
message-waiting indicator.

Verified on 2026-05-14 against the live fleet (7975 / 9.4.2 and
CP8861 / 14.1.1): both firmwares correctly parse the `(0/0)` and
light the red MWI lamp. Workaround is not needed for any firmware
this project currently encounters.

If a future deployment ships with older firmware that *does* break:
the port is a `body_supplement` registered for
`application/simple-message-summary` (the MIME type asterisk uses for
MWI NOTIFY bodies). The supplement strips trailing ` (0/0)` from any
`Voice-Message:` line when the recipient endpoint has a parallel
`type=cisco` sorcery section (same gating contract the rest of the
project uses). ~30 lines, slots into `res_pjsip_cisco_pidf_body_generator.so`
alongside the existing PIDF supplement. No redesign required.
