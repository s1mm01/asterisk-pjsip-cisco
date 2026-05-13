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

The `cisco_endpoint_get()` helper lives `static inline` in
`res/cisco_endpoint.h` so each consuming module compiles its own
copy. Asterisk's per-module `.exports` linker scripts default to
`local: *;` and reliably-exporting cross-module helpers is a fight
with the build system.

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

Handles Cisco phone-originated DND / CFwdALL softkey signaling, in both
forms the firmware uses depending on the `<dndControl>` SEP setting:
`Event: as-feature-event` SUBSCRIBE (`application/x-as-feature-event+xml`)
and `Event: presence` PUBLISH (`application/pidf+xml` with a `<ce:dnd/>`
activity). Both handlers run after PJSIP authentication and before stock
pubsub, and write to the same astdb keys bulkupdate reads:
`DND/<endpoint>` and `CF/<endpoint>`. The PUBLISH path also registers a
`cisco_auth` endpoint identifier that matches the post-401 PUBLISH by its
`Authorization` digest username, since Cisco's PUBLISH carries the device
MAC (not the line id) in its From-URI and stock identifiers can't match
it.

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
`cisco_dialog_session_lookup` helper in `cisco_session.h`), queues
`AST_CONTROL_MCID` on the matched channel when the call is bridged,
and sends Cisco statusline/tone feedback REFERs.

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

Other parsed softkeys (Confrn, Join, …) are deliberately returned as `603 Decline`
until their Asterisk-side feature integrations are ported. Claiming
those REFERs is still useful: it prevents stock PJSIP REFER transfer
handling from trying to process Cisco RemoteCC bodies as ordinary call
transfer requests.

### res_pjsip_cisco_call_extras

Adds Cisco-specific call signaling details to regular INVITE/UPDATE
traffic for Cisco endpoints. It appends `X-cisco-sis-10.0.0` to
`Supported`, sends `Call-Info:
<urn:x-cisco-remotecc:callinfo>;orientation=...;security=...`, carries
`CISCO_CALLBACK_NUMBER` as `x-cisco-callback-number` in
Remote-Party-ID, and adds the legacy H.264 video SDP hints
`b=TIAS:4000000` plus default `imageattr` when stock SDP has no
imageattr. `CISCO_HUNTPILOT` is also represented as `huntpiloturi` in
Call-Info for outbound calls to the phone.

### res_pjsip_cisco_conference

Phase 1 conference control. Hooks into the same RemoteCC REFER stream
as `res_pjsip_cisco_remotecc` but at a slot earlier in the pjsip
module priority chain (`PJSIP_MOD_PRIORITY_APPLICATION - 2`), so it
gets first crack at parsing the body. The only request shape it
currently claims is `<softkeyeventmsg><softkeyevent>ConfList</…>`;
everything else falls through to `res_pjsip_cisco_remotecc` unchanged.

When ConfList arrives, it resolves the XML `<dialogid>` to an
`ast_sip_session` via the shared `cisco_dialog_session_lookup` helper
(in `cisco_session.h`, wrapping `pjsip_ua_find_dialog` +
`ast_sip_dialog_get_session`), walks the resulting channel's bridge with
`ast_bridge_peers`, and sends an unsolicited REFER back to the phone
carrying a multipart body with a `<datapassthroughreq
applicationid="1">` echo and a `<CiscoIPPhoneMenu>` populated with
each peer's caller-id name. Body shapes mirror the chan_sip patch's
`channels/sip/conference.c sip_conference_participants`.

Bridge-agnostic: this works against any Asterisk bridge containing
the phone's channel, not just Cisco-built ad-hoc conferences. A user
in a `ConfBridge()` conference pressing ConfList sees the ConfBridge
participants the same way.

Phase 1 is read-only. The Mute / Remove / Update softkeys on the
returned menu surface as `<datapassthroughreq>` REFERs the conference
module does not yet recognise; those currently fall through to the
remotecc handler, which `603 Decline`s them. Phase 2 will add Confrn
(build a conference from the active 2-party call), then Phase 3 Join
+ Select, then Phase 4 RmLastConf / multi-admin / keep-conference.

## Build system

`Makefile` (no autoconf — too much ceremony for ten modules). It can
source pjproject headers from `pkg-config`, an explicit
`PJPROJECT_INCLUDE`, or an Asterisk source tree via `PJPROJECT_DIR`.
When `PJPROJECT_DIR` is set, Asterisk headers are derived from that
same source tree so struct layouts match the running binary.

`AST_MODULE_SELF_SYM` is hand-derived per-module via Make's `$<`. The
linker script (`.exports` file) is the standard Asterisk one — every
symbol is local except `_IO_stdin_used` — so we don't have a
visibility fight on every Asterisk minor.

## XML documentation

Asterisk's strict sorcery validator demands a `<configObject>` match
in some `*-en_US.xml` file under `/var/lib/asterisk/documentation/`
before it'll accept field registrations. We extract every
`/*** DOCUMENTATION ... ***/` block from the source files into
`doc/res_pjsip_cisco-en_US.xml` at build time. `make install`
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

Out of scope for this project (for now):

- **The conference-building RemoteCC softkeys** — Confrn (build a
  conference from the active 2-party call), Join (merge a held call /
  another conference in), and Select + Direct Transfer (merge two held
  calls). Plain blind/attended transfer via the `Trnsfer` softkey
  already works through stock `chan_pjsip` REFER handling, and **Park /
  ParkMonitor** are now implemented (see
  `res_pjsip_cisco_remotecc` below — blind-transfer the peer to
  `res_parking`'s parkext); this is the remaining subset CallManager
  drives via `x-cisco-remotecc-request+xml` REFERs, where the server has
  to parse the softkey XML, do the channel surgery (ConfBridge /
  bridge-transfer), and REFER back to the phone to keep its
  call-appearance UI in sync. `res_pjsip_cisco_remotecc` parses these
  and `603 Decline`s the unimplemented ones; `res_pjsip_cisco_conference`
  is the Phase-1 scaffold for the conference side (read-only ConfList).
  Replaceable for fleets that accept the UX change by dialplan codes
  (`*8` to pickup, `*0<ext>` for a chained-transfer conference).

If/when this becomes needed it's an additive module that slots in
alongside the existing ones without redesign — same supplement /
register-service / pubsub pattern.
