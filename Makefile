# asterisk-pjsip-cisco
#
# Out-of-tree shared modules that add Cisco Enterprise SIP firmware
# support to chan_pjsip without patching Asterisk core.
#
# Build:   make
# Install: sudo make install
# Remove:  sudo make uninstall
#
# Source layout (Asterisk house style):
#   include/cisco/<X>.h               public cross-.so headers (cisco_*
#                                     symbols exported from
#                                     res_pjsip_cisco_endpoint.so)
#   res/res_pjsip_cisco_<feature>.c   one entry per module (matches .so)
#   res/res_pjsip_cisco_<feature>.exports   linker --version-script
#   res/cisco_<feature>/<X>.c         helpers compiled into the matching .so
#   res/cisco_<feature>/include/<X>_private.h   internal header for this .so
#
# Build output (default OBJ_DIR=obj):
#   $(OBJ_DIR)/res_pjsip_cisco_<feature>.o        entry object
#   $(OBJ_DIR)/res_pjsip_cisco_<feature>/*.o      helper objects
#   $(OBJ_DIR)/res_pjsip_cisco_<feature>.so       linked module
#   $(OBJ_DIR)/doc/res_pjsip_cisco-en_US.xml      generated doc XML

# --------------------------------------------------------------------
# Configurable paths. Override on the command line or in environment
# if your distro lays things out differently:
#     make ASTERISK_MODULES_DIR=/opt/asterisk/lib/modules
#
# ASTERISK_MODULES_DIR resolution:
#   1. Explicit override on the command line / environment.
#   2. astmoddir from /etc/asterisk/asterisk.conf if the file exists.
#      That's the authoritative source for where the running asterisk
#      looks; Debian/Ubuntu's asterisk-config sets it to the multiarch
#      path (/usr/lib/<triple>/asterisk/modules), upstream's default
#      points at /usr/lib/asterisk/modules.
#   3. Upstream default /usr/lib/asterisk/modules.
# Resolution order ensures `sudo make install` lands modules where
# the local asterisk binary will actually look for them, regardless
# of distro packaging convention.
# --------------------------------------------------------------------

ASTERISK_CONF        ?= /etc/asterisk/asterisk.conf

# Helper: extract one [directories] entry from asterisk.conf.
# Tolerates both `key => value` (upstream / Debian style) and a plain
# `key = value` for hand-edited configs.
ast_conf_dir = $(shell sed -nE \
    's|^[[:space:]]*$(1)[[:space:]]*=>?[[:space:]]*([^[:space:];]+).*|\1|p' \
    $(ASTERISK_CONF) | head -1)

ifeq ($(strip $(ASTERISK_MODULES_DIR)),)
ifneq ($(wildcard $(ASTERISK_CONF)),)
ASTERISK_MODULES_DIR := $(call ast_conf_dir,astmoddir)
endif
endif
ASTERISK_MODULES_DIR ?= /usr/lib/asterisk/modules

# Doc XML lives under astdatadir/documentation per asterisk's xmldoc
# loader. Upstream default for astdatadir is /var/lib/asterisk;
# Debian's asterisk-config moves it to /usr/share/asterisk (FHS).
ifeq ($(strip $(ASTERISK_DOC_DIR)),)
ifneq ($(wildcard $(ASTERISK_CONF)),)
ASTERISK_DATA_DIR    := $(call ast_conf_dir,astdatadir)
ifneq ($(strip $(ASTERISK_DATA_DIR)),)
ASTERISK_DOC_DIR     := $(ASTERISK_DATA_DIR)/documentation
endif
endif
endif
ASTERISK_DOC_DIR     ?= /var/lib/asterisk/documentation

ASTERISK_SAMPLE_DIR  ?= /usr/share/doc/asterisk-pjsip-cisco

