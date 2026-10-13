# CDCOnBoot=default routes Serial to hardware UART0 (D6/D7 — confirmed
# unused by lora_pins.h) instead of native USB CDC. Native USB doesn't
# survive deep sleep and nobody's plugged into it in the field anyway;
# UART0 does survive it (with an external FTDI adapter) and is what made
# the brownout/RTC_NOINIT_ATTR investigation possible in the first place.
# Standard for production builds; DEBUG builds keep CDCOnBoot=cdc since
# that's the "USB plugged in, monitor right there" case — one cable for
# both flashing and monitoring, no separate FTDI adapter needed.
FQBN      = esp32:esp32:XIAO_ESP32C6:CDCOnBoot=default
DBG_FQBN  = esp32:esp32:XIAO_ESP32C6:DebugLevel=info,CDCOnBoot=cdc
# Monitoring a production (non-debug) build means pointing this at the
# FTDI adapter's device path, not the native USB one -- there's no native
# USB connection to monitor on a build that deep sleeps.
PORT     ?= /dev/cu.usbmodem1101
SKETCH    = $(notdir $(CURDIR))
BUILD_DIR = build
DBG_DIR   = build-debug

compile: $(BUILD_DIR)/$(SKETCH).ino.bin

$(BUILD_DIR)/$(SKETCH).ino.bin: *.ino *.h
	arduino-cli compile --verbose --fqbn $(FQBN) --build-path $(BUILD_DIR) .

$(DBG_DIR)/$(SKETCH).ino.bin: *.ino *.h
	arduino-cli compile --verbose --fqbn $(DBG_FQBN) --build-path $(DBG_DIR) \
	  --build-property "compiler.cpp.extra_flags=-DDEBUG" .

upload: $(BUILD_DIR)/$(SKETCH).ino.bin
	arduino-cli upload -p $(PORT) --fqbn $(FQBN) --input-dir $(BUILD_DIR) .

flash: $(BUILD_DIR)/$(SKETCH).ino.bin upload

debug: $(DBG_DIR)/$(SKETCH).ino.bin
	arduino-cli upload -p $(PORT) --fqbn $(DBG_FQBN) --input-dir $(DBG_DIR) .
	arduino-cli monitor -p $(PORT) --config baudrate=115200

monitor:
	arduino-cli monitor -p $(PORT) --config baudrate=115200

clean:
	rm -rf $(BUILD_DIR) $(DBG_DIR)

.PHONY: compile upload flash debug monitor clean
