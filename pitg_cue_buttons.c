/*
 * pitg_cue_buttons.c - Two-button PerfectCue transmitter for Raspberry Pi
 *
 * Reads two GPIO button inputs and sends PerfectCue bytes over a hardware
 * UART connected to an RS-485 transceiver.  Since the Pi 1 Model B has a
 * single UART (ttyAMA0), this service should not run concurrently with
 * pitg-gpio (Limitimer).  Both share the same UART and RS-485 wiring.
 *
 * Default mapping:
 *   NEXT button  -> 0x0F
 *   PREV button  -> 0x1F
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <gpiod.h>

#define DEFAULT_NEXT_GPIO 23
#define DEFAULT_PREV_GPIO 24
#define DEFAULT_SERIAL "/dev/ttyAMA0"
#define DEFAULT_CHIP_INDEX 0
#define DEFAULT_BAUD 19200
#define DEFAULT_DEBOUNCE_MS 120

typedef struct {
    int next_gpio;
    int prev_gpio;
    int chip_index;
    int baud;
    int debounce_ms;
    int active_high;
    const char *serial_dev;
} options_t;

static volatile sig_atomic_t g_running = 1;

static void on_signal(int sig)
{
    (void)sig;
    g_running = 0;
}

static int parse_int_arg(const char *s, int minv, int maxv)
{
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (!s || *s == '\0' || !end || *end != '\0') return -1;
    if (v < minv || v > maxv) return -1;
    return (int)v;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [-N next_gpio] [-P prev_gpio] [-S serial_dev]\n"
            "          [-B baud] [-D debounce_ms] [-c chip_index] [-H] [-T]\n"
            "\n"
            "  -N  GPIO input pin for NEXT button (default: 23)\n"
            "  -P  GPIO input pin for PREV button (default: 24)\n"
            "  -S  serial device for RS-485 TX (default: /dev/ttyAMA0)\n"
            "  -B  baud rate (default: 19200)\n"
            "  -D  debounce time in ms (default: 120)\n"
            "  -c  GPIO chip index (default: 0 -> /dev/gpiochip0)\n"
            "  -H  buttons are active-high (default is active-low)\n"
            "  -T  test mode: auto-fire next/prev alternating every 10s\n",
            prog);
}

static speed_t baud_to_speed(int baud)
{
    switch (baud) {
    case 1200:   return B1200;
    case 2400:   return B2400;
    case 4800:   return B4800;
    case 9600:   return B9600;
    case 19200:  return B19200;
    case 38400:  return B38400;
    case 57600:  return B57600;
    case 115200: return B115200;
    default:     return B0;
    }
}

static int open_serial(const char *dev, int baud)
{
    speed_t speed = baud_to_speed(baud);
    if (speed == B0) {
        fprintf(stderr, "pitg-cue-buttons: unsupported baud rate %d\n", baud);
        return -1;
    }

    int fd = open(dev, O_WRONLY | O_NOCTTY | O_NDELAY);
    if (fd < 0) {
        fprintf(stderr, "pitg-cue-buttons: cannot open %s: %s\n", dev, strerror(errno));
        return -1;
    }

    struct termios tio;
    memset(&tio, 0, sizeof(tio));
    tio.c_cflag = CS8 | CLOCAL;   /* 8N1, ignore modem control */
    tio.c_oflag = 0;
    tio.c_iflag = 0;
    tio.c_lflag = 0;
    cfsetospeed(&tio, speed);
    cfsetispeed(&tio, speed);

    if (tcsetattr(fd, TCSANOW, &tio) < 0) {
        fprintf(stderr, "pitg-cue-buttons: tcsetattr failed: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    tcflush(fd, TCOFLUSH);
    return fd;
}

static int send_cue(int fd, uint8_t byte)
{
    if (write(fd, &byte, 1) != 1) {
        fprintf(stderr, "pitg-cue-buttons: write failed: %s\n", strerror(errno));
        return -1;
    }
    tcdrain(fd);
    return 0;
}

static struct gpiod_line_request *request_input(struct gpiod_chip *chip, int gpio,
                                                const char *consumer)
{
    struct gpiod_line_settings *settings = NULL;
    struct gpiod_line_config *line_cfg = NULL;
    struct gpiod_request_config *req_cfg = NULL;
    struct gpiod_line_request *req = NULL;
    unsigned int offset;

    settings = gpiod_line_settings_new();
    line_cfg = gpiod_line_config_new();
    req_cfg = gpiod_request_config_new();
    if (!settings || !line_cfg || !req_cfg) {
        fprintf(stderr, "pitg-cue-buttons: failed to allocate gpiod settings/config\n");
        goto out;
    }

    if (gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_INPUT) < 0 ||
        gpiod_line_settings_set_bias(settings, GPIOD_LINE_BIAS_PULL_UP) < 0) {
        fprintf(stderr, "pitg-cue-buttons: failed to set input line settings: %s\n", strerror(errno));
        goto out;
    }

    offset = (unsigned int)gpio;
    if (gpiod_line_config_add_line_settings(line_cfg, &offset, 1, settings) < 0) {
        fprintf(stderr, "pitg-cue-buttons: failed to add line settings for GPIO %d: %s\n",
                gpio, strerror(errno));
        goto out;
    }

    gpiod_request_config_set_consumer(req_cfg, consumer);
    req = gpiod_chip_request_lines(chip, req_cfg, line_cfg);
    if (!req) {
        fprintf(stderr, "pitg-cue-buttons: failed to request GPIO %d: %s\n", gpio, strerror(errno));
        goto out;
    }

out:
    if (settings) gpiod_line_settings_free(settings);
    if (line_cfg) gpiod_line_config_free(line_cfg);
    if (req_cfg) gpiod_request_config_free(req_cfg);
    return req;
}

