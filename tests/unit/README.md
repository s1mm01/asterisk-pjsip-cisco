# Unit tests

Lightweight CI signal beyond the top-level `make` (which only proves
the .so files compile).

Run from the repo root:

```sh
make tests              # both flavours
make -C tests/unit smoke   # build-artefact checks only
make -C tests/unit unit    # pjlib-linked C tests only
```

The smoke target follows the top-level artifact variables when invoked
via `make tests`: `OBJ_DIR`, `MODULE_BUILD_DIR`, and `DOC_XML`.

## What's covered

**smoke** (`smoke.sh`) — sanity checks the files Asterisk's loader will
look at:

- `$(DOC_XML)` parses (xmllint).
  The Makefile harvests `/*** DOCUMENTATION ***/` blocks from
  `res/*.c` and `res/cisco_*/*.c` via awk/sed. A malformed block
  in one file breaks documentation for all ten modules and Asterisk
  silently refuses to register sorcery types whose `<configObject>`
  doesn't parse.

- Each built `.so` under `$(MODULE_BUILD_DIR)` has `__mod_info`,
  `load_module`, and `unload_module` symbols at the locations Asterisk's loader
  resolves via dlsym at registration time. Catches version-script
  regressions (e.g. `local: *;` accidentally hiding the entry
  points) and stripped/half-linked builds.

Apt deps: `libxml2-utils` (`xmllint`), `binutils` (`readelf`).

**unit** — standalone C programs. Each test file is its own `main()`
with `assert()`s; the Makefile builds and runs them.

Currently:

- `test_string_utils` — pjlib primitives (`pj_stricmp2`, `pj_strchr`,
  `pj_str`) used by `cisco_media_type_is`,
  `cisco_copy_sip_uri_hostport`, and the conference module's
  body-building paths. Pure pjlib APIs we depend on; pinning them
  catches the rare-but-real failure mode of pjproject behaviour
  drifting across versions. Pjproject-linked.

- `test_xml_bodies` — snprintfs each wire-format XML body template
  (the `{bulkupdate,remotecc}_bodies.h` macros: DND part, HLog part,
  bulkupdate body, MCID feedback, Park toast / orbit, HLog update)
  with bench-realistic substitutions, then parses the result with
  libxml2. Catches sprintf typos (missing close tag, mismatched
  attribute quote) that would otherwise only surface on a real Cisco
  phone. libxml2-linked, no pjproject. Cisco-private `\200` glyph
  bytes embedded in MCID-status / Park-toast bodies are handled via
  libxml2's recovery mode (deliberate firmware behaviour, not
  malformed XML from our side). CI runs this as its own step on every
  PR — the pjproject-linked half of `make tests` is local-only
  because CI doesn't build pjproject static libs.

- `test_blf_pidf` — golden-output test for
  `cisco_blf_build_pidf` (the helper behind the unsolicited Event:
  presence NOTIFYs). Compiles `res/cisco_unsolicited_blf/pidf.c`
  against the shim Asterisk headers under `include/asterisk/` and
  links it with the `ast_str_*` / `ast_sip_sanitize_xml`
  implementations in `asterisk_stubs.c`. Exercises every documented
  exten-state / presence-state combination plus XML-special escaping,
  comparing each rendered body byte-for-byte against a golden literal
  built from the documented wire shape. Catches activity-tag drift,
  attribute-reorder, namespace edits, and missing escaping before
  they reach a real Cisco phone. No pjproject linkage; the shim
  headers / stub implementations let the helper run as a pure
  function.

## Adding a unit test

The right template depends on what the helper under test depends on.

**Pjproject primitives only** — copy `test_string_utils.c`. Already
linked against pjproject; use this for code that calls
`pj_str` / `pj_strcmp` / `pj_pool_*`.

**Wire-format string templates** — copy `test_xml_bodies.c`. Uses
libxml2 for structure validation, no Asterisk runtime, no pjproject.
Right for `*_bodies.h`-style snprintf-template tests.

**Body-building helpers that touch `ast_str_*` / Asterisk APIs** —
copy `test_blf_pidf.c`. The helper to be tested needs to be
extracted from its module's entry point file into its own
sibling `.c` (compile target shared between the production `.so`
build and the test). The Makefile rule for the test compiles a
private copy of that `.c` against the stub headers in
`include/asterisk/` and links with `asterisk_stubs.o`. If the
helper uses an Asterisk API the stubs don't yet cover, extend
`asterisk_stubs.c` + the matching header under `include/asterisk/`
once; subsequent golden tests pick up the additions for free.

For all three patterns:

1. Write `test_<name>.c` with `main()` + `assert()`s.
2. Append `<name>` to `UNIT_BINS` in `Makefile`.
3. If you're following the third pattern, add the explicit Makefile
   rule modelled on `test_blf_pidf` (the generic `%: %.c` rule
   assumes pjproject linkage).

## What's intentionally NOT covered

Most of the helpers under `res/cisco_*/` take Asterisk types
(`struct ast_str`, `ast_sip_session *`, `ast_xml_node *`, `ao2_*`).
Pulling them into a standalone test binary needs a stub layer for the
broader Asterisk runtime that `res_pjsip.h` transitively drags in.
That layer is not present in the current harness. Two reasons:

1. The smoke target already catches the failure modes those tests
   would most often catch — module load breakage, missing entry-
   point symbols, malformed XML.
2. The Asterisk testsuite (the in-tree `TEST_INIT` / `TEST_EXECUTE`
   macros invoked via `asterisk -rx "test execute"`) is the right
   home for tests that need a real runtime. Out-of-tree shadowing
   of it is a lot of upfront stub maintenance for limited extra
   coverage.

If a helper turns out to be worth a real test, the recommended path
is to extract its pure logic into a callable form (e.g. take `const
char *` instead of `pjsip_sip_uri *`, with a thin pjsip-shaped
wrapper at the call site) and test the pure form. That's
production-code-improving rather than test-only scaffolding.
