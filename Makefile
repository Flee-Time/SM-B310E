# Optional aliases for the shared Python build entry point.
ifeq ($(OS),Windows_NT)
PYTHON ?= python
else
PYTHON ?= python3
endif
.DEFAULT_GOAL := firmware
.PHONY: all firmware os-sd debug dsp-diag fpmain games rockbox qemu sdcard sd-image hosttest check clean doctor dis size stockram stockram-diag
all: firmware
firmware os-sd debug dsp-diag fpmain games rockbox qemu sdcard sd-image hosttest check clean doctor dis size stockram stockram-diag:
	$(PYTHON) scripts/build.py $@
