SUBDIRS = tx-test rx-test tx rx

.DEFAULT_GOAL := compile

# Compile every sketch (no upload, no hardware needed) — the fast check that
# everything still builds against the installed ESP32 core + RadioLib.
compile:
	$(foreach d,$(SUBDIRS),$(MAKE) -C $(d) compile &&) true

# Host-side tests for the link-layer packet formats (see test/Makefile) —
# no ESP32 toolchain or hardware needed either.
test:
	$(MAKE) -C test test

flash-tx:
	$(MAKE) -C tx-test flash

flash-rx:
	$(MAKE) -C rx-test flash

clean:
	$(foreach d,$(SUBDIRS),$(MAKE) -C $(d) clean;)
	$(MAKE) -C test clean

.PHONY: compile test flash-tx flash-rx clean
