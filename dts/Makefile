DTC ?= dtc
DTC_FLAGS ?= -@ -I dts -O dtb

SRCS := aht10-overlay.dts w25q64-overlay.dts
TARGETS := $(SRCS:.dts=.dtbo)

.PHONY: all clean install

all: $(TARGETS)

%.dtbo: %.dts
	$(DTC) $(DTC_FLAGS) -o $@ $<

install: all
	@echo "Installing dtbo overlays to /boot/firmware/overlays/..."
	sudo cp $(TARGETS) /boot/firmware/overlays/
	@echo "Overlays installed. Remember to append them in /boot/firmware/config.txt."

clean:
	rm -f $(TARGETS)
