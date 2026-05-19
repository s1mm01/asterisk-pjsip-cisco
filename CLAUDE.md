# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this project is

Ten out-of-tree Asterisk shared modules (`res_pjsip_cisco_*`) that bolt Cisco Enterprise SIP firmware support onto stock `chan_pjsip` **without patching Asterisk core**. The body shapes and REGISTER-time behaviour are line-mapped from Gareth Palmer's `cisco-usecallmanager` chan_sip patch — when in doubt about a wire format, the chan_sip patch is the source of truth.

## Build & development commands

```sh
make                      # build all .so into $(OBJ_DIR) + regen $(DOC_XML)
make doc                  # regenerate only the XML documentation
make clean                # rm -rf $(OBJ_DIR)/
sudo make install         # built .so -> ASTERISK_MODULES_DIR, $(DOC_XML) -> ASTERISK_DOC_DIR, conf-samples -> ASTERISK_SAMPLE_DIR
sudo make uninstall
sudo make check           # runtime: queries a running asterisk for "module show like cisco_"
dpkg-buildpackage -us -uc -b -d   # Debian packaging (-d skips Build-Depends not on stock Ubuntu)
```

Header sourcing — pick **one** of these when invoking `make`:

- `make ASTERISK_SRC_DIR=/path/to/asterisk-22.9.0` — points at an Asterisk source tree; the Makefile derives Asterisk headers (`<src>/include/`), bundled pjproject headers (`<src>/third-party/pjproject/source/…`), AND auto-applies asterisk's pjproject patch overlay (`<src>/third-party/pjproject/patches/config_site.h`) from it. **Strongly preferred — the only mode that guarantees struct compatibility with the runtime asterisk.** See the header-mismatch-trap section below for why.
- `make PJPROJECT_INCLUDE="-DPJ_AUTOCONF=1 -I.../pjlib/include …"` — pin pjproject headers manually; Asterisk headers come from `/usr/include` (asterisk-dev). The Makefile prints a build-time warning that struct layouts may not match the runtime asterisk.
- Bare `make` — uses `pkg-config libpjproject` and asterisk-dev. Only safe when both are in lockstep with the running asterisk binary. Same warning fires.

`PJPROJECT_DIR` is the deprecated legacy name for `ASTERISK_SRC_DIR` (the variable always pointed at an asterisk source tree, not a pjproject one). It still works but emits a deprecation note.

The `check-headers` target is a sanity gate; it fails fast if neither path resolves.
`OBJ_DIR` defaults to `obj`; `MODULE_BUILD_DIR` and `DOC_XML` derive from it unless explicitly overridden.

There is **no unit-test framework**. CI (`.github/workflows/ci.yml`) builds against the highest stable tags of Asterisk 20.x (the LTS before 22), 22.x, and 23.x — downloads matching pjproject (version pinned from `asterisk-src/third-party/versions.mak`), stubs `buildopts.h`, and verifies all ten `.so` files plus XML validity. Behaviour is verified by hand against a real Cisco phone (on 22.9.x) — see `tests/README.md` for the parallel-TCP-on-:5160 recipe.

## The header-mismatch trap (read before changing struct field access)

Any code that touches `struct ast_sip_endpoint` (or other PJSIP structs) deeper than its first few fields must be compiled against the **exact same headers** the running asterisk binary was built with. `asterisk-dev` from apt is frequently stale relative to a self-built asterisk; building against stale headers produces modules that compile cleanly and SEGV at runtime when struct field offsets diverge. This is why `ASTERISK_SRC_DIR` is the recommended build mode — it pins both header sets to the same tree.

There is a quieter form of the same trap that bites even when headers and binary nominally agree on pjproject's *version*. Asterisk's bundled pjproject is built with a customised `third-party/pjproject/patches/config_site.h` that redefines several layout-critical macros — most notably `PJSIP_MAX_PKT_LEN` (65535 vs default 2000), `PJSIP_MAX_MODULE` (38 vs 32), and `PJMEDIA_MAX_SDP_FMT` (varies by asterisk version). Those macros size **arrays inside** `pjsip_rx_data`, `pjmedia_sdp_media`, and `pjsip_endpoint`, so they directly shift the offset of every later struct field. Compiling our modules against stock pjproject headers (e.g. an `apt source pjproject`-style tree without the overlay) while loading them into an asterisk built *with* the overlay produces struct offsets that disagree by tens of kilobytes — every `rdata->msg_info.msg` read lands at the wrong address and silently returns NULL. No SEGV, no warning; `on_rx_request` hooks just no-op.

