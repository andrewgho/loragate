# Host-side tests for logic kept free of Arduino/radio/WiFi dependencies
# specifically so it can run here — no ESP32 toolchain needed, just a C++
# compiler. Doesn't touch or replace testing the actual firmware (hardware
# behavior — radio, deep sleep, WiFi, the reed switch itself — still only
# gets verified by flashing real hardware): this locks down the wire
# format (test_lora_packet), tx's wake-decision logic (test_tx_wake_logic,
# against tx/door_wake_logic.h), tx's RTC-persistence checksum
# (test_rtc_checksum, against tx/rtc_checksum.h), and rx's retry-dedup
# logic (test_seq_dedup, against rx/seq_dedup.h).

CXX      = c++
CXXFLAGS = -std=c++17 -Wall -Wextra -Werror

BINS = test_lora_packet test_tx_wake_logic test_rtc_checksum test_seq_dedup

test: $(BINS)
	# lora_packet.h is duplicated between tx/ and rx/ (Arduino can't share
	# files across sketches) and must stay identical — catch it here if
	# that ever drifts.
	diff -q ../tx/lora_packet.h ../rx/lora_packet.h
	./test_lora_packet
	./test_tx_wake_logic
	./test_rtc_checksum
	./test_seq_dedup
	@echo "all tests passed"

test_lora_packet: test_lora_packet.cpp ../tx/lora_packet.h
	$(CXX) $(CXXFLAGS) -I../tx -o $@ test_lora_packet.cpp

test_tx_wake_logic: test_tx_wake_logic.cpp ../tx/door_wake_logic.h
	$(CXX) $(CXXFLAGS) -I../tx -o $@ test_tx_wake_logic.cpp

test_rtc_checksum: test_rtc_checksum.cpp ../tx/rtc_checksum.h ../tx/door_wake_logic.h
	$(CXX) $(CXXFLAGS) -I../tx -o $@ test_rtc_checksum.cpp

test_seq_dedup: test_seq_dedup.cpp ../rx/seq_dedup.h
	$(CXX) $(CXXFLAGS) -I../rx -o $@ test_seq_dedup.cpp

clean:
	rm -f $(BINS)

.PHONY: test clean
