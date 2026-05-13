# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this project is

Ten out-of-tree Asterisk shared modules (`res_pjsip_cisco_*`) that bolt Cisco Enterprise SIP firmware support onto stock `chan_pjsip` **without patching Asterisk core**. The body shapes and REGISTER-time behaviour are line-mapped from Gareth Palmer's `cisco-usecallmanager` chan_sip patch — when in doubt about a wire format, the chan_sip patch is the source of truth.

## Build & development commands

```sh
make                      # build all .so + regen doc/res_pjsip_cisco-en_US.xml
make clean
sudo make install         # to ASTERISK_MODULES_DIR / ASTERISK_DOC_DIR / ASTERISK_SAMPLE_DIR
sudo make uninstall
sudo make check           # runtime: queries a running asterisk for "module show like cisco_"
dpkg-buildpackage -us -uc -b -d   # Debian packaging (-d skips Build-Depends not on stock Ubuntu)
```

Header sourcing — pick **one** of these when invoking `make`:

- `make PJPROJECT_DIR=/path/to/asterisk-22.9.0` — points at an Asterisk source tree; the Makefile derives both Asterisk headers (`<src>/include/`) and bundled pjproject headers (`<src>/third-party/pjproject/source/...`) from it. **Preferred when Asterisk is self-built.**
- `make PJPROJECT_INCLUDE="-DPJ_AUTOCONF=1 -I.../pjlib/include ..."` — pin pjproject headers manually; Asterisk headers come from `/usr/include` (asterisk-dev).
- Bare `make` — uses `pkg-config libpjproject` and asterisk-dev. Only safe when both are in lockstep with the running asterisk binary.

The `check-headers` target is a sanity gate; it fails fast if neither path resolves.

There is **no unit-test framework**. CI (`.github/workflows/ci.yml`) builds against the highest stable tags of Asterisk 20.x (the LTS before 22), 22.x, and 23.x — downloads matching pjproject (version pinned from `asterisk-src/third-party/versions.mak`), stubs `buildopts.h`, and verifies all ten `.so` files plus XML validity. Behaviour is verified by hand against a real Cisco phone (on 22.9.x) — see `tests/README.md` for the parallel-TCP-on-:5160 recipe.

## The header-mismatch trap (read before changing struct field access)

Any code that touches `struct ast_sip_endpoint` (or other PJSIP structs) deeper than its first few fields must be compiled against the **exact same headers** the running asterisk binary was built with. `asterisk-dev` from apt is frequently stale relative to a self-built asterisk; building against stale headers produces modules that compile cleanly and SEGV at runtime when struct field offsets diverge. This is why `PJPROJECT_DIR` is the recommended build mode — it pins both header sets to the same tree.

## Architecture (the non-obvious bits)

The Cisco firmware classifies a line button as **BLF Speed Dial** (lit/unlit, hook icon) vs **plain Speed Dial** (no state, keypad icon) only when **all four** of these signals are present at REGISTER time:

1. SEP file `<featureID>21</featureID>` (provisioning, not our concern).
2. REGISTER 200 OK with a Cisco RemoteCC **optionsind** body.
3. Unsolicited REFER carrying a multipart **bulkupdate** body with per-line config.
4. **Unsolicited** `Event: presence` NOTIFYs (distinct from NOTIFYs sent in response to the phone's SUBSCRIBE — same body, different trigger; the firmware checks).

Missing any of (2)–(4) silently downgrades the buttons. The module set supplies these signals plus related softkey, service-control, call-signaling, and conference behaviours. See `ARCHITECTURE.md` for the full module-by-module rationale.

### The gating contract

Existence of a `[name] type=cisco` sorcery section (defined by `res_pjsip_cisco_endpoint`, schema in `res/cisco_endpoint.h`) is the **single gating signal** for every Cisco-specific behaviour in the other modules. A non-Cisco endpoint with no parallel `type=cisco` section falls through every supplement/hook unchanged. The PIDF body generator (`res_pjsip_cisco_pidf_body_generator`) is the deliberate exception — it's global and emits Cisco-flavoured PIDF for any presence subscriber, since non-Cisco UAs ignore the extra RPID elements.

### No cross-module symbol exports

Asterisk's per-module `.exports` linker scripts default to `local: *;` (every symbol hidden). Rather than fight that to share helpers, the convention here is: **shared logic lives in `static inline` functions in `res/cisco_endpoint.h` or `res/cisco_session.h`**, and each consuming module compiles its own copy. New cross-module helpers should follow the same pattern; do not export symbols from `res_pjsip_cisco_endpoint.so`.

### Module loading & PIDF body-generator slot

`res_pjsip_cisco_pidf_body_generator` does two things in one `.so`:
- Registers a **body supplement** for `application/pidf+xml` that augments stock `res_pjsip_pidf_body_generator`'s output (additive, both modules run; pubsub passes the same `pjpidf_pres` tree to the supplement).
- Registers a **body generator** for `application/cpim-pidf+xml` with a `load_pri` lower than `AST_MODPRI_CHANNEL_DEPEND` so it claims the slot before stock `res_pjsip_xpidf_body_generator`. The startup `WARNING: A body generator for application/cpim-pidf+xml is already registered` is **expected and harmless** — that's stock xpidf declining after we got there first. Don't try to silence it with `noload`; that breaks unrelated MWI flows.

### XML documentation

Asterisk's strict sorcery validator demands a matching `<configObject>` in some XML file under `$ASTERISK_DOC_DIR` before it will accept field registrations. The Makefile harvests every `/*** DOCUMENTATION ... ***/` block from `res/*.c`, concatenates them into `doc/res_pjsip_cisco-en_US.xml`, and `make install` deploys it. **When you add or rename a sorcery field, the matching `<configOption>` block in the source file is mandatory** — Asterisk will refuse to load the module otherwise.

### Hook style for REGISTER-driven behaviour

The optionsind, bulkupdate, and unsolicited-BLF modules all hook the outgoing REGISTER 200 OK via `ast_sip_register_supplement` with an `outgoing_response` callback. Endpoint identity is extracted from the To-header user part. Unsolicited follow-ups (REFER for bulkupdate, NOTIFY for unsolicited BLF) are sent as deferred tasks rather than inline in the response hook — sending in-line breaks the response transaction.

## Coding conventions

Tabs for indentation, K&R braces, `snake_case`, module names `res_pjsip_cisco_<feature>`. Match Asterisk's existing C style. Comments only where they explain Asterisk/PJSIP lifecycle constraints, ABI assumptions, or non-obvious Cisco firmware behaviour — every existing file has a top-level block comment mapping its body shapes to the chan_sip patch line numbers; preserve and update those when changing wire format.

Commit subjects use a short scope prefix: `cisco: ...`, `bulkupdate: ...`, `pidf: ...`, `ci: ...`, `debian: ...`. Imperative mood.