static int is_pressed(int raw_value, int active_high)
{
    if (active_high)
        return raw_value == 1;
    return raw_value == 0;
}

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)(ts.tv_sec * 1000L + ts.tv_nsec / 1000000L);
}

int main(int argc, char **argv)
{
    options_t opt = {
        .next_gpio = DEFAULT_NEXT_GPIO,
        .prev_gpio = DEFAULT_PREV_GPIO,
        .chip_index = DEFAULT_CHIP_INDEX,
        .baud = DEFAULT_BAUD,
        .debounce_ms = DEFAULT_DEBOUNCE_MS,
        .active_high = 0,
        .serial_dev = DEFAULT_SERIAL,
    };
    int test_mode = 0;

    int c;
    while ((c = getopt(argc, argv, "N:P:S:B:D:c:HTh")) != -1) {
        switch (c) {
        case 'N':
            opt.next_gpio = parse_int_arg(optarg, 0, 53);
            if (opt.next_gpio < 0) return 1;
            break;
        case 'P':
            opt.prev_gpio = parse_int_arg(optarg, 0, 53);
            if (opt.prev_gpio < 0) return 1;
            break;
        case 'S':
            opt.serial_dev = optarg;
            break;
        case 'B':
            opt.baud = parse_int_arg(optarg, 300, 1000000);
            if (opt.baud < 0) return 1;
            break;
        case 'D':
            opt.debounce_ms = parse_int_arg(optarg, 1, 5000);
            if (opt.debounce_ms < 0) return 1;
            break;
        case 'c':
            opt.chip_index = parse_int_arg(optarg, 0, 15);
            if (opt.chip_index < 0) return 1;
            break;
        case 'H':
            opt.active_high = 1;
            break;
        case 'T':
            test_mode = 1;
            break;
        case 'h':
            usage(argv[0]);
            return 0;
        default:
            usage(argv[0]);
            return 1;
        }
    }

    if (opt.next_gpio == opt.prev_gpio) {
        fprintf(stderr, "pitg-cue-buttons: NEXT and PREV pins must be different\n");
        return 1;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    int serial_fd = open_serial(opt.serial_dev, opt.baud);
    if (serial_fd < 0)
        return 1;

    char chip_path[64];
    snprintf(chip_path, sizeof(chip_path), "/dev/gpiochip%d", opt.chip_index);
    struct gpiod_chip *chip = gpiod_chip_open(chip_path);
    if (!chip) {
        fprintf(stderr, "pitg-cue-buttons: failed to open gpiochip%d: %s\n",
                opt.chip_index, strerror(errno));
        close(serial_fd);
        return 1;
    }

    struct gpiod_line_request *req_next = request_input(chip, opt.next_gpio, "pitg-cue-buttons-next");
    struct gpiod_line_request *req_prev = request_input(chip, opt.prev_gpio, "pitg-cue-buttons-prev");
    if (!req_next || !req_prev) {
        if (req_next) gpiod_line_request_release(req_next);
        if (req_prev) gpiod_line_request_release(req_prev);
        gpiod_chip_close(chip);
        close(serial_fd);
        return 1;
    }

    long last_next = 0;
    long last_prev = 0;

    int prev_next_state = gpiod_line_request_get_value(req_next, (unsigned int)opt.next_gpio);
    int prev_prev_state = gpiod_line_request_get_value(req_prev, (unsigned int)opt.prev_gpio);

    fprintf(stderr,
            "pitg-cue-buttons: serial=%s baud=%d next=%d prev=%d debounce=%dms active_%s%s\n",
            opt.serial_dev, opt.baud,
            opt.next_gpio, opt.prev_gpio,
            opt.debounce_ms,
            opt.active_high ? "high" : "low",
            test_mode ? " TEST" : "");
    if (test_mode)
        fprintf(stderr, "pitg-cue-buttons: test mode: auto-fire every 10s (next/prev alternating)\n");

    long last_test_fire = now_ms();
    int test_toggle = 0; /* 0=next, 1=prev */

    while (g_running) {
        int next_state = gpiod_line_request_get_value(req_next, (unsigned int)opt.next_gpio);
        int prev_state = gpiod_line_request_get_value(req_prev, (unsigned int)opt.prev_gpio);
        long t = now_ms();

        if (next_state >= 0 && prev_next_state >= 0) {
            int now_pressed = is_pressed(next_state, opt.active_high);
            int was_pressed = is_pressed(prev_next_state, opt.active_high);
            if (now_pressed && !was_pressed && (t - last_next) >= opt.debounce_ms) {
                if (send_cue(serial_fd, 0x0F) == 0) {
                    fprintf(stderr, "pitg-cue-buttons: NEXT -> 0x0F\n");
                    last_next = t;
                }
            }
        }

        if (prev_state >= 0 && prev_prev_state >= 0) {
            int now_pressed = is_pressed(prev_state, opt.active_high);
            int was_pressed = is_pressed(prev_prev_state, opt.active_high);
            if (now_pressed && !was_pressed && (t - last_prev) >= opt.debounce_ms) {
                if (send_cue(serial_fd, 0x1F) == 0) {
                    fprintf(stderr, "pitg-cue-buttons: PREV -> 0x1F\n");
                    last_prev = t;
                }
            }
        }

        prev_next_state = next_state;
        prev_prev_state = prev_state;

        /* Test mode: auto-fire every 10s, alternating next/prev */
        if (test_mode) {
            long now = now_ms();
            if (now - last_test_fire >= 10000L) {
                last_test_fire = now;
                uint8_t byte = test_toggle ? 0x1F : 0x0F;
                if (send_cue(serial_fd, byte) == 0) {
                    fprintf(stderr, "pitg-cue-buttons: TEST auto-fire %s -> 0x%02X\n",
                            test_toggle ? "prev" : "next", byte);
                }
                test_toggle = !test_toggle;
            }
        }

        usleep(5000);
    }

    gpiod_line_request_release(req_next);
    gpiod_line_request_release(req_prev);
    gpiod_chip_close(chip);
    close(serial_fd);
    fprintf(stderr, "pitg-cue-buttons: stopped\n");
    return 0;
}
