/*
 * pitg_cue_buttons.c - Two-button PerfectCue transmitter for Raspberry Pi
 *
 * Reads two GPIO button inputs and sends PerfectCue bytes on one GPIO output.
 * Default mapping:
 *   NEXT button  -> 0x0F
 *   PREV button  -> 0x1F
 */

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <gpiod.h>

#define DEFAULT_NEXT_GPIO 23
#define DEFAULT_PREV_GPIO 24
#define DEFAULT_TX_GPIO 18
#define DEFAULT_CHIP_INDEX 0
#define DEFAULT_BAUD 19200
#define DEFAULT_DEBOUNCE_MS 120

typedef struct {
    int next_gpio;
    int prev_gpio;
    int tx_gpio;
    int chip_index;
    int baud;
    int debounce_ms;
    int active_high;
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
            "Usage: %s [-N next_gpio] [-P prev_gpio] [-T tx_gpio]\\n"
            "          [-B baud] [-D debounce_ms] [-c chip_index] [-H]\\n\\n"
            "  -N  GPIO input pin for NEXT button (default: 23)\\n"
            "  -P  GPIO input pin for PREV button (default: 24)\\n"
            "  -T  GPIO output pin for PerfectCue TX (default: 18)\\n"
            "  -B  UART bit rate for TX output (default: 19200)\\n"
            "  -D  debounce time in ms (default: 120)\\n"
            "  -c  GPIO chip index (default: 0 -> /dev/gpiochip0)\\n"
            "  -H  buttons are active-high (default is active-low)\\n",
            prog);
}

static void timespec_add_ns(struct timespec *ts, long ns)
{
    ts->tv_nsec += ns;
    while (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000L;
    }
}

static int line_write(struct gpiod_line_request *req, int gpio, int value)
{
    if (gpiod_line_request_set_value(req, (unsigned int)gpio, value) < 0) {
        fprintf(stderr, "pitg-cue-buttons: gpio write failed: %s\\n", strerror(errno));
        return -1;
    }
    return 0;
}

static int uart_send_byte(struct gpiod_line_request *req, int gpio, uint8_t b, long bit_ns)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0) {
        fprintf(stderr, "pitg-cue-buttons: clock_gettime failed: %s\\n", strerror(errno));
        return -1;
    }

    if (line_write(req, gpio, 0) < 0) return -1;
    timespec_add_ns(&t, bit_ns);
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL);

    for (int i = 0; i < 8; i++) {
        int bit = (b >> i) & 0x01;
        if (line_write(req, gpio, bit) < 0) return -1;
        timespec_add_ns(&t, bit_ns);
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL);
    }

    if (line_write(req, gpio, 1) < 0) return -1;
    timespec_add_ns(&t, bit_ns);
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL);
    return 0;
}

