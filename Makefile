# asterisk-pjsip-cisco
#
# Out-of-tree shared modules that add Cisco Enterprise SIP firmware
# support to chan_pjsip without patching Asterisk core.
#
# Build:   make
# Install: sudo make install
# Remove:  sudo make uninstall

# --------------------------------------------------------------------
# Configurable paths. Override on the command line or in environment
# if your distro lays things out differently:
#     make ASTERISK_MODULES_DIR=/opt/asterisk/lib/modules
# --------------------------------------------------------------------

ASTERISK_MODULES_DIR ?= /usr/lib/asterisk/modules
ASTERISK_DOC_DIR     ?= /var/lib/asterisk/documentation
ASTERISK_SAMPLE_DIR  ?= /usr/share/doc/asterisk-pjsip-cisco

# Asterisk headers. We MUST build against the same headers the running
# asterisk was built with, otherwise struct layouts diverge and any
# code that touches struct ast_sip_endpoint deeper than its first few
# fields will SEGV at runtime.
#
# Resolution order:
#   1. ASTERISK_INCLUDE_DIR explicitly set on the command line.
#   2. PJPROJECT_DIR's include/ subdir (the source tree's headers,
#      when PJPROJECT_DIR points at the asterisk source root).
#   3. /usr/include (asterisk-dev package).
#
# Distros that build asterisk from source frequently end up with a
# mismatch between asterisk-dev (stale) and the locally-built binary
# (current). Pinning to PJPROJECT_DIR/include avoids that trap.
ifeq ($(strip $(ASTERISK_INCLUDE_DIR)),)
ifneq ($(strip $(PJPROJECT_DIR)),)
ifneq ($(wildcard $(PJPROJECT_DIR)/include/asterisk/res_pjsip.h),)
ASTERISK_INCLUDE_DIR := $(PJPROJECT_DIR)/include
endif
endif
endif
ASTERISK_INCLUDE_DIR ?= /usr/include

DESTDIR              ?=

# pjproject headers. Three resolution paths, in order:
#   1. pkg-config (Debian's libpjproject-dev provides this)
#   2. PJPROJECT_INCLUDE override on the command line, e.g.
#      make PJPROJECT_INCLUDE="-I/path/to/pjproject/source/pjsip/include ..."
#   3. PJPROJECT_DIR pointing at an Asterisk source tree containing
#      a bundled pjproject under third-party/, e.g.
#      make PJPROJECT_DIR=/path/to/asterisk-22.9.0
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
ifneq ($(strip $(PJPROJECT_DIR)),)
PJPROJECT_CFLAGS     := -DPJ_AUTOCONF=1 \
                        -I$(PJPROJECT_DIR)/third-party/pjproject/source/pjlib/include \
                        -I$(PJPROJECT_DIR)/third-party/pjproject/source/pjlib-util/include \
                        -I$(PJPROJECT_DIR)/third-party/pjproject/source/pjnath/include \
                        -I$(PJPROJECT_DIR)/third-party/pjproject/source/pjmedia/include \
                        -I$(PJPROJECT_DIR)/third-party/pjproject/source/pjsip/include
endif
endif

# --------------------------------------------------------------------
# Build flags. We inherit user CFLAGS/LDFLAGS for distro packaging.
# --------------------------------------------------------------------

CC                   ?= cc
CFLAGS               ?= -O2 -g
override CFLAGS      += -fPIC -Wall -Werror -Wno-unused-function \
                        -I$(ASTERISK_INCLUDE_DIR) \
                        -Ires \
                        $(PJPROJECT_CFLAGS) \
                        -DAST_MODULE_SELF_SYM=__internal_$(basename $(notdir $<))_self
LDFLAGS              ?=
override LDFLAGS     += -shared

# --------------------------------------------------------------------
# Modules. Order matters at install/load time only when a module
# depends on the sorcery type registered by another, but our load_pri
# settings already handle that.
# --------------------------------------------------------------------