# Asterisk headers. We MUST build against the same headers the running
# asterisk was built with, otherwise struct layouts diverge and any
# code that touches struct ast_sip_endpoint deeper than its first few
# fields will SEGV at runtime.
#
# ASTERISK_SRC_DIR — path to an asterisk source tree (e.g.
# /path/to/asterisk-22.9.0). When set, the Makefile derives:
#   * asterisk headers from <dir>/include/asterisk/
#   * bundled-pjproject headers from <dir>/third-party/pjproject/source/
#   * asterisk's pjproject patch overlay (config_site.h +
#     asterisk_malloc_debug.h) from <dir>/third-party/pjproject/patches/
# all in one go. This is the only mode that guarantees struct
# compatibility with the runtime asterisk (see CLAUDE.md's
# "header-mismatch trap" essay).
#
# PJPROJECT_DIR is the deprecated name for this variable — kept for
# backward compatibility with existing scripts. The original name was
# misleading; it always pointed at an asterisk source tree, not a
# pjproject one.
ifeq ($(strip $(ASTERISK_SRC_DIR)),)
ifneq ($(strip $(PJPROJECT_DIR)),)
ASTERISK_SRC_DIR := $(PJPROJECT_DIR)
$(info NOTE: PJPROJECT_DIR is deprecated; use ASTERISK_SRC_DIR (the variable points at an asterisk source tree, not a pjproject one).)
endif
endif

# Resolution order:
#   1. ASTERISK_INCLUDE_DIR explicitly set on the command line.
#   2. ASTERISK_SRC_DIR's include/ subdir (the source tree's headers,
#      when ASTERISK_SRC_DIR points at the asterisk source root).
#   3. /usr/include (asterisk-dev package).
#
# Distros that build asterisk from source frequently end up with a
# mismatch between asterisk-dev (stale) and the locally-built binary
# (current). Pinning to ASTERISK_SRC_DIR/include avoids that trap.
ifeq ($(strip $(ASTERISK_INCLUDE_DIR)),)
ifneq ($(strip $(ASTERISK_SRC_DIR)),)
ifneq ($(wildcard $(ASTERISK_SRC_DIR)/include/asterisk/res_pjsip.h),)
ASTERISK_INCLUDE_DIR := $(ASTERISK_SRC_DIR)/include
endif
endif
endif
ASTERISK_INCLUDE_DIR ?= /usr/include

DESTDIR              ?=

# Build output roots. Override individually to split outputs across
# directories (e.g. distro packaging that wants .so and .xml staged
# under different dh_install prefixes). Defaults chain through OBJ_DIR
# so a single override moves everything together:
#
#   OBJ_DIR          — overall build-output root.
#   MODULE_BUILD_DIR — where the .o + .so files land. Defaults to OBJ_DIR.
#   DOC_BUILD_DIR    — where the generated XML lands. Defaults to
#                      OBJ_DIR/doc.
#   DOC_XML          — full path of the generated XML. Defaults to
#                      DOC_BUILD_DIR/res_pjsip_cisco-en_US.xml (set
#                      further below, after the doc-dir is fixed).
OBJ_DIR              ?= obj
MODULE_BUILD_DIR     ?= $(OBJ_DIR)
DOC_BUILD_DIR        ?= $(OBJ_DIR)/doc

# pjproject headers. Three resolution paths, in order:
#   1. pkg-config (Debian's libpjproject-dev provides this)
#   2. PJPROJECT_INCLUDE override on the command line, e.g.
#      make PJPROJECT_INCLUDE="-I/path/to/pjproject/source/pjsip/include ..."
#   3. ASTERISK_SRC_DIR pointing at an Asterisk source tree containing
#      a bundled pjproject under third-party/, e.g.
#      make ASTERISK_SRC_DIR=/path/to/asterisk-22.9.0
PJPROJECT_CFLAGS     := $(shell pkg-config --cflags libpjproject 2>/dev/null)
ifeq ($(strip $(PJPROJECT_CFLAGS)),)
PJPROJECT_CFLAGS     := $(shell pkg-config --cflags pjproject 2>/dev/null)
endif
ifeq ($(strip $(PJPROJECT_CFLAGS)),)
ifneq ($(strip $(PJPROJECT_INCLUDE)),)
PJPROJECT_CFLAGS     := $(PJPROJECT_INCLUDE)
endif
endif
ifeq ($(strip $(PJPROJECT_CFLAGS)),)
ifneq ($(strip $(ASTERISK_SRC_DIR)),)
PJPROJECT_CFLAGS     := -DPJ_AUTOCONF=1 \
                        -I$(ASTERISK_SRC_DIR)/third-party/pjproject/source/pjlib/include \
                        -I$(ASTERISK_SRC_DIR)/third-party/pjproject/source/pjlib-util/include \
                        -I$(ASTERISK_SRC_DIR)/third-party/pjproject/source/pjnath/include \
                        -I$(ASTERISK_SRC_DIR)/third-party/pjproject/source/pjmedia/include \
                        -I$(ASTERISK_SRC_DIR)/third-party/pjproject/source/pjsip/include
endif
endif

