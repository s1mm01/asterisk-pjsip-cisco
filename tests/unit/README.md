# Unit tests

Lightweight CI signal beyond the top-level `make` (which only proves
the .so files compile).

Run from the repo root:

```sh
make tests              # both flavours
make -C tests/unit smoke   # build-artefact checks only
make -C tests/unit unit    # pjlib-linked C tests only
```

## What's covered

**smoke** (`smoke.sh`) — sanity checks the files Asterisk's loader will
look at:

- `obj/doc/res_pjsip_cisco-en_US.xml` parses (xmllint).
  The Makefile harvests `/*** DOCUMENTATION ***/` blocks from
  `res/*.c` and `res/cisco_*/*.c` via awk/sed. A malformed block
  in one file breaks documentation for all ten modules and Asterisk
  silently refuses to register sorcery types whose `<configObject>`
  doesn't parse.

- Each `obj/*.so` has `__mod_info`, `load_module`, and
  `unload_module` symbols at the locations Asterisk's loader
  resolves via dlsym at registration time. Catches version-script
  regressions (e.g. `local: *;` accidentally hiding the entry
  points) and stripped/half-linked builds.

Apt deps: `libxml2-utils` (`xmllint`), `binutils` (`readelf`).

**unit** — standalone C programs linked against pjproject. Each test
file is its own `main()` with `assert()`s; the Makefile builds and
runs them.

Today this exercises pjlib primitives (`pj_stricmp2`, `pj_strchr`,
`pj_str`) used by `cisco_media_type_is`, `cisco_copy_sip_uri_hostport`,
and the conference module's body-building paths. These are pure pjlib
APIs we depend on; pinning them catches the rare-but-real failure mode
of pjproject behaviour drifting across versions.

## Adding a unit test

The harness pattern is in `test_string_utils.c`:

1. Copy it to `test_<name>.c`.
2. Append `<name>` to `UNIT_BINS` in `Makefile`.
3. Write `main()` with `assert()`s.

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
