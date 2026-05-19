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
override CFLAGS      += -fPIC -Wall -Werror -Wno-unused-function \
                        -Wstrict-prototypes -Wmissing-prototypes \
                        -Wmissing-declarations -Wold-style-definition \
                        -Wnested-externs \
                        -Wshadow -Wpointer-arith -Wjump-misses-init \
                        -Wlogical-op -Wduplicated-cond -Wduplicated-branches \
                        -Wvla -Wformat=2 \
                        -I$(ASTERISK_INCLUDE_DIR) \
                        -Iinclude \
                        $(PJPROJECT_CFLAGS)
LDFLAGS              ?=
override LDFLAGS     += -shared

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

.PHONY: all clean install uninstall doc check check-headers help tests

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
