# Unit tests for asterisk-pjsip-cisco. Two flavours:
#
#   smoke   — sanity-checks the build artefacts that Asterisk's loader
#             will actually look at: validates the generated doc XML
#             with xmllint, and uses readelf to confirm each .so has
#             the AST_MODULE_INFO descriptor + load/unload symbols at
#             the offsets the loader expects.
#
#   unit    — standalone C tests with three flavours, picked per
#             helper-under-test's dependency surface:
#               * pjproject-only           (test_string_utils)
#               * libxml2-only             (test_xml_bodies)
#               * Asterisk-API + shim      (test_blf_pidf)
#             See tests/unit/README.md for the "which template" guide.
#
# Invoke from the repository root via `make tests`, or directly with
# `make -C tests/unit all` from a checkout.

SHELL := /bin/bash

REPO_ROOT := $(abspath $(CURDIR)/../..)
OBJ_DIR ?= obj
MODULE_BUILD_DIR ?= $(OBJ_DIR)
DOC_XML ?= $(OBJ_DIR)/doc/res_pjsip_cisco-en_US.xml

CC      ?= cc
# -O0: keep test bodies un-folded so assertions actually evaluate at
# runtime instead of gcc proving them at compile time and constant-
# folding the entire test away (which would also make local pj_str_t
# variables look "unused").
CFLAGS  ?= -O0 -g
override CFLAGS += -Wall -Werror -Wstrict-prototypes -Wmissing-prototypes \
                   -Wshadow -Wpointer-arith

# Pick up pjproject the same way the top-level Makefile does: pkg-config
# first, PJPROJECT_DIR second.
PJPROJECT_CFLAGS := $(shell pkg-config --cflags libpjproject 2>/dev/null)
ifeq ($(strip $(PJPROJECT_CFLAGS)),)
PJPROJECT_CFLAGS := $(shell pkg-config --cflags pjproject 2>/dev/null)
endif
ifeq ($(strip $(PJPROJECT_CFLAGS)),)
ifneq ($(strip $(PJPROJECT_DIR)),)
PJPROJECT_CFLAGS := -DPJ_AUTOCONF=1 \
                    -I$(PJPROJECT_DIR)/third-party/pjproject/source/pjlib/include \
                    -I$(PJPROJECT_DIR)/third-party/pjproject/source/pjlib-util/include \
                    -I$(PJPROJECT_DIR)/third-party/pjproject/source/pjnath/include \
                    -I$(PJPROJECT_DIR)/third-party/pjproject/source/pjmedia/include \
                    -I$(PJPROJECT_DIR)/third-party/pjproject/source/pjsip/include
endif
endif

PJPROJECT_LIBS := $(shell pkg-config --libs libpjproject 2>/dev/null)
ifeq ($(strip $(PJPROJECT_LIBS)),)
PJPROJECT_LIBS := $(shell pkg-config --libs pjproject 2>/dev/null)
endif
# Fall back to bundled static libs under PJPROJECT_DIR if pkg-config
# didn't surface a system pjproject — same path the top-level Makefile
# pulls headers from when building against an Asterisk source tree.
# Glob picks up whatever <arch> suffix the build used (e.g.
# x86_64-pc-linux-gnu vs x86_64-unknown-linux-gnu).
ifeq ($(strip $(PJPROJECT_LIBS)),)
ifneq ($(strip $(PJPROJECT_DIR)),)
PJPROJECT_LIBS := \
    $(wildcard $(PJPROJECT_DIR)/third-party/pjproject/source/pjsip/lib/libpjsua-*.a) \
    $(wildcard $(PJPROJECT_DIR)/third-party/pjproject/source/pjsip/lib/libpjsip-ua-*.a) \
    $(wildcard $(PJPROJECT_DIR)/third-party/pjproject/source/pjsip/lib/libpjsip-simple-*.a) \
    $(wildcard $(PJPROJECT_DIR)/third-party/pjproject/source/pjsip/lib/libpjsip-*.a) \
    $(wildcard $(PJPROJECT_DIR)/third-party/pjproject/source/pjmedia/lib/libpjmedia-codec-*.a) \
    $(wildcard $(PJPROJECT_DIR)/third-party/pjproject/source/pjmedia/lib/libpjmedia-videodev-*.a) \
    $(wildcard $(PJPROJECT_DIR)/third-party/pjproject/source/pjmedia/lib/libpjmedia-audiodev-*.a) \
    $(wildcard $(PJPROJECT_DIR)/third-party/pjproject/source/pjmedia/lib/libpjmedia-*.a) \
    $(wildcard $(PJPROJECT_DIR)/third-party/pjproject/source/pjnath/lib/libpjnath-*.a) \
    $(wildcard $(PJPROJECT_DIR)/third-party/pjproject/source/pjlib-util/lib/libpjlib-util-*.a) \
    $(wildcard $(PJPROJECT_DIR)/third-party/pjproject/source/pjlib/lib/libpj-*.a)
