# ghostcontrol-8bitdo — Xbox controllers on PS5 via the 8BitDo USB Adapter 2
# (Switch mode) or by wire, as virtual DualSense pads.
#
#   export PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk
#   make                      build ghostcontrol-8bitdo.elf
#   make send PS5_HOST=<ip>   send it to an ELF loader listening on port 9021

PS5_HOST ?= ps5
PS5_PORT ?= 9021
TARGET   := ghostcontrol-8bitdo.elf

ifndef PS5_PAYLOAD_SDK
$(error PS5_PAYLOAD_SDK is not set)
endif

include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

CFLAGS  += -D__PROSPERO__ -Wall -Wextra -O2 -fPIC -fno-stack-protector
LDFLAGS += -lScePad -lSceUserService -lSceAppInstUtil

SRC := $(addprefix src/,main.c switch_pad.c gip_pad.c usb_guard.c shellui_bind.c mapper.c web.c shortcut.c)

.PHONY: all clean send
all: $(TARGET)

$(TARGET): $(SRC) $(wildcard src/*.h) assets/index.html assets/icon0.png
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)

send: $(TARGET)
	nc -q0 $(PS5_HOST) $(PS5_PORT) < $(TARGET)

clean:
	rm -f $(TARGET)
