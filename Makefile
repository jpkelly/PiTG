CC      = gcc
CFLAGS  = -O2 -Wall -Wextra
LDFLAGS = -lasound -lltc

TARGET  = pitg
SRCS    = pitg.c
HELPER  = pitgctl
HARNESS = pitg-harnessctl
GPIO_TARGET = pitg-gpio
GPIO_SRCS   = pitg_gpio.c
GPIO_LDFLAGS = -lgpiod
BTN_TARGET = pitg-cue-buttons
BTN_SRCS   = pitg_cue_buttons.c
BTN_LDFLAGS = -lgpiod

.PHONY: all clean install deps install-service enable-service

all: $(TARGET) $(GPIO_TARGET) $(BTN_TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(GPIO_TARGET): $(GPIO_SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(GPIO_LDFLAGS)

$(BTN_TARGET): $(BTN_SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(BTN_LDFLAGS)

# Install build dependencies (run once on the Pi)
deps:
	sudo apt-get install -y libltc-dev libasound2-dev libgpiod-dev gcc make

install: $(TARGET) $(GPIO_TARGET) $(BTN_TARGET)
	sudo install -m 755 $(TARGET) /usr/local/bin/$(TARGET)
	sudo install -m 755 $(HELPER) /usr/local/bin/$(HELPER)
	sudo install -m 755 $(HARNESS) /usr/local/bin/$(HARNESS)
	sudo install -m 755 $(GPIO_TARGET) /usr/local/bin/$(GPIO_TARGET)
	sudo install -m 755 $(BTN_TARGET) /usr/local/bin/$(BTN_TARGET)

install-service: $(TARGET) $(GPIO_TARGET) $(BTN_TARGET)
	sudo install -m 755 $(TARGET) /usr/local/bin/$(TARGET)
	sudo install -m 755 $(HELPER) /usr/local/bin/$(HELPER)
	sudo install -m 755 $(HARNESS) /usr/local/bin/$(HARNESS)
	sudo install -m 755 $(GPIO_TARGET) /usr/local/bin/$(GPIO_TARGET)
	sudo install -m 755 $(BTN_TARGET) /usr/local/bin/$(BTN_TARGET)
	sudo install -m 644 pitg.service /etc/systemd/system/pitg.service
	sudo install -m 644 pitg-gpio.service /etc/systemd/system/pitg-gpio.service
	sudo install -m 644 pitg-cue-buttons.service /etc/systemd/system/pitg-cue-buttons.service
	sudo install -m 644 99-waveshare-limitimer.rules /etc/udev/rules.d/99-waveshare-limitimer.rules
	sudo install -m 644 99-waveshare-rs485.rules /etc/udev/rules.d/99-waveshare-rs485.rules
	sudo sh -c 'test -f /etc/default/pitg || install -m 644 pitg.env.example /etc/default/pitg'
	sudo sh -c 'test -f /etc/default/pitg-gpio || install -m 644 pitg-gpio.env.example /etc/default/pitg-gpio'
	sudo sh -c 'test -f /etc/default/pitg-cue-buttons || install -m 644 pitg-cue-buttons.env.example /etc/default/pitg-cue-buttons'
	sudo udevadm control --reload-rules
	sudo systemctl daemon-reload

enable-service:
	sudo systemctl enable --now pitg.service
	sudo systemctl enable --now pitg-gpio.service
	sudo systemctl enable --now pitg-cue-buttons.service

clean:
	rm -f $(TARGET) $(GPIO_TARGET) $(BTN_TARGET)
