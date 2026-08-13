SUBDIRS = tx-test rx-test

flash-tx:
	$(MAKE) -C tx-test flash

flash-rx:
	$(MAKE) -C rx-test flash

clean:
	$(foreach d,$(SUBDIRS),$(MAKE) -C $(d) clean;)

.PHONY: flash-tx flash-rx clean