The Makefile's `ASTERISK_SRC_DIR` mode handles this automatically: it overlays `<dir>/third-party/pjproject/patches/config_site.h` onto `<dir>/third-party/pjproject/source/pjlib/include/pj/config_site.h` (and `asterisk_malloc_debug.h` alongside it) as a build prerequisite, mirroring asterisk's own bundled-pjproject Makefile rule. In any other mode the Makefile prints a build-time warning.

For **Debian/Ubuntu apt asterisk** users: `make ASTERISK_SRC_DIR=/usr/include` does **not** work (apt asterisk-dev doesn't ship the bundled pjproject tree). The right invocation is:

```sh
apt source asterisk                              # drops asterisk-X.Y.Z.../ in CWD
cd asterisk-X.Y.Z.../
# Debian splits pjproject into orig-Xpjproject which lands at ./Xpjproject;
# move it to where asterisk (and our Makefile) expects to find it:
mkdir -p third-party/pjproject/source
cp -r Xpjproject/. third-party/pjproject/source/
cd third-party/pjproject/source && ./configure --disable-libwebrtc-aec && cd -
# Now build the modules against this tree:
cd /path/to/asterisk-pjsip-cisco
make ASTERISK_SRC_DIR=/path/to/asterisk-X.Y.Z...
```

The CI workflow does exactly this — see the "prepare apt asterisk source tree for ASTERISK_SRC_DIR" step in `.github/workflows/ci.yml`.

## Architecture (the non-obvious bits)

The Cisco firmware classifies a line button as **BLF Speed Dial** (lit/unlit, hook icon) vs **plain Speed Dial** (no state, keypad icon) only when **all four** of these signals are present at REGISTER time:

1. SEP file `<featureID>21</featureID>` (provisioning, not our concern).
2. REGISTER 200 OK with a Cisco RemoteCC **optionsind** body.
3. Unsolicited REFER carrying a multipart **bulkupdate** body with per-line config.
4. **Unsolicited** `Event: presence` NOTIFYs (distinct from NOTIFYs sent in response to the phone's SUBSCRIBE — same body, different trigger; the firmware checks).

Missing any of (2)–(4) silently downgrades the buttons. The module set supplies these signals plus related softkey, service-control, call-signaling, and conference behaviours. See `ARCHITECTURE.md` for the full module-by-module rationale.

### The gating contract

Existence of a `[name] type=cisco` sorcery section (defined by `res_pjsip_cisco_endpoint`, schema in `include/cisco/endpoint.h`) is the **single gating signal** for every Cisco-specific behaviour in the other modules. A non-Cisco endpoint with no parallel `type=cisco` section falls through every supplement/hook unchanged. The PIDF body generator (`res_pjsip_cisco_pidf_body_generator`) is the deliberate exception — it's global and emits Cisco-flavoured PIDF for any presence subscriber, since non-Cisco UAs ignore the extra RPID elements.

### Shared helpers live in `res_pjsip_cisco_endpoint.so`

Same pattern stock Asterisk uses for `ast_sip_*` (defined in `res_pjsip.so`, called from every PJSIP submodule):

- The six topical headers under `include/cisco/` — `endpoint.h`, `rdata.h`, `register.h`, `refer.h`, `session.h`, `orig_host.h` — contain **declarations only** (struct definitions, typedefs, function prototypes). Consumers `#include "cisco/<topic>.h"` (with `-Iinclude`).
- The bodies live in sibling `.c` files under `res/cisco_endpoint/` (`endpoint.c`, `rdata.c`, `register.c`, `refer.c`, `session.c`, `orig_host.c`) which **compile into `res_pjsip_cisco_endpoint.so`** alongside the module entry point `res/res_pjsip_cisco_endpoint.c`.
- `res_pjsip_cisco_endpoint.c`'s `AST_MODULE_INFO` carries `AST_MODFLAG_GLOBAL_SYMBOLS` so Asterisk's loader (which defaults to `RTLD_LOCAL`) re-opens the module with `RTLD_GLOBAL` and makes its symbols visible to subsequent `dlopen`s.
- `res/res_pjsip_cisco_endpoint.exports` lists `cisco_*` as `global:`. Every other module's `.exports` has `local: *;` only. The Makefile passes `-Wl,--version-script=res/res_pjsip_cisco_<module>.exports` for every `.so`, so the export set is enforced.
- Module load order is governed by each consumer's `AST_MODULE_INFO.requires` field — they list `res_pjsip_cisco_endpoint`, which guarantees the helpers are resolvable by the time a consumer loads.

To add a new shared helper: declare it in the topical `.h` under `include/cisco/`, define it in the sibling `.c` under `res/cisco_endpoint/`. It picks up the `cisco_*` export glob automatically. No need to touch `.exports` unless the helper name doesn't start with `cisco_`.

The grouping into topical headers (rather than one big shared `.h`) is for readability — split by concern: endpoint state, REGISTER tracking, REFER sending, rdata/URI/XML utilities, session/dialog lookup. Cross-cisco header dependencies are: `cisco/rdata.h` → `cisco/endpoint.h`; `cisco/register.h` → `cisco/rdata.h` (transitively `cisco/endpoint.h`); `cisco/refer.h`, `cisco/session.h`, and `cisco/orig_host.h` are independent.

**`orig_host` is a different shape** to the others: it's not a function library that other modules call, it's an `on_tx_request` `pjsip_module` that `res_pjsip_cisco_endpoint`'s `load_module` registers globally. Every outbound SIP request from any consumer module (or stock asterisk) passes through it; the hook rewrites the Request-URI and To-URI back to the phone's self-advertised host:port when `res_pjsip_nat`'s `rewrite_contact=yes` has left an `x-ast-orig-host` URI parameter on the Contact-derived RURI. Without this, Cisco firmware on NAT'd phones rejects unsolicited NOTIFYs/REFERs with `400 Bad Request` because the public NAT mapping in the RURI doesn't match what the phone knows about itself. A strict no-op for any URI lacking the parameter (LAN-registered contacts, upstream trunks, non-NAT'd targets) — consumers don't opt in, the hook just runs.

### Module loading & PIDF body-generator slot

`res_pjsip_cisco_pidf_body_generator` does two things in one `.so`:
- Registers a **body supplement** for `application/pidf+xml` that augments stock `res_pjsip_pidf_body_generator`'s output (additive, both modules run; pubsub passes the same `pjpidf_pres` tree to the supplement).
- Registers a **body generator** for `application/cpim-pidf+xml` with a `load_pri` lower than `AST_MODPRI_CHANNEL_DEPEND` so it claims the slot before stock `res_pjsip_xpidf_body_generator`. The startup `WARNING: A body generator for application/cpim-pidf+xml is already registered` is **expected and harmless** — that's stock xpidf declining after we got there first. Don't try to silence it with `noload`; that breaks unrelated MWI flows.

### XML documentation

Asterisk's strict sorcery validator demands a matching `<configObject>` in some XML file under `$ASTERISK_DOC_DIR` before it will accept field registrations. The Makefile harvests every `/*** DOCUMENTATION ... ***/` block from `res/*.c` and `res/cisco_*/*.c`, concatenates them into `$(DOC_XML)` (`obj/doc/res_pjsip_cisco-en_US.xml` by default), and `make install` deploys it. **When you add or rename a sorcery field, the matching `<configOption>` block in the source file is mandatory** — Asterisk will refuse to load the module otherwise.

### Hook style for REGISTER-driven behaviour

The optionsind, bulkupdate, and unsolicited-BLF modules all hook the outgoing REGISTER 200 OK via `ast_sip_register_supplement` with an `outgoing_response` callback. Endpoint identity is extracted from the To-header user part. Unsolicited follow-ups (REFER for bulkupdate, NOTIFY for unsolicited BLF) are sent as deferred tasks rather than inline in the response hook — sending in-line breaks the response transaction.

## Coding conventions

Tabs for indentation, K&R braces, `snake_case`, module names `res_pjsip_cisco_<feature>`. Match Asterisk's existing C style. Comments only where they explain Asterisk/PJSIP lifecycle constraints, ABI assumptions, or non-obvious Cisco firmware behaviour — every existing file has a top-level block comment mapping its body shapes to the chan_sip patch line numbers; preserve and update those when changing wire format.

Commit subjects use a short scope prefix: `cisco: ...`, `bulkupdate: ...`, `pidf: ...`, `ci: ...`, `debian: ...`. Imperative mood.
