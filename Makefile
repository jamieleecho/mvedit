# mvedit -- a mouse-driven text editor for Multi-Vue, built with MVKit.
#
# Run every build inside the coco-dev toolchain image, e.g.:
#     ./coco-dev make          # build build/mvedit.os9
#     ./coco-dev make clean
#     make run                 # launch in MAME (on the host, needs a display)
#
# This Makefile drives mvkit/app.mk (the reusable MVKit app build) and adds the
# one-time bootstrap app.mk leaves to the project: cloning + building cmoc_os9
# (libc/libcgfx) at a known-good commit and installing the vendored MVKit into
# cmoc's shared dir so the app can `#include <mvkit/mvkit.h>` and link -lmvkit.

# ---- app.mk configuration (must precede the include) ------------------------
APP          := mvedit
SHORT        := mve
SRCS         := textdoc.c text_view.c mvedit.c

# Screen type 5 = 1 bpp (640x192, black & white); app.mk derives image BPP=1.
SCREEN_TYPE  := 5
WIN_W        := 80
WIN_H        := 25
WIN_BG       := 0
WIN_FG       := 1
# Data area (BSS + stack + heap) in 256-byte pages. The 6809 maps only 64KB per
# process, shared with the ~26KB code module, so module + data must stay under
# 64KB or the fork fails with E$MemFul (207). 120 pages (30KB) holds the three
# whole-document buffers (model + undo snapshot + clipboard, ~20KB of BSS) with
# room for the stack; raising it much past ~140 reintroduces the fork error.
MEM_SIZE     := 120

CMOC_OS9_DIR := cmoc_os9
BASEIMAGE    := disks/NOS9_6809_L2_v030300_coco3_80d.os9

# Sample document(s) copied to the disk root. The CR-converted copy under
# $(BUILD) is what lands on the disk (see the rule below). Deferred (=) so
# $(BUILD), defined by app.mk after this point, expands when the rule runs.
DATA_FILES    = $(BUILD)/tom.mve

-include mvkit/app.mk

# ---- bootstrap that app.mk leaves to the project ----------------------------
CMOC_OS9_COMMIT := 14b8f6bc983a1c694d36e3890f34b16c06a2af20

# Make the app binary wait for cmoc_os9's libc/libcgfx and an installed MVKit.
$(BUILD)/$(APP): | libc libcgfx mvkit

# app.mk's AIF rule has no prerequisites, so a change to WIN_W/WIN_H/SCREEN_TYPE
# would not regenerate it. Depend on this Makefile so those edits take effect.
$(AIF): Makefile

# Convert the host (LF) sample document to OS-9 CR line endings before it is
# copied to the disk root, so it's a proper OS-9 text file (textdoc_load also
# tolerates LF, but mvedit's own save and other OS-9 tools use CR).
$(BUILD)/tom.mve: tom.mve | $(BUILD)
	unix2mac -q -n $< $@

$(CMOC_OS9_DIR):
	git clone https://github.com/nitros9project/cmoc_os9.git $@
	cd $@ && git checkout $(CMOC_OS9_COMMIT)

.PHONY: libc libcgfx mvkit real-clean

## Build cmoc_os9's C library (libc)
libc: | $(CMOC_OS9_DIR)
	$(MAKE) -C $(CMOC_OS9_DIR)/lib all

## Build cmoc_os9's CoCo graphics library (libcgfx)
libcgfx: | $(CMOC_OS9_DIR)
	$(MAKE) -C $(CMOC_OS9_DIR)/cgfx all

## Fetch (once) and install the vendored MVKit into cmoc's shared dir. Every
## `docker run` is a fresh container, so the install must run on every build;
## the clone is skipped when mvkit/ is already checked out (kept by the volume).
mvkit:
	if [ ! -d mvkit ]; then \
	  rm -rf xmastree; \
	  git clone https://github.com/jamieleecho/xmastree.git; \
	  mv xmastree/$@ .; \
	  rm -rf xmastree; \
	fi
	$(MAKE) -C mvkit install

## Remove build artifacts, the installed MVKit, and the cmoc_os9 checkout
real-clean: clean
	-$(MAKE) -C mvkit uninstall
	-$(MAKE) -C mvkit clean
	rm -rf $(CMOC_OS9_DIR) mvkit xmastree
