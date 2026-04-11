# PiTG

PiTG is a simple LTC timecode generator for Raspberry Pi.

It generates SMPTE LTC and outputs it on the Raspberry Pi 3.5mm audio jack using ALSA and libltc. The intended target is a Raspberry Pi 1 running 32-bit Raspberry Pi OS Trixie as a fire-and-forget LTC test source.

## Features

- Standard frame rates: `24`, `25`, `29.97`, `29.97df`, `30`
- Starts from system wall clock by default
- Optional fixed start timecode
- Headless operation via systemd
- Output via the Pi headphone jack

## Requirements

- Raspberry Pi OS 32-bit
- `libltc-dev`
- `libasound2-dev`
- `gcc`
- `make`

## Build

On the Pi:

```bash
cd ~/PiTG
make deps
make
```

Optional install:

```bash
sudo make install
```

## Usage

Run with defaults:

```bash
./pitg
```

Examples:

```bash
./pitg -r 25
./pitg -r 30
./pitg -r 29.97df
./pitg -r 24 -s 01:00:00:00
./pitg -r 25 -d plughw:CARD=Headphones,DEV=0
```

Options:

- `-r <fps>` frame rate: `24 | 25 | 29.97 | 29.97df | 30`
- `-a <hz>` sample rate in Hz: `8000-192000`
- `-d <device>` ALSA device
- `-s <HH:MM:SS:FF>` fixed start timecode

Example with explicit sample rate:

```bash
./pitg -r 25 -a 48000 -d plughw:CARD=Headphones,DEV=0
```

## Audio Output

For Raspberry Pi 1, the intended output is:

```bash
plughw:CARD=Headphones,DEV=0
```

List devices with:

```bash
aplay -l
aplay -L
```

Set output level manually if needed:

```bash
amixer sset PCM 100% unmute
```

## Systemd Service

Install the service:

```bash
sudo make install-service
sudo make enable-service
```

This installs:

- `/usr/local/bin/pitg`
- `/etc/systemd/system/pitg.service`
- `/etc/default/pitg` if it does not already exist

The service is configured to:

- force headphone PCM to `100%` and unmute before start
- restart automatically if the process exits
- start on boot

## Service Configuration

Edit:

```bash
sudo nano /etc/default/pitg
```

Example:

```bash
PITG_OPTS="-r 25 -a 48000 -d plughw:CARD=Headphones,DEV=0"
```

Apply changes:

```bash
sudo systemctl restart pitg.service
```

Recommended settings on Pi 1:

- Frame rate: choose the rate your receiving clock expects
- Sample rate: `48000` is the safest default
- ALSA device: `plughw:CARD=Headphones,DEV=0`

Check status:

```bash
systemctl status pitg.service
journalctl -u pitg.service -n 50 --no-pager
```

## Notes

- Do not run a manual `pitg` process at the same time as `pitg.service`.
- If LTC disappears unexpectedly, first check whether another instance is already holding the audio device.
- The Pi 1 audio jack is PWM-based, but it is adequate for LTC test generation.