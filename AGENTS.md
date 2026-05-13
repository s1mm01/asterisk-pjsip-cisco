# Repository Guidelines

## Project Structure & Module Organization

This repository builds out-of-tree Asterisk `res_pjsip_cisco_*` shared modules for Cisco Enterprise SIP firmware support.

- `res/` contains the C modules, shared helper header (`cisco_endpoint.h`), module export files, and generated `.so` build outputs.
- `doc/` contains generated Asterisk XML documentation (`res_pjsip_cisco-en_US.xml`).
- `conf-samples/` contains sample `pjsip.conf` Cisco sections.
- `debian/` contains Debian packaging metadata and `debian/rules`.
- `tests/README.md` documents the manual test bench against a real Cisco phone.
- `.github/workflows/ci.yml` builds against the latest Asterisk 20.x, 22.x, and 23.x releases.

## Build, Test, and Development Commands

- `make` builds all modules and regenerates the XML documentation. Use `PJPROJECT_DIR=/path/to/asterisk-src` when building against a source-built Asterisk.
- `make clean` removes module objects, `.so` files, and generated XML.
- `sudo make install` installs modules, docs, and sample config.
- `sudo make uninstall` removes installed project files.
- `sudo make check` reports whether each Cisco module is loaded in a running Asterisk instance.
- `dpkg-buildpackage -us -uc -b -d` verifies Debian packaging when the needed Asterisk/pjproject headers are available.

## Coding Style & Naming Conventions

Use existing Asterisk C style: tabs for indentation, braces on control blocks, `snake_case` functions, and `res_pjsip_cisco_<feature>` module names. Keep behavior gated by the `[name] type=cisco` sorcery object unless the module is intentionally global, as with PIDF body generation. Add comments only where they explain Asterisk/PJSIP lifecycle constraints, ABI assumptions, or non-obvious Cisco firmware behavior.

## Testing Guidelines

There is no unit-test framework in this repository. CI validates compilation, shared-object generation, XML validity, and Debian package construction. For behavior changes, follow `tests/README.md`: use a parallel PJSIP TCP transport, capture REGISTER/REFER/NOTIFY traffic, and verify BLF/service-control behavior on a real phone. Document tested Asterisk, pjproject, phone model, and firmware version in PRs.

## Commit & Pull Request Guidelines

Recent history uses short, scope-prefixed commits such as `ci: ...`, `debian: ...`, `pidf: ...`, and `bulkupdate: ...`. Keep subjects imperative and focused.

Pull requests should include the problem, implementation summary, build/test commands run, and any manual phone or packet-capture evidence. Link issues when applicable. Update `README.md`, `ARCHITECTURE.md`, samples, and generated docs when behavior or configuration changes.

## Security & Configuration Tips

Build against headers matching the running Asterisk binary; mismatched Asterisk or pjproject headers can compile successfully but crash at runtime. Do not commit site-specific SIP secrets, phone SEP files, packet captures with credentials, or private network details.
