# Optional aliases for the shared Python build entry point.
ifeq ($(OS),Windows_NT)
PYTHON ?= python
else
PYTHON ?= python3
endif
.DEFAULT_GOAL := os
.PHONY: all os boot games rockbox qemu sdcard sd-image clean doctor stockram stockram-diag
all: os
os boot games rockbox qemu sdcard sd-image clean doctor stockram stockram-diag:
	$(PYTHON) scripts/build.py $@
