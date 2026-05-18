# Repository Guidelines

## Project Structure & Module Organization

This repository builds out-of-tree Asterisk `res_pjsip_cisco_*` shared modules for Cisco Enterprise SIP firmware support.

- `res/` contains the C sources: ten `res_pjsip_cisco_<feature>.c` entry files at the top level (one per `.so`), each paired with `res_pjsip_cisco_<feature>.exports` (linker version-script). Multi-file modules carry their helpers in a `res/cisco_<feature>/` subdir, with any internal header in `res/cisco_<feature>/include/<feature>_private.h`.
- `include/cisco/` holds the six topical public headers (`endpoint.h`, `rdata.h`, `register.h`, `refer.h`, `session.h`, `orig_host.h`) — the `cisco_*` symbols `res_pjsip_cisco_endpoint.so` exports to its sister modules. Their bodies sit under `res/cisco_endpoint/`.
- `obj/` is the default build output tree (gitignored): `obj/res_pjsip_cisco_<feature>.so` for installables, `obj/res_pjsip_cisco_<feature>/*.o` for helper objects, `obj/doc/res_pjsip_cisco-en_US.xml` for the generated Asterisk XML documentation. Override `OBJ_DIR` when a different artifact root is needed.
- `conf-samples/` contains sample `pjsip.conf` Cisco sections.
- `debian/` contains Debian packaging metadata and `debian/rules`.
- `tests/README.md` documents the manual test bench against a real Cisco phone.
- `.github/workflows/ci.yml` builds against the latest Asterisk 20.x, 22.x, and 23.x releases.

## Build, Test, and Development Commands

- `make` builds all modules into `$(MODULE_BUILD_DIR)` and regenerates the XML documentation at `$(DOC_XML)` (defaults: `obj/` and `obj/doc/res_pjsip_cisco-en_US.xml`). `make doc` regenerates only the XML. Use `PJPROJECT_DIR=/path/to/asterisk-src` when building against a source-built Asterisk.
- `make clean` removes the `$(OBJ_DIR)` tree (objects, `.so` files, generated XML — everything `make` produced).
- Output-path overrides chain through a single root: `OBJ_DIR` is the overall build root; `MODULE_BUILD_DIR` defaults to `$(OBJ_DIR)` and holds the `.o`/`.so` files; `DOC_BUILD_DIR` defaults to `$(OBJ_DIR)/doc` and holds the XML; `DOC_XML` defaults to `$(DOC_BUILD_DIR)/res_pjsip_cisco-en_US.xml`. Set just `OBJ_DIR` to relocate everything together, or override any of the lower-level vars independently (distro packaging that wants modules and docs in different `dh_install` prefixes).
- `sudo make install` installs modules, docs, and sample config.
- `sudo make uninstall` removes installed project files.
- `sudo make check` reports whether each Cisco module is loaded in a running Asterisk instance.
- `dpkg-buildpackage -us -uc -b -d` verifies Debian packaging when the needed Asterisk/pjproject headers are available.

## Coding Style & Naming Conventions

Use existing Asterisk C style: tabs for indentation, braces on control blocks, `snake_case` functions, and `res_pjsip_cisco_<feature>` module names. Keep behavior gated by the `[name] type=cisco` sorcery object unless the module is intentionally global, as with PIDF body generation. Add comments only where they explain Asterisk/PJSIP lifecycle constraints, ABI assumptions, or non-obvious Cisco firmware behavior.

## Testing Guidelines

`make tests` runs two flavours from `tests/unit/`:
- **smoke** — `xmllint` the generated `$(DOC_XML)` plus `readelf`-based symbol checks (`__mod_info`, `load_module`, `unload_module`) for each built `.so` under `$(MODULE_BUILD_DIR)`. Catches the failure modes that surface as "module silently refuses to register at startup".
- **unit** — standalone C programs with `assert()`s. `test_string_utils` (pjlib-linked) pins the pjlib primitives we depend on. `test_xml_bodies` (libxml2-linked) snprintfs every sprintf'd XML body template (`{bulkupdate,remotecc}_bodies.h`) with bench-realistic substitutions and parses the result, catching format-string typos at CI time rather than on the bench. CI runs the libxml2 half on every PR; the pjproject-linked half is local-only.

For behaviour changes that touch live SIP flows, follow `tests/README.md`: parallel PJSIP TCP transport, capture REGISTER/REFER/NOTIFY traffic, verify on a real phone. Document tested Asterisk, pjproject, phone model, and firmware version in PRs.

## Commit & Pull Request Guidelines

Recent history uses short, scope-prefixed commits such as `ci: ...`, `debian: ...`, `pidf: ...`, and `bulkupdate: ...`. Keep subjects imperative and focused.

Pull requests should include the problem, implementation summary, build/test commands run, and any manual phone or packet-capture evidence. Link issues when applicable. Update `README.md`, `ARCHITECTURE.md`, samples, and generated docs when behavior or configuration changes.

## Security & Configuration Tips

Build against headers matching the running Asterisk binary; mismatched Asterisk or pjproject headers can compile successfully but crash at runtime. Do not commit site-specific SIP secrets, phone SEP files, packet captures with credentials, or private network details.
