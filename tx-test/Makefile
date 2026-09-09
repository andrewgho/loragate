FQBN      = esp32:esp32:XIAO_ESP32C6
DBG_FQBN  = esp32:esp32:XIAO_ESP32C6:DebugLevel=info
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
