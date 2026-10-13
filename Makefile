.DEFAULT_GOAL := bin
ip ?= PIC18Flasher.local
port ?= /dev/ttyACM0
BUILD := /tmp/arduino-build-pic18
BIN := $(wildcard $(BUILD)/*.ino.bin)
# T-Display S3: 16MB flash, OPI PSRAM (per Xinyuan-LilyGO/T-Display-S3 README)
FQBN := esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=opi
ESPOTA ?= $(HOME)/.arduino15/packages/esp32/hardware/esp32/3.3.0/tools/espota.py

# Push a file (script or .hex) to the running flasher's FFat, e.g.:
#   make file F=path/to/app.hex
file:
	curl -f -F 'file=@$(F)' http://$(ip)/file

bin:
	@ arduino-cli compile --build-path $(BUILD) -b $(FQBN) .

# "flash" here means updating the jig itself over WiFi, not the target.
flash: ota

usb: serial

# Frees the port if a microcom session holds it, then uploads over USB.
serial:
	- pkill -f "microcom.*$(port)"
	arduino-cli upload -p $(port) --fqbn $(FQBN) --input-dir $(BUILD) .

ota:
	python3 "$(ESPOTA)" -r -i $(ip) -p 3232 -f "$(BIN)"

# Host-side checks (g++ only, no hardware). Later tasks append their own binaries.
test:
	g++ -I. -x c++ tests/test_ihx.c ihx.cpp -o $(BUILD)-t1 && $(BUILD)-t1
	g++ -I. -x c++ tests/test_script.c script.cpp ihx.cpp -o $(BUILD)-t2 && $(BUILD)-t2
	g++ -I. -DHOST_TEST -x c++ tests/test_parts.c pic18k22.cpp -o $(BUILD)-t3 && $(BUILD)-t3

clean:
	rm -rf $(BUILD)
