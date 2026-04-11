CC      = gcc
CFLAGS  = -O2 -Wall -Wextra
LDFLAGS = -lasound -lltc

TARGET  = pitg
SRCS    = pitg.c
HELPER  = pitgctl

.PHONY: all clean install deps install-service enable-service

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Install build dependencies (run once on the Pi)
deps:
	sudo apt-get install -y libltc-dev libasound2-dev gcc make

install: $(TARGET)
	sudo install -m 755 $(TARGET) /usr/local/bin/$(TARGET)
	sudo install -m 755 $(HELPER) /usr/local/bin/$(HELPER)

install-service: $(TARGET)
	sudo install -m 755 $(TARGET) /usr/local/bin/$(TARGET)
	sudo install -m 755 $(HELPER) /usr/local/bin/$(HELPER)
	sudo install -m 644 pitg.service /etc/systemd/system/pitg.service
	sudo sh -c 'test -f /etc/default/pitg || install -m 644 pitg.env.example /etc/default/pitg'
	sudo systemctl daemon-reload

enable-service:
	sudo systemctl enable --now pitg.service

clean:
	rm -f $(TARGET)