static struct gpiod_line_request *request_line(struct gpiod_chip *chip, int gpio,
                                               int is_output, int default_high,
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

    if (is_output) {
        if (gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_OUTPUT) < 0 ||
            gpiod_line_settings_set_output_value(settings,
                default_high ? GPIOD_LINE_VALUE_ACTIVE : GPIOD_LINE_VALUE_INACTIVE) < 0) {
            fprintf(stderr, "pitg-cue-buttons: failed to set output line settings: %s\n", strerror(errno));
            goto out;
        }
    } else {
        if (gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_INPUT) < 0 ||
            gpiod_line_settings_set_bias(settings, GPIOD_LINE_BIAS_PULL_UP) < 0) {
            fprintf(stderr, "pitg-cue-buttons: failed to set input line settings: %s\n", strerror(errno));
            goto out;
        }
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
        .tx_gpio = DEFAULT_TX_GPIO,
        .chip_index = DEFAULT_CHIP_INDEX,
        .baud = DEFAULT_BAUD,
        .debounce_ms = DEFAULT_DEBOUNCE_MS,
        .active_high = 0,
    };

    int c;
    while ((c = getopt(argc, argv, "N:P:T:B:D:c:Hh")) != -1) {
        switch (c) {
        case 'N':
            opt.next_gpio = parse_int_arg(optarg, 0, 53);
            if (opt.next_gpio < 0) return 1;
            break;
        case 'P':
            opt.prev_gpio = parse_int_arg(optarg, 0, 53);
            if (opt.prev_gpio < 0) return 1;
            break;
        case 'T':
            opt.tx_gpio = parse_int_arg(optarg, 0, 53);
            if (opt.tx_gpio < 0) return 1;
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
        case 'h':
            usage(argv[0]);
            return 0;
        default:
            usage(argv[0]);
            return 1;
        }
    }

    if (opt.next_gpio == opt.prev_gpio ||
        opt.next_gpio == opt.tx_gpio ||
        opt.prev_gpio == opt.tx_gpio) {
        fprintf(stderr, "pitg-cue-buttons: button and TX pins must be unique\\n");
        return 1;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    char chip_path[64];
    snprintf(chip_path, sizeof(chip_path), "/dev/gpiochip%d", opt.chip_index);
    struct gpiod_chip *chip = gpiod_chip_open(chip_path);
    if (!chip) {
        fprintf(stderr, "pitg-cue-buttons: failed to open gpiochip%d: %s\\n",
                opt.chip_index, strerror(errno));
        return 1;
    }

    struct gpiod_line_request *req_next = NULL;
    struct gpiod_line_request *req_prev = NULL;
    struct gpiod_line_request *req_tx = NULL;

    req_next = request_line(chip, opt.next_gpio, 0, 0, "pitg-cue-buttons-next");
    req_prev = request_line(chip, opt.prev_gpio, 0, 0, "pitg-cue-buttons-prev");
    req_tx = request_line(chip, opt.tx_gpio, 1, 1, "pitg-cue-buttons-tx");
    if (!req_next || !req_prev || !req_tx) {
        if (req_next) gpiod_line_request_release(req_next);
        if (req_prev) gpiod_line_request_release(req_prev);
        if (req_tx) gpiod_line_request_release(req_tx);
        gpiod_chip_close(chip);
        return 1;
    }

    long bit_ns = 1000000000L / opt.baud;
    long last_next = 0;
    long last_prev = 0;

    int prev_next_state = gpiod_line_request_get_value(req_next, (unsigned int)opt.next_gpio);
    int prev_prev_state = gpiod_line_request_get_value(req_prev, (unsigned int)opt.prev_gpio);

    fprintf(stderr,
            "pitg-cue-buttons: chip=gpiochip%d next=%d prev=%d tx=%d baud=%d debounce=%dms active_%s\\n",
            opt.chip_index,
            opt.next_gpio,
            opt.prev_gpio,
            opt.tx_gpio,
            opt.baud,
            opt.debounce_ms,
            opt.active_high ? "high" : "low");

    while (g_running) {
        int next_state = gpiod_line_request_get_value(req_next, (unsigned int)opt.next_gpio);
        int prev_state = gpiod_line_request_get_value(req_prev, (unsigned int)opt.prev_gpio);
        long t = now_ms();

        if (next_state >= 0 && prev_next_state >= 0) {
            int now_pressed = is_pressed(next_state, opt.active_high);
            int was_pressed = is_pressed(prev_next_state, opt.active_high);
            if (now_pressed && !was_pressed && (t - last_next) >= opt.debounce_ms) {
                if (uart_send_byte(req_tx, opt.tx_gpio, 0x0F, bit_ns) == 0) {
                    fprintf(stderr, "pitg-cue-buttons: NEXT -> 0x0F\\n");
                    last_next = t;
                }
            }
        }

        if (prev_state >= 0 && prev_prev_state >= 0) {
            int now_pressed = is_pressed(prev_state, opt.active_high);
            int was_pressed = is_pressed(prev_prev_state, opt.active_high);
            if (now_pressed && !was_pressed && (t - last_prev) >= opt.debounce_ms) {
                if (uart_send_byte(req_tx, opt.tx_gpio, 0x1F, bit_ns) == 0) {
                    fprintf(stderr, "pitg-cue-buttons: PREV -> 0x1F\\n");
                    last_prev = t;
                }
            }
        }

        prev_next_state = next_state;
        prev_prev_state = prev_state;

        usleep(5000);
    }

    line_write(req_tx, opt.tx_gpio, 1);
    gpiod_line_request_release(req_next);
    gpiod_line_request_release(req_prev);
    gpiod_line_request_release(req_tx);
    gpiod_chip_close(chip);
    fprintf(stderr, "pitg-cue-buttons: stopped\\n");
    return 0;
}