endif
endif

UNIT_BINS := test_string_utils test_xml_bodies test_blf_pidf

# Asterisk's bundled pjproject patches malloc/free to route through
# __ast_repl_malloc / __ast_free. Stubs link those symbols into the
# test binary so we don't need libasterisk. libuuid covers
# pjlib/guid_uuid.c.
STUB_OBJS  := asterisk_stubs.o
EXTRA_LIBS := -luuid -lpthread -lm

# test_xml_bodies links against libxml2 only (no pjproject), and
# needs to include the body-template headers that live alongside
# each module's other private declarations. Headers are no-deps —
# just C string-literal macros — so adding the -I paths costs
# nothing for the other test binaries that ignore them.
XML_CFLAGS := $(shell pkg-config --cflags libxml-2.0 2>/dev/null)
XML_LIBS   := $(shell pkg-config --libs   libxml-2.0 2>/dev/null)
BODY_INCLUDES := \
    -I$(REPO_ROOT)/res/cisco_bulkupdate/include \
    -I$(REPO_ROOT)/res/cisco_remotecc/include

.PHONY: all smoke unit clean

all: smoke unit

# --------------------------------------------------------------------
# Smoke target: validates that `make` produced loadable artefacts.
# Runs xmllint on the generated documentation XML, and uses readelf
# to verify each .so file has the AST_MODULE_INFO + load_module +
# unload_module symbols Asterisk's loader expects.
# --------------------------------------------------------------------

smoke:
	@OBJ_DIR='$(OBJ_DIR)' \
	    MODULE_BUILD_DIR='$(MODULE_BUILD_DIR)' \
	    DOC_XML='$(DOC_XML)' \
	    $(REPO_ROOT)/tests/unit/smoke.sh

# --------------------------------------------------------------------
# Unit tests: standalone C programs linked against pjproject. Each test
# carries its own main() with assert()s. Adding a new test: write
# test_<name>.c, add the name to UNIT_BINS above, copy the pjlib init
# pattern from test_string_utils.c.
# --------------------------------------------------------------------

unit: $(UNIT_BINS)
	@set -e; for t in $(UNIT_BINS); do \
	  echo "  RUN  $$t"; \
	  ./$$t || { echo "  FAIL $$t"; exit 1; }; \
	done
	@echo "All $(words $(UNIT_BINS)) unit test(s) passed."

asterisk_stubs.o: asterisk_stubs.c
	$(CC) $(CFLAGS) -I$(REPO_ROOT)/tests/unit/include -c -o $@ $<

# test_xml_bodies is libxml2-linked (no pjproject), unlike the rest.
# Explicit rule overrides the pattern rule below.
test_xml_bodies: test_xml_bodies.c
	$(CC) $(CFLAGS) $(BODY_INCLUDES) $(XML_CFLAGS) -o $@ $< $(XML_LIBS)

# test_blf_pidf exercises cisco_blf_build_pidf as a pure function:
# compiles a private copy of pidf.c against the stub asterisk headers
# in tests/unit/include/, links it with the asterisk_stubs.c shims
# (ast_str_*, ast_sip_sanitize_xml). No pjproject linkage; no real
# Asterisk runtime touched. The -I order matters — the stub-include
# path comes BEFORE any system include so #include <asterisk/*.h>
# resolves to our shims, not to a system-installed asterisk-dev.
PIDF_INCLUDES := \
    -I$(REPO_ROOT)/tests/unit/include \
    -I$(REPO_ROOT)/res/cisco_unsolicited_blf/include
test_blf_pidf: test_blf_pidf.c $(REPO_ROOT)/res/cisco_unsolicited_blf/pidf.c \
        asterisk_stubs.o
	$(CC) $(CFLAGS) $(PIDF_INCLUDES) -o $@ \
	    test_blf_pidf.c $(REPO_ROOT)/res/cisco_unsolicited_blf/pidf.c \
	    asterisk_stubs.o

%: %.c $(STUB_OBJS)
	$(CC) $(CFLAGS) $(PJPROJECT_CFLAGS) -o $@ $< $(STUB_OBJS) \
	    $(PJPROJECT_LIBS) $(EXTRA_LIBS)

clean:
	rm -f $(UNIT_BINS) $(STUB_OBJS)