# --------------------------------------------------------------------
# Apply asterisk's pjproject patches (config_site.h + the
# asterisk_malloc_debug.h it includes).
#
# Stock pjproject ships with an empty config_site.h. Asterisk's
# bundled-pjproject build overlays a customised config_site.h via the
# pattern rule in third-party/pjproject/Makefile:
#
#     source/pjlib/include/pj/%.h: patches/%.h
#
# That file redefines several layout-critical macros — most notably
#
#     PJSIP_MAX_PKT_LEN  65535   (default 2000)
#     PJSIP_MAX_MODULE   38      (default 32)
#     PJMEDIA_MAX_SDP_FMT  72/64 (default 32)
#
# which size arrays *inside* pjsip_rx_data, pjmedia_sdp_media, and
# pjsip_endpoint. Compiling our modules against stock pjproject
# headers while loading them into an asterisk built with the
# patched headers produces struct offsets that disagree by tens of
# kilobytes — our hooks read rdata->msg_info.msg at the wrong
# address and silently observe NULL. CLAUDE.md's "header-mismatch
# trap" in its worst form (no crash, no warning, just functional
# break).
#
# Mirror asterisk's own rule so ASTERISK_SRC_DIR-mode builds are
# automatically struct-compatible with the runtime asterisk,
# regardless of whether the user has run asterisk's own build yet.
# --------------------------------------------------------------------

ifneq ($(strip $(ASTERISK_SRC_DIR)),)
ASTERISK_PJ_PATCHES_DIR := $(ASTERISK_SRC_DIR)/third-party/pjproject/patches
ASTERISK_PJ_SOURCE_DIR  := $(ASTERISK_SRC_DIR)/third-party/pjproject/source/pjlib/include/pj

