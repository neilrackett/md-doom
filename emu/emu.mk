# Copyright (C) 2026 Neil Rackett
# SPDX-License-Identifier: GPL-3.0-or-later
#
# make emu, included from the Makefile. It lives here rather than there so
# that changing it is an emulator-only change, which CI does not build or
# release (paths-ignore in .github/workflows).

## Build MD/DOOM for this computer and run it in Hatari with EmuMD (see
## README.md). TOS= a TOS image (default: EmuMD's EmuTOS); MONO=1 a mono
## monitor (ST high res); RECORD=FILE.avi records picture and sound until
## you quit; EMUMD= another EmuMD checkout.
EMUMD ?= emu/emumd
.PHONY: emu
emu:
	$(EMUMD)/tools/mdfw run $(if $(TOS),--tos $(TOS)) $(if $(RECORD),--record $(RECORD)) \
	    $(if $(MONO),-- --monitor mono)

## The same on a TT, whose colour monitor shows Doom in 256 colours. A TT
## needs EmuTOS's 512k image -- the 256k one EmuMD downloads cannot run
## one, and Hatari then quietly runs an STE instead -- so it is downloaded
## into EmuMD's cache, beside that one, the first time. TOS= (a TOS 3.06
## image, say) and RECORD= as for emu. No MONO=: a TT's mono monitor is
## TT high (1280x960), which MD/DOOM does not draw.
EMUTOS_TT_VERSION ?= 1.4
EMUMD_CACHE_DIR = $(or $(EMUMD_CACHE),$(or $(XDG_CACHE_HOME),$(HOME)/.cache)/emumd)
EMUTOS_TT_DIR = $(EMUMD_CACHE_DIR)/emutos-512k-$(EMUTOS_TT_VERSION)
EMUTOS_TT = $(EMUTOS_TT_DIR)/etos512uk.img
.PHONY: emu-tt
emu-tt: $(if $(TOS),,$(EMUTOS_TT))
	$(EMUMD)/tools/mdfw run --machine tt --tos $(or $(TOS),$(EMUTOS_TT)) \
	    $(if $(RECORD),--record $(RECORD))

# Unpacked aside first, so an interrupted download is tried again next time.
$(EMUTOS_TT):
	@echo "Downloading EmuTOS $(EMUTOS_TT_VERSION) (512k)"
	@set -e; mkdir -p "$(EMUMD_CACHE_DIR)"; \
	tmp=$$(mktemp -d "$(EMUMD_CACHE_DIR)/.emutos.XXXXXX"); trap 'rm -rf "$$tmp"' EXIT; \
	curl -fsSL -o "$$tmp/emutos.zip" \
	    "https://downloads.sourceforge.net/project/emutos/emutos/$(EMUTOS_TT_VERSION)/emutos-512k-$(EMUTOS_TT_VERSION).zip"; \
	python3 -m zipfile -e "$$tmp/emutos.zip" "$$tmp"; \
	rm -rf "$(EMUTOS_TT_DIR)"; \
	mv "$$tmp/emutos-512k-$(EMUTOS_TT_VERSION)" "$(EMUTOS_TT_DIR)"