MODULES := \
    res_pjsip_cisco_endpoint \
    res_pjsip_cisco_pidf_body_generator \
    res_pjsip_cisco_register_optionsind \
    res_pjsip_cisco_bulkupdate \
    res_pjsip_cisco_unsolicited_blf \
    res_pjsip_cisco_service_control \
    res_pjsip_cisco_feature_events \
    res_pjsip_cisco_call_extras \
    res_pjsip_cisco_conference \
    res_pjsip_cisco_remotecc

OBJS := $(addprefix res/,$(addsuffix .o,$(MODULES)))
SOS  := $(addprefix res/,$(addsuffix .so,$(MODULES)))

DOC_XML := doc/res_pjsip_cisco-en_US.xml

.PHONY: all clean install uninstall doc check check-headers help

all: check-headers $(SOS) $(DOC_XML)

# --------------------------------------------------------------------
# Per-module build rule. AST_MODULE is the C symbol Asterisk uses to
# tag this module's logger lines; conventionally it matches the .so.
# --------------------------------------------------------------------

res/%.o: res/%.c res/cisco_endpoint.h
	$(CC) $(CFLAGS) -DAST_MODULE=\"$*\" -c $< -o $@

res/%.so: res/%.o
	$(CC) $(LDFLAGS) -o $@ $<

# --------------------------------------------------------------------
# XML documentation extraction.
#
# Asterisk's strict sorcery validator rejects field registrations
# unless a matching <configObject> exists in a documentation XML file
# under $ASTERISK_DOC_DIR. We extract every /*** DOCUMENTATION ... ***/
# block from our sources and assemble them into a single XML file.
# --------------------------------------------------------------------

$(DOC_XML): res/*.c
	@mkdir -p doc
	@( \
	  echo '<?xml version="1.0" encoding="UTF-8"?>'; \
	  echo '<docs xmlns:xi="http://www.w3.org/2001/XInclude">'; \
	  for f in res/*.c; do \
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
	    echo "  sudo apt install libpjproject-dev          (preferred)" >&2 ; \
	    echo "  make PJPROJECT_DIR=/path/to/asterisk-source (uses bundled headers)" >&2 ; \
	    exit 1 ; \
	fi

# --------------------------------------------------------------------
# Install / uninstall.
# --------------------------------------------------------------------

install: all
	install -d $(DESTDIR)$(ASTERISK_MODULES_DIR)
	install -m 0644 $(SOS) $(DESTDIR)$(ASTERISK_MODULES_DIR)/
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
	rm -f $(addprefix $(DESTDIR)$(ASTERISK_MODULES_DIR)/,$(addsuffix .so,$(MODULES)))
	rm -f $(DESTDIR)$(ASTERISK_DOC_DIR)/res_pjsip_cisco-en_US.xml
	rm -rf $(DESTDIR)$(ASTERISK_SAMPLE_DIR)

clean:
	rm -f $(OBJS) $(SOS) $(DOC_XML)

# --------------------------------------------------------------------
# Convenience: smoke-test load all modules in a running asterisk.
# --------------------------------------------------------------------

check:
	@failed=0; \
	for m in $(MODULES); do \
	  output=$$(asterisk -rx "module show like $$m" 2>&1); \
	  if printf '%s\n' "$$output" | grep -q Running; then \
	    echo "  $$m: Running"; \
	  else \
	    echo "  $$m: NOT RUNNING"; \
	    failed=1; \
	  fi; \
	done; \
	exit $$failed

help:
	@echo "Targets:"
	@echo "  make            - build all modules and the doc XML"
	@echo "  make install    - install modules, docs, and config samples"
	@echo "  make uninstall  - remove what 'install' put down"
	@echo "  make clean      - remove build artefacts"
	@echo "  make check      - report which modules are loaded in a"
	@echo "                    running asterisk (run as root)"
	@echo
	@echo "Common overrides:"
	@echo "  ASTERISK_INCLUDE_DIR (default: $(ASTERISK_INCLUDE_DIR))"
	@echo "  ASTERISK_MODULES_DIR (default: $(ASTERISK_MODULES_DIR))"
	@echo "  ASTERISK_DOC_DIR     (default: $(ASTERISK_DOC_DIR))"
	@echo "  DESTDIR              (default: empty; for packaging)"
