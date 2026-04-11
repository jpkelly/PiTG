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
- `/usr/local/bin/pitgctl`
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

Or use the helper:

```bash
sudo pitgctl restart
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

## pitgctl Helper

`pitgctl` provides a small command wrapper around the service and `/etc/default/pitg`.

Show the current effective config:

```bash
sudo pitgctl show
```

Basic service control:

```bash
sudo pitgctl status
sudo pitgctl start
sudo pitgctl stop
sudo pitgctl restart
sudo pitgctl logs
```

Set common options:

```bash
sudo pitgctl set-fps 25
sudo pitgctl set-fps 29.97df
sudo pitgctl set-rate 48000
sudo pitgctl set-device plughw:CARD=Headphones,DEV=0
sudo pitgctl set-start 01:00:00:00
sudo pitgctl clear-start
```

Replace the full option string directly:

```bash
sudo pitgctl set-opts "-r 25 -a 48000 -d plughw:CARD=Headphones,DEV=0"
```

Every `set-*` command updates `/etc/default/pitg` and restarts `pitg.service` automatically.

## Configuration Files

Runtime configuration:

- `/etc/default/pitg`

Service definition:

- `/etc/systemd/system/pitg.service`

Installed binaries:

- `/usr/local/bin/pitg`
- `/usr/local/bin/pitgctl`

Template config in the repo:

- `pitg.env.example`

## Recommended Configuration

For a Pi 1 LTC test source, use:

```bash
PITG_OPTS="-r 25 -a 48000 -d plughw:CARD=Headphones,DEV=0"
```

If your target expects another frame rate, change only `-r` first.

Recommended adjustment order:

- frame rate first
- sample rate second, only if needed
- ALSA device only if the Pi audio routing changes
- fixed start time only if you explicitly want free-running code from a known start value instead of wall clock

## Troubleshooting

- If there is no LTC, run `sudo pitgctl status` and `sudo pitgctl logs`.
- If the service is running but the target sees no code, verify the mixer state with `amixer scontents`.
- Do not run a manual `pitg` process while `pitg.service` is active.
- If you need to test manually, stop the service first: `sudo pitgctl stop`
- Restart the service after manual testing: `sudo pitgctl start`

## Notes

- Do not run a manual `pitg` process at the same time as `pitg.service`.
- If LTC disappears unexpectedly, first check whether another instance is already holding the audio device.
- The Pi 1 audio jack is PWM-based, but it is adequate for LTC test generation.
*** Add File: /Users/jp/Documents/GitHub/PiTG/pitgctl
#!/bin/sh

set -eu

SERVICE="pitg.service"
CONFIG_FILE="/etc/default/pitg"
DEFAULT_OPTS="-r 25 -a 48000 -d plughw:CARD=Headphones,DEV=0"

usage() {
	cat <<'EOF'
Usage:
  pitgctl show
  pitgctl status
  pitgctl logs
  pitgctl start
  pitgctl stop
  pitgctl restart
  pitgctl set-fps <24|25|29.97|29.97df|30>
  pitgctl set-rate <hz>
  pitgctl set-device <alsa-device>
  pitgctl set-start <HH:MM:SS:FF>
  pitgctl clear-start
  pitgctl set-opts "<full pitg option string>"
EOF
}

ensure_root() {
	if [ "$(id -u)" -ne 0 ]; then
		echo "pitgctl: run as root (use sudo)" >&2
		exit 1
	fi
}

ensure_config() {
	if [ ! -f "$CONFIG_FILE" ]; then
		write_opts "$DEFAULT_OPTS"
	fi
}

current_opts() {
	if [ -f "$CONFIG_FILE" ]; then
		sed -n 's/^PITG_OPTS="\(.*\)"$/\1/p' "$CONFIG_FILE" | tail -n 1
	fi
}

write_opts() {
	opts="$1"
	cat > "$CONFIG_FILE" <<EOF
# Managed by pitgctl.
# Standard frame rates: 24, 25, 29.97, 29.97df, 30

PITG_OPTS="$opts"
EOF
}

apply_and_restart() {
	systemctl restart "$SERVICE"
	systemctl --no-pager --full status "$SERVICE" | head -15
}

replace_or_append_flag() {
	opts="$1"
	flag="$2"
	value="$3"

	set -- $opts
	out=""
	replaced=0

	while [ "$#" -gt 0 ]; do
		if [ "$1" = "$flag" ]; then
			shift
			if [ "$#" -eq 0 ]; then
				break
			fi
			if [ -n "$out" ]; then
				out="$out "
			fi
			out="$out$flag $value"
			replaced=1
			shift
			continue
		fi
		if [ -n "$out" ]; then
			out="$out "
		fi
		out="$out$1"
		shift
	done

	if [ "$replaced" -eq 0 ]; then
		if [ -n "$out" ]; then
			out="$out "
		fi
		out="$out$flag $value"
	fi

	echo "$out"
}

remove_flag() {
	opts="$1"
	flag="$2"

	set -- $opts
	out=""

	while [ "$#" -gt 0 ]; do
		if [ "$1" = "$flag" ]; then
			shift
			if [ "$#" -gt 0 ]; then
				shift
			fi
			continue
		fi
		if [ -n "$out" ]; then
			out="$out "
		fi
		out="$out$1"
		shift
	done

	echo "$out"
}

validate_fps() {
	case "$1" in
		24|25|29.97|29.97df|30) ;;
		*)
			echo "pitgctl: invalid frame rate '$1'" >&2
			exit 1
			;;
	esac
}

validate_rate() {
	case "$1" in
		''|*[!0-9]*)
			echo "pitgctl: sample rate must be an integer" >&2
			exit 1
			;;
	esac

	if [ "$1" -lt 8000 ] || [ "$1" -gt 192000 ]; then
		echo "pitgctl: sample rate must be between 8000 and 192000" >&2
		exit 1
	fi
}

validate_start() {
	echo "$1" | grep -Eq '^[0-9]{2}:[0-9]{2}:[0-9]{2}:[0-9]{2}$' || {
		echo "pitgctl: start timecode must be HH:MM:SS:FF" >&2
		exit 1
	}
}

cmd_show() {
	ensure_config
	echo "CONFIG_FILE=$CONFIG_FILE"
	echo "PITG_OPTS=$(current_opts)"
}

cmd_status() {
	systemctl --no-pager --full status "$SERVICE"
}

cmd_logs() {
	journalctl -u "$SERVICE" -n 50 --no-pager
}

cmd_set_flag() {
	ensure_root
	ensure_config
	opts=$(current_opts)
	new_opts=$(replace_or_append_flag "$opts" "$1" "$2")
	write_opts "$new_opts"
	apply_and_restart
}

cmd_clear_start() {
	ensure_root
	ensure_config
	opts=$(current_opts)
	new_opts=$(remove_flag "$opts" "-s")
	write_opts "$new_opts"
	apply_and_restart
}

cmd_set_opts() {
	ensure_root
	write_opts "$1"
	apply_and_restart
}

case "${1:-}" in
	show)
		cmd_show
		;;
	status)
		cmd_status
		;;
	logs)
		cmd_logs
		;;
	start)
		ensure_root
		systemctl start "$SERVICE"
		cmd_status
		;;
	stop)
		ensure_root
		systemctl stop "$SERVICE"
		cmd_status
		;;
	restart)
		ensure_root
		apply_and_restart
		;;
	set-fps)
		[ "$#" -eq 2 ] || { usage >&2; exit 1; }
		validate_fps "$2"
		cmd_set_flag -r "$2"
		;;
	set-rate)
		[ "$#" -eq 2 ] || { usage >&2; exit 1; }
		validate_rate "$2"
		cmd_set_flag -a "$2"
		;;
	set-device)
		[ "$#" -eq 2 ] || { usage >&2; exit 1; }
		cmd_set_flag -d "$2"
		;;
	set-start)
		[ "$#" -eq 2 ] || { usage >&2; exit 1; }
		validate_start "$2"
		cmd_set_flag -s "$2"
		;;
	clear-start)
		[ "$#" -eq 1 ] || { usage >&2; exit 1; }
		cmd_clear_start
		;;
	set-opts)
		[ "$#" -eq 2 ] || { usage >&2; exit 1; }
		cmd_set_opts "$2"
		;;
	''|-h|--help|help)
		usage
		;;
	*)
		usage >&2
		exit 1
		;;
esac