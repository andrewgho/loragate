# Host-side tests for the link-layer packet formats — no ESP32/Arduino
# toolchain needed, just a C++ compiler. Doesn't touch or replace testing the
# actual firmware (radio/WiFi/reed-switch logic still only gets verified by
# flashing real hardware); this just locks down the wire format both ends
# depend on.

CXX      = c++
CXXFLAGS = -std=c++17 -Wall -Wextra -Werror

BINS = test_lora_packet_phase1 test_lora_packet_phase2

test: $(BINS)
	# lora_packet.h is duplicated across sketch directories (Arduino can't
	# share files across sketches) and must stay identical within each
	# phase's pair — catch it here if that ever drifts.
	diff -q ../tx-test/lora_packet.h ../rx-test/lora_packet.h
	diff -q ../tx/lora_packet.h ../rx/lora_packet.h
	./test_lora_packet_phase1
	./test_lora_packet_phase2
	@echo "all tests passed"

test_lora_packet_phase1: test_lora_packet_phase1.cpp ../tx-test/lora_packet.h
	$(CXX) $(CXXFLAGS) -I../tx-test -o $@ test_lora_packet_phase1.cpp

test_lora_packet_phase2: test_lora_packet_phase2.cpp ../tx/lora_packet.h
	$(CXX) $(CXXFLAGS) -I../tx -o $@ test_lora_packet_phase2.cpp

clean:
	rm -f $(BINS)

.PHONY: test clean