# Apply every .h overlay asterisk ships in third-party/pjproject/patches/
# — typically config_site.h and asterisk_malloc_debug.h, but discover
# the set rather than enumerate it so an asterisk that adds another
# overlay header gets picked up automatically. Mirrors asterisk's own
# pattern rule (third-party/pjproject/Makefile):
#
#   source/pjlib/include/pj/%.h: patches/%.h
ASTERISK_PJ_APPLIED_PATCHES := \
    $(patsubst $(ASTERISK_PJ_PATCHES_DIR)/%,$(ASTERISK_PJ_SOURCE_DIR)/%,\
        $(wildcard $(ASTERISK_PJ_PATCHES_DIR)/*.h))

# FORCE prerequisite makes the overlay copy fire on every make
# invocation. The natural timestamp check (dest older than source =
# rebuild) is unreliable here: an empty stub config_site.h is often
# pre-created with a fresh mtime so pjproject's ./configure has
# something to include, which leaves the overlay erroneously
# unapplied on subsequent make. The copy is two ~3KB files; the cost
# is negligible, and guaranteeing correctness is worth more.
$(ASTERISK_PJ_SOURCE_DIR)/%.h: $(ASTERISK_PJ_PATCHES_DIR)/%.h FORCE
	cp -f $< $@

.PHONY: FORCE
FORCE:
endif

# --------------------------------------------------------------------
# Build flags. We inherit user CFLAGS/LDFLAGS for distro packaging.
# --------------------------------------------------------------------

CC                   ?= cc
CFLAGS               ?= -O2 -g
# Hardening flags on top of -Wall.
#
# Prototype / declaration hygiene:
#   -Wstrict-prototypes      — flag K&R f() declarations (must be f(void)).
#   -Wmissing-prototypes     — flag non-static functions with no prior
#                              declaration in a header.
#   -Wmissing-declarations   — counterpart on the definition side: a
#                              non-static definition must match a prior
#                              declaration.
#   -Wold-style-definition   — flag K&R definitions f() instead of f(void).
#   -Wnested-externs         — disallow extern inside function bodies.
#
# Bug-class detectors:
#   -Wshadow                 — inner-scope shadowing of outer locals.
#   -Wpointer-arith          — arithmetic on void* / function pointers.
#   -Wjump-misses-init       — goto/switch that skips a local init.
#   -Wlogical-op             — suspicious || / && (constant operands,
#                              same operand on both sides).
#   -Wduplicated-cond        — `if (x) ... else if (x) ...` typos.
#   -Wduplicated-branches    — identical then/else bodies (copy-paste bugs).
#   -Wvla                    — variable-length arrays (Asterisk style
#                              prefers fixed-size buffers).
#   -Wformat=2               — stricter format-string checking on top of
#                              -Wall's -Wformat (catches non-literal
#                              format strings and %n misuse).
#   -Wconversion             — implicit conversions that may change a
#   -Wsign-conversion          value or its sign (size_t<->int, the
#                              snprintf/ast_db_get width mismatches,
#                              pj_ssize_t lengths, etc). The nearest C
#                              gets to type-safety enforcement.
#
# Third-party headers are included with -isystem, not -I. Asterisk and
# pjproject macros/inline functions trip -Wconversion hundreds of times
# (ao2 containers, pj_str_t arithmetic, …) and we can't fix code we
# don't own. -isystem marks those trees as system headers, so GCC
# suppresses warnings originating inside them (and inside their macro
# expansions) while still flagging conversions written in OUR sources.
# Our own headers stay on -Iinclude so they remain in scope for warnings.
PJPROJECT_ISYSTEM_CFLAGS := $(patsubst -I%,-isystem %,$(PJPROJECT_CFLAGS))

# Opt-in instrumentation hooks. Empty for a normal build; the
# `sanitize-ub` target sets them to build a UBSan variant. Appended last
# so they sit after the warning/-Werror set without disturbing it.
SANITIZE_CFLAGS  ?=
SANITIZE_LDFLAGS ?=

override CFLAGS      += -fPIC -Wall -Werror -Wno-unused-function \
                        -Wstrict-prototypes -Wmissing-prototypes \
                        -Wmissing-declarations -Wold-style-definition \
                        -Wnested-externs \
                        -Wshadow -Wpointer-arith -Wjump-misses-init \
                        -Wlogical-op -Wduplicated-cond -Wduplicated-branches \
                        -Wvla -Wformat=2 \
                        -Wconversion -Wsign-conversion \
                        -isystem $(ASTERISK_INCLUDE_DIR) \
                        -Iinclude \
                        $(PJPROJECT_ISYSTEM_CFLAGS) \
                        $(SANITIZE_CFLAGS)
LDFLAGS              ?=
override LDFLAGS     += -shared $(SANITIZE_LDFLAGS)

# --------------------------------------------------------------------
# Modules. Each MODULE has a short feature name; the .so install name
# is res_pjsip_cisco_<feature>.so. Multi-file modules list their
# helper .c files (basenames, without extension) in
# <feature>_HELPERS; single-file modules leave that empty.
# --------------------------------------------------------------------

MODULES := \
    endpoint \
    pidf_body_generator \
    register_optionsind \
    bulkupdate \
    unsolicited_blf \
    service_control \
    feature_events \
    call_extras \
    conference \
    remotecc

endpoint_HELPERS         := endpoint orig_host rdata refer register session device
bulkupdate_HELPERS       := cli func
call_extras_HELPERS      := video
conference_HELPERS       := state list confrn
feature_events_HELPERS   := dnd mac
remotecc_HELPERS         := mcid park record
unsolicited_blf_HELPERS  := pidf

# Single-file modules — declared empty for completeness.
pidf_body_generator_HELPERS  :=
register_optionsind_HELPERS  :=
service_control_HELPERS      :=

# --------------------------------------------------------------------
# Per-module object lists and link rules, generated via eval.
#
# For each <m> in MODULES:
#   $(m)_OBJ          = $(MODULE_BUILD_DIR)/res_pjsip_cisco_<m>.o          (entry)
#   $(m)_HELPER_OBJS  = $(MODULE_BUILD_DIR)/res_pjsip_cisco_<m>/<x>.o ...  (helpers)
#   $(m)_SO           = $(MODULE_BUILD_DIR)/res_pjsip_cisco_<m>.so         (linked .so)
# --------------------------------------------------------------------

define MODULE_VARS_template
$(1)_OBJ         := $(MODULE_BUILD_DIR)/res_pjsip_cisco_$(1).o
$(1)_HELPER_OBJS := $$(addprefix $(MODULE_BUILD_DIR)/res_pjsip_cisco_$(1)/,$$(addsuffix .o,$$($(1)_HELPERS)))
$(1)_SO          := $(MODULE_BUILD_DIR)/res_pjsip_cisco_$(1).so
endef
$(foreach m,$(MODULES),$(eval $(call MODULE_VARS_template,$(m))))

ALL_OBJS := $(foreach m,$(MODULES),$($(m)_OBJ) $($(m)_HELPER_OBJS))
ALL_SOS  := $(foreach m,$(MODULES),$($(m)_SO))

DOC_XML  ?= $(DOC_BUILD_DIR)/res_pjsip_cisco-en_US.xml

.PHONY: all clean install uninstall doc check check-headers help tests \
        compile-commands tidy cppcheck sanitize-ub

# `all` is the default goal even when targets earlier in the file
# (e.g. the FORCE phony in the pjproject patches block) might
# otherwise win make's first-rule-defines-default-goal heuristic.
.DEFAULT_GOAL := all

all: check-headers $(ASTERISK_PJ_APPLIED_PATCHES) $(ALL_SOS) $(DOC_XML)

# --------------------------------------------------------------------
# Tests: build-artefact smoke checks + pjlib-linked unit tests. See
# tests/unit/README.md for what's covered and how to add more.
# --------------------------------------------------------------------

tests: all
	$(MAKE) -C tests/unit all \
	    ASTERISK_SRC_DIR='$(ASTERISK_SRC_DIR)' \
	    OBJ_DIR='$(OBJ_DIR)' \
	    MODULE_BUILD_DIR='$(MODULE_BUILD_DIR)' \
	    DOC_XML='$(DOC_XML)'

# --------------------------------------------------------------------
# Per-module compile + link rules.
#
# Entry rule  (per module): $(MODULE_BUILD_DIR)/res_pjsip_cisco_<m>.o ← res/res_pjsip_cisco_<m>.c
# Helper rule (per module): $(MODULE_BUILD_DIR)/res_pjsip_cisco_<m>/%.o ← res/cisco_<m>/%.c
# Link rule   (per module): $(MODULE_BUILD_DIR)/res_pjsip_cisco_<m>.so ← entry .o + helpers + exports
#
# All three are generated by eval-ing the templates below so each
# module gets its own correctly-named -DAST_MODULE / -DAST_MODULE_SELF_SYM.
#
# All .c files depend on every public header in include/cisco/ and on
# the module's own internal header (if any). Header-change rebuilds
# are rare; universal rebuild is the right trade-off.
# --------------------------------------------------------------------

ALL_PUBLIC_HDRS := $(wildcard include/cisco/*.h)

define MODULE_RULES_template
# Entry compile
$(MODULE_BUILD_DIR)/res_pjsip_cisco_$(1).o: res/res_pjsip_cisco_$(1).c $$(ALL_PUBLIC_HDRS) $$(wildcard res/cisco_$(1)/include/*.h)
	@mkdir -p $$(@D)
	$$(CC) $$(CFLAGS) \
	    -Ires/cisco_$(1)/include \
	    -DAST_MODULE_SELF_SYM=__internal_res_pjsip_cisco_$(1)_self \
	    -DAST_MODULE=\"res_pjsip_cisco_$(1)\" \
	    -c $$< -o $$@

# Helper compile (matches any helper in this module's subdir).
$(MODULE_BUILD_DIR)/res_pjsip_cisco_$(1)/%.o: res/cisco_$(1)/%.c $$(ALL_PUBLIC_HDRS) $$(wildcard res/cisco_$(1)/include/*.h)
	@mkdir -p $$(@D)
	$$(CC) $$(CFLAGS) \
	    -Ires/cisco_$(1)/include \
	    -DAST_MODULE_SELF_SYM=__internal_res_pjsip_cisco_$(1)_self \
	    -DAST_MODULE=\"res_pjsip_cisco_$(1)\" \
	    -c $$< -o $$@

# Link: entry .o + helper .o(s) + .exports version-script.
$$($(1)_SO): $$($(1)_OBJ) $$($(1)_HELPER_OBJS) res/res_pjsip_cisco_$(1).exports
	$$(CC) $$(LDFLAGS) \
	    -Wl,--version-script=res/res_pjsip_cisco_$(1).exports \
	    -o $$@ \
	    $$(filter %.o,$$^)
endef
$(foreach m,$(MODULES),$(eval $(call MODULE_RULES_template,$(m))))

# --------------------------------------------------------------------
# XML documentation extraction.
#
# Asterisk's strict sorcery validator rejects field registrations
# unless a matching <configObject> exists in a documentation XML file
# under $ASTERISK_DOC_DIR. We extract every /*** DOCUMENTATION ... ***/
# block from our sources and assemble them into a single XML file.
# --------------------------------------------------------------------

ALL_SOURCES := $(wildcard res/*.c) $(wildcard res/cisco_*/*.c)

doc: $(DOC_XML)

$(DOC_XML): $(ALL_SOURCES)
	@mkdir -p $(@D)
	@( \
	  echo '<?xml version="1.0" encoding="UTF-8"?>'; \
	  echo '<docs xmlns:xi="http://www.w3.org/2001/XInclude">'; \
	  for f in $(ALL_SOURCES); do \
	    awk '/\/\*\*\* *DOCUMENTATION/,/\*\*\*\//' $$f \
	      | sed -e 's@/\*\*\* *DOCUMENTATION@@' -e 's@\*\*\*/@@'; \
	  done; \
	  echo '</docs>'; \
	) > $@

# --------------------------------------------------------------------
# Static analysis (clang-tidy).
#
# clang-tidy reasons about each translation unit using the exact -I / -D
# flags it was compiled with — which, for this project, is load-bearing:
# the pjproject config_site.h overlay shifts struct offsets inside
# pjsip_rx_data / pjmedia_sdp_media / pjsip_endpoint (CLAUDE.md's
# "header-mismatch trap"). So rather than hand-maintain a flag list, we
# record a real build with Bear (https://github.com/rizsotto/Bear) into
# compile_commands.json and let clang-tidy read that.
#
# Because the flags come from the build, `make tidy` is only as correct
# as the build it wraps: invoke it the same way you build — ideally
#   make tidy ASTERISK_SRC_DIR=/path/to/asterisk-source
# so the overlay is applied and clang-tidy sees the runtime ABI.
#
# Advisory: the checks live in .clang-tidy and never gate the build
# (the -Werror warning set in CFLAGS is the blocking line). Use it to
# surface null-deref / leak / refcount-shaped bugs the compiler can't.
# --------------------------------------------------------------------

COMPILE_COMMANDS ?= compile_commands.json

# Regenerate the compile DB by recording a clean build. FORCE-style:
# we always rebuild it rather than timestamp-tracking every source,
# since a stale DB silently analyses the wrong flags. `make clean`
# first so bear captures every TU (incremental builds skip up-to-date
# objects, leaving them out of the DB).
compile-commands:
	@command -v bear >/dev/null 2>&1 || { \
	    echo "bear not found. Install with: sudo apt install bear" >&2; \
	    exit 1; }
	$(MAKE) clean
	bear --output $(COMPILE_COMMANDS) -- $(MAKE) all

tidy: compile-commands
	@command -v clang-tidy >/dev/null 2>&1 || { \
	    echo "clang-tidy not found. Install with: sudo apt install clang-tidy" >&2; \
	    exit 1; }
	clang-tidy -p . $(ALL_SOURCES)

# --------------------------------------------------------------------
# Cppcheck — a second static-analysis opinion. Different engine to
# clang-tidy: it catches things clang's analyzer misses (and vice
# versa) and runs without compiling.
#
# We feed it the same include set the build uses, but as -I (not the
# build's -isystem): cppcheck SKIPS -isystem headers, so asterisk's
# AST_DECLARE_STRING_FIELDS / ao2 macros would go unresolved and it
# would emit spurious syntaxError / unknownMacro. With them as -I the
# macros expand and our sources parse cleanly; we then suppress findings
# located in the third-party trees so only our code is reported.
#
# Pass ASTERISK_SRC_DIR the way you build:
#   make cppcheck ASTERISK_SRC_DIR=/path/to/asterisk-source
# --------------------------------------------------------------------

# Header dirs whose findings we silence (asterisk + whatever pjproject
# dirs PJPROJECT_CFLAGS resolved to — we don't own that code).
CPPCHECK_INC_DIRS := $(ASTERISK_INCLUDE_DIR) \
                     $(patsubst -I%,%,$(filter -I%,$(PJPROJECT_CFLAGS)))

CPPCHECK_FLAGS ?= --enable=warning,performance,portability \
                  --check-level=exhaustive --inline-suppr --quiet \
                  --error-exitcode=1 \
                  --suppress=missingIncludeSystem \
                  --suppress=unmatchedSuppression

cppcheck:
	@command -v cppcheck >/dev/null 2>&1 || { \
	    echo "cppcheck not found. Install with: sudo apt install cppcheck" >&2; \
	    exit 1; }
	cppcheck $(CPPCHECK_FLAGS) \
	    $(addprefix --suppress=*:,$(addsuffix /*,$(CPPCHECK_INC_DIRS))) \
	    -DAST_MODULE_SELF_SYM=cppcheck_stub -DAST_MODULE=\"cppcheck\" \
	    -Iinclude -I$(ASTERISK_INCLUDE_DIR) $(PJPROJECT_CFLAGS) \
	    res/

# --------------------------------------------------------------------
# UBSan build variant. Compiles the modules with
# -fsanitize=undefined (runtime mode — the .so gains a NEEDED on
# libubsan, which asterisk's dlopen pulls in), so undefined behaviour in
# OUR code — signed overflow, bad shifts, null/misaligned deref,
# out-of-range enum loads, the runtime side of the conversions
# -Wconversion flags statically — is reported with a file:line message
# at runtime. Unlike ASan, UBSan needs no early init, so it drops into a
# normally-launched asterisk with no LD_PRELOAD.
#
# Output goes to a SEPARATE tree ($(OBJ_DIR)/sanitize-ub) so it never
# overwrites the production .so — a sanitized module must not be
# `make install`ed onto a real PBX by accident. To use it:
#
#   make sanitize-ub ASTERISK_SRC_DIR=/path/to/asterisk-source
#   sudo install -m0644 $(OBJ_DIR)/sanitize-ub/res_pjsip_cisco_*.so \
#       <asterisk modules dir>
#   # then run asterisk with, e.g.:
#   #   UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=0:log_path=/tmp/ubsan
#   # exercise via the real phone / tests/sipp, then read /tmp/ubsan.*
#
# halt_on_error=0 keeps the PBX alive and logs every hit; set =1 to
# abort on the first. print_stacktrace needs llvm-symbolizer or addr2line
# on PATH for symbolised frames. Build with a lower -O for cleaner
# traces if needed, e.g. CFLAGS='-O1 -g'.
# --------------------------------------------------------------------

sanitize-ub:
	$(MAKE) all \
	    OBJ_DIR='$(OBJ_DIR)/sanitize-ub' \
	    SANITIZE_CFLAGS='-fsanitize=undefined -fno-omit-frame-pointer' \
	    SANITIZE_LDFLAGS='-fsanitize=undefined'
	@echo
	@echo "UBSan modules built in $(OBJ_DIR)/sanitize-ub/."
	@echo "Install them over the production .so on a TEST asterisk only,"
	@echo "then run with UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=0"
	@echo "and exercise the call flows. See the sanitize-ub comment in the"
	@echo "Makefile for the full recipe."

# --------------------------------------------------------------------
# Sanity check that asterisk-dev headers are installed.
# --------------------------------------------------------------------

check-headers:
	@test -f $(ASTERISK_INCLUDE_DIR)/asterisk/res_pjsip.h || ( \
	    echo "asterisk-dev headers not found at $(ASTERISK_INCLUDE_DIR)/asterisk/" >&2 ; \
	    echo "Install with: sudo apt install asterisk-dev libpjproject-dev" >&2 ; \
	    exit 1 )
	@if [ -z "$(strip $(PJPROJECT_CFLAGS))" ]; then \
	    echo "pjproject headers not found. One of:" >&2 ; \
	    echo "  sudo apt install libpjproject-dev               (preferred)" >&2 ; \
	    echo "  make ASTERISK_SRC_DIR=/path/to/asterisk-source  (uses bundled headers)" >&2 ; \
	    exit 1 ; \
	fi
	@if [ -z "$(strip $(ASTERISK_SRC_DIR))" ]; then \
	    echo "" >&2 ; \
	    echo "WARNING: building without ASTERISK_SRC_DIR." >&2 ; \
	    echo "  The pjproject headers you've supplied are not being" >&2 ; \
	    echo "  overlaid with asterisk's third-party/pjproject/patches/" >&2 ; \
	    echo "  config_site.h. If the runtime asterisk was built with" >&2 ; \
	    echo "  the patched config (every Debian/Ubuntu apt asterisk is)," >&2 ; \
	    echo "  several pjsip / pjmedia struct layouts will disagree" >&2 ; \
	    echo "  with the binary. The modules will load and run, but" >&2 ; \
	    echo "  on_rx_request hooks observe NULL msg pointers and" >&2 ; \
	    echo "  silently no-op. See CLAUDE.md \"header-mismatch trap\"." >&2 ; \
	    echo "  Recommended: rebuild with ASTERISK_SRC_DIR pointing at" >&2 ; \
	    echo "  the asterisk source tree your runtime asterisk was" >&2 ; \
	    echo "  built from (e.g. \`apt source asterisk\` for the apt" >&2 ; \
	    echo "  binary)." >&2 ; \
	    echo "" >&2 ; \
	fi

# --------------------------------------------------------------------
# Install / uninstall.
# --------------------------------------------------------------------

install: all
	install -d $(DESTDIR)$(ASTERISK_MODULES_DIR)
	install -m 0644 $(ALL_SOS) $(DESTDIR)$(ASTERISK_MODULES_DIR)/
	install -d $(DESTDIR)$(ASTERISK_DOC_DIR)
	install -m 0644 $(DOC_XML) $(DESTDIR)$(ASTERISK_DOC_DIR)/
	install -d $(DESTDIR)$(ASTERISK_SAMPLE_DIR)
	install -m 0644 conf-samples/* $(DESTDIR)$(ASTERISK_SAMPLE_DIR)/
	install -m 0644 README.md ARCHITECTURE.md $(DESTDIR)$(ASTERISK_SAMPLE_DIR)/
	@echo
	@echo "Installed. Next steps:"
	@echo "  1) cat $(ASTERISK_SAMPLE_DIR)/pjsip.conf.cisco-section.sample"
	@echo "     and add a [name] type=cisco section per Cisco endpoint"
	@echo "  2) sudo systemctl restart asterisk"
	@echo "     (no modules.conf changes required)"

uninstall:
	rm -f $(addprefix $(DESTDIR)$(ASTERISK_MODULES_DIR)/res_pjsip_cisco_,$(addsuffix .so,$(MODULES)))
	rm -f $(DESTDIR)$(ASTERISK_DOC_DIR)/res_pjsip_cisco-en_US.xml
	rm -rf $(DESTDIR)$(ASTERISK_SAMPLE_DIR)

clean:
	@case "$(strip $(OBJ_DIR))" in ""|"/"|"."|"..") \
	    echo "Refusing to clean unsafe OBJ_DIR='$(OBJ_DIR)'" >&2; \
	    exit 1;; \
	esac
	rm -rf $(OBJ_DIR)/

# --------------------------------------------------------------------
# Convenience: smoke-test load all modules in a running asterisk.
# --------------------------------------------------------------------

check:
	@failed=0; \
	for m in $(MODULES); do \
	  output=$$(asterisk -rx "module show like res_pjsip_cisco_$$m" 2>&1); \
	  if printf '%s\n' "$$output" | grep -q Running; then \
	    echo "  res_pjsip_cisco_$$m: Running"; \
	  else \
	    echo "  res_pjsip_cisco_$$m: NOT RUNNING"; \
	    failed=1; \
	  fi; \
	done; \
	exit $$failed

help:
	@echo "Targets:"
	@echo "  make            - build all modules and the doc XML"
	@echo "  make doc        - regenerate the doc XML only"
	@echo "  make install    - install modules, docs, and config samples"
	@echo "  make uninstall  - remove what 'install' put down"
	@echo "  make clean      - remove build artefacts (rm -rf $(OBJ_DIR)/)"
	@echo "  make check      - report which modules are loaded in a"
	@echo "                    running asterisk (run as root)"
	@echo "  make tidy       - run clang-tidy static analysis (needs bear"
	@echo "                    + clang-tidy; pass ASTERISK_SRC_DIR as you"
	@echo "                    would for a build)"
	@echo "  make cppcheck   - run cppcheck static analysis (second"
	@echo "                    opinion; needs cppcheck; pass ASTERISK_SRC_DIR)"
	@echo "  make sanitize-ub - build a UBSan variant into OBJ_DIR/sanitize-ub"
	@echo "                    for runtime UB checking on a TEST asterisk"
	@echo "  make compile-commands - regenerate compile_commands.json only"
	@echo
	@echo "Common overrides:"
	@echo "  ASTERISK_INCLUDE_DIR (default: $(ASTERISK_INCLUDE_DIR))"
	@echo "  ASTERISK_MODULES_DIR (default: $(ASTERISK_MODULES_DIR)"
	@echo "                        — read from astmoddir in $(ASTERISK_CONF) if present)"
	@echo "  ASTERISK_DOC_DIR     (default: $(ASTERISK_DOC_DIR))"
	@echo "  OBJ_DIR              (default: $(OBJ_DIR))"
	@echo "  MODULE_BUILD_DIR     (default: $(MODULE_BUILD_DIR))"
	@echo "  DOC_BUILD_DIR        (default: $(DOC_BUILD_DIR))"
	@echo "  DOC_XML              (default: $(DOC_XML))"
	@echo "  DESTDIR              (default: empty; for packaging)"
