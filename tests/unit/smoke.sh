#!/usr/bin/env bash
# Build-artefact smoke test. Sanity-checks the files Asterisk's loader
# will actually look at:
#
#   1. $DOC_XML is well-formed XML (the Makefile's
#      awk/sed harvester can produce invalid output if a /*** DOCUMENTATION
#      ***/ block in any source file is malformed; Asterisk silently
#      refuses to register a sorcery type whose <configObject> doesn't
#      parse).
#
#   2. Each .so in $MODULE_BUILD_DIR has __mod_info + load_module + unload_module
#      symbols. The loader dlopens each .so RTLD_LOCAL and dlsym's
#      __mod_info to find AST_MODULE_INFO; missing symbols here mean
#      "module silently refuses to register at startup".
#
# Exits non-zero on the first failure, with a clear diagnostic.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"

fail() { printf "FAIL: %s\n" "$*" >&2; exit 1; }

OBJ_DIR="${OBJ_DIR:-obj}"
MODULE_BUILD_DIR="${MODULE_BUILD_DIR:-$OBJ_DIR}"
DOC_XML="${DOC_XML:-$OBJ_DIR/doc/res_pjsip_cisco-en_US.xml}"

# 1. XML validity.
if [[ ! -f "$DOC_XML" ]]; then
	fail "$DOC_XML not present (run 'make' first)"
fi
if ! command -v xmllint >/dev/null 2>&1; then
	fail "xmllint not installed (apt install libxml2-utils)"
fi
if ! xmllint --noout "$DOC_XML" 2>/dev/null; then
	# Re-run with stderr so the user sees the line number.
	xmllint --noout "$DOC_XML"
	fail "$DOC_XML failed xmllint"
fi
printf "  OK   %s\n" "$DOC_XML"

# 2. Each .so has the loader-required symbols.
SOS=("$MODULE_BUILD_DIR"/res_pjsip_cisco_*.so)
if [[ ${#SOS[@]} -eq 0 || ! -f "${SOS[0]}" ]]; then
	fail "no .so files in $MODULE_BUILD_DIR (run 'make' first)"
fi

for so in "${SOS[@]}"; do
	# readelf -s prints both static and dynamic symbol tables. The
	# Asterisk loader resolves __mod_info via the .symtab (not .dynsym),
	# so plain readelf -s captures both.
	syms=$(readelf -s "$so")
	for needed in __mod_info load_module unload_module; do
		if ! grep -q " $needed\$" <<< "$syms"; then
			fail "$so: missing symbol '$needed'"
		fi
	done
	printf "  OK   %s\n" "$so"
done

printf "Smoke test: %d .so + 1 XML passed.\n" "${#SOS[@]}"
