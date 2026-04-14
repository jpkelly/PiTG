/*
 * pitg_gpio.c - Limitimer/PerfectCue transmitter for Raspberry Pi
 *
 * Supported transport split:
 * - Limitimer over hardware UART (-u /dev/ttyAMA0 recommended on Pi 1)
 * - PerfectCue over GPIO bit-banged serial (occasional one-shot events)
 *
 * Limitimer can still be sent via GPIO when -u is not provided.
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

#define DEFAULT_GPIO_LIMITIMER 17
#define DEFAULT_GPIO_PERFECTCUE 18
#define DEFAULT_CHIP_INDEX 0
#define DEFAULT_BAUD 19200
#define DEFAULT_INTERVAL_MS 1000
#define DEFAULT_TOTAL_SECONDS 600  /* 10:00 */
#define DEFAULT_SUMUP_SECONDS 60   /* warning at 1:00 remaining */

static volatile sig_atomic_t g_running = 1;
static volatile sig_atomic_t g_paused  = 0;
static volatile sig_atomic_t g_reset   = 0;
static volatile sig_atomic_t g_sigterm_pid = 0;
static volatile sig_atomic_t g_sigterm_uid = 0;

typedef enum {
    PROTO_LIMITIMER = 0,
    PROTO_PERFECTCUE,
    PROTO_BOTH,
} protocol_mode_t;

typedef enum {
    PC_NEXT = 0,
    PC_PREV,
    PC_BLANK_OFF,
    PC_BLANK_ON,
} perfectcue_cmd_t;

typedef struct {
    int pin;
    struct gpiod_line_request *req;
} gpio_output_t;

static void on_signal(int sig)
{
    (void)sig;
    g_running = 0;
}

static void on_sigterm(int sig, siginfo_t *info, void *ctx)
{
    (void)sig;
    (void)ctx;
    if (info) {
        g_sigterm_pid = (sig_atomic_t)info->si_pid;
        g_sigterm_uid = (sig_atomic_t)info->si_uid;
    }
    g_running = 0;
}

static void on_pause(int sig)
{
    (void)sig;
    g_paused = !g_paused;
}

static void on_reset(int sig)
{
    (void)sig;
    g_reset = 1;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [-p protocol] [-u limitimer_uart] [-g limitimer_gpio] [-q perfectcue_gpio]\n"
            "          [-x pc_cmd] [-b baud] [-i interval_ms] [-c chip_index] [-t total] [-T] [-1]\n"
            "  -p  protocol: limitimer | perfectcue | both (default: both)\n"
            "  -u  UART device for limitimer output (example: /dev/ttyAMA0)\n"
            "  -g  GPIO pin for limitimer output (default: 17)\n"
            "  -q  GPIO pin for perfectcue output (default: 18)\n"
            "  -x  perfectcue command: next | prev | blank-off | blank-on (default: next)\n"
            "  -b  UART bit rate in baud (default: 19200)\n"
            "  -i  frame interval in milliseconds (default: 1000)\n"
            "  -c  GPIO chip index (default: 0 -> /dev/gpiochip0)\n"
            "  -t  countdown total time as MM:SS or seconds (default: 10:00)\n"
            "  -T  test mode: auto-fire PerfectCue every 10s, alternating next/prev\n"
            "  -1  oneshot: send one frame/event and exit\n"
            "Signals: SIGUSR1 = toggle pause/resume, SIGUSR2 = reset to start\n",
            prog);
}

static protocol_mode_t parse_protocol(const char *s)
{
    if (strcmp(s, "limitimer") == 0) return PROTO_LIMITIMER;
    if (strcmp(s, "perfectcue") == 0) return PROTO_PERFECTCUE;
    if (strcmp(s, "both") == 0) return PROTO_BOTH;
    return -1;
}

static int parse_time_arg(const char *s)
{
    int m, sec;
    if (sscanf(s, "%d:%d", &m, &sec) == 2) {
        if (m < 0 || sec < 0 || sec >= 60) return -1;
        return m * 60 + sec;
    }
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (!s || *s == '\0' || !end || *end != '\0') return -1;
    return (v > 0 && v <= 86400) ? (int)v : -1;
}

static int parse_int_arg(const char *s, int minv, int maxv)
{
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (!s || *s == '\0' || !end || *end != '\0') return -1;
    if (v < minv || v > maxv) return -1;
    return (int)v;
}

static int parse_perfectcue_cmd(const char *s, perfectcue_cmd_t *cmd)
{
    if (strcmp(s, "next") == 0) {
        *cmd = PC_NEXT;
        return 0;
    }
    if (strcmp(s, "prev") == 0) {
        *cmd = PC_PREV;
        return 0;
    }
    if (strcmp(s, "blank-off") == 0) {
        *cmd = PC_BLANK_OFF;
        return 0;
    }
    if (strcmp(s, "blank-on") == 0) {
        *cmd = PC_BLANK_ON;
        return 0;
    }
    return -1;
}

static void timespec_add_ns(struct timespec *ts, long ns)
{
    ts->tv_nsec += ns;
    while (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000L;
    }
}

static int line_write(gpio_output_t *out, int value)
{
    if (gpiod_line_request_set_value(out->req, (unsigned int)out->pin, value) < 0) {
        fprintf(stderr, "pitg-gpio: gpio write failed: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

static int uart_send_byte(gpio_output_t *out, uint8_t b, long bit_ns)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0) {
        fprintf(stderr, "pitg-gpio: clock_gettime failed: %s\n", strerror(errno));
        return -1;
    }

    /* Start bit (low) */
    if (line_write(out, 0) < 0) return -1;
    timespec_add_ns(&t, bit_ns);
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL);

    /* 8 data bits, LSB first */
    for (int i = 0; i < 8; i++) {
        int bit = (b >> i) & 0x01;
        if (line_write(out, bit) < 0) return -1;
        timespec_add_ns(&t, bit_ns);
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL);
    }

    /* Stop bit (high) */
    if (line_write(out, 1) < 0) return -1;
    timespec_add_ns(&t, bit_ns);
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL);
    return 0;
}

static int uart_send_bytes(gpio_output_t *out, const uint8_t *bytes,
                           size_t len, long bit_ns)
{
    for (size_t i = 0; i < len; i++) {
        if (uart_send_byte(out, bytes[i], bit_ns) < 0)
            return -1;
    }
    return 0;
}

static uint16_t crc16_modbus(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 0x0001) {
                crc = (uint16_t)((crc >> 1) ^ 0xA001);
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

static void encode_limitimer_time(int seconds, uint8_t out[3])
{
    if (seconds < 0) seconds = 0;
    out[0] = (uint8_t)((seconds / (128 * 128)) % 128);
    out[1] = (uint8_t)((seconds / 128) % 128);
    out[2] = (uint8_t)(seconds % 128);
}

static size_t build_limitimer_status_packet(uint8_t packet[55], uint8_t seq,
                                            uint8_t config_low,
                                            int total_s, int sumup_s,
                                            int elapsed_s)
{
    packet[0] = 0x81;
    packet[1] = 0x00;
    packet[2] = 0x00;
    packet[3] = config_low;
    packet[4] = seq & 0x7F; /* payload bytes must be 7-bit (< 0x80); seq wraps 0-127 */
    packet[5] = 0x00; /* selected timer: program 1 */

    for (int i = 0; i < 4; i++) {
        int base = 6 + (11 * i);

        packet[base + 0] = g_paused ? 0x00 : 0x01; /* run flag: 0=paused, 1=running */
        packet[base + 1] = 0x00; /* unknown byte in reverse-engineered format */

        encode_limitimer_time(total_s,   &packet[base + 2]);
        encode_limitimer_time(sumup_s,   &packet[base + 5]);
        encode_limitimer_time(elapsed_s, &packet[base + 8]);
    }

    packet[50] = 0x00;
    packet[51] = 0x83;

    {
        uint16_t crc = crc16_modbus(packet, 52);
        packet[52] = (uint8_t)(crc >> 8);
        packet[53] = (uint8_t)(crc & 0xFF);
    }
    packet[54] = 0xFF;
    return 55;
}

static uint8_t perfectcue_command_byte(perfectcue_cmd_t cmd)
{
    switch (cmd) {
    case PC_NEXT:
        return 0x0F;
    case PC_PREV:
        return 0x1F;
    case PC_BLANK_OFF:
        return 0x2F;
    case PC_BLANK_ON:
        return 0x3F;
    }
    return 0x0F;
}

static int request_output_line(struct gpiod_chip *chip, int pin, gpio_output_t *out)
{
    struct gpiod_line_settings *settings = NULL;
    struct gpiod_line_config *line_cfg = NULL;
    struct gpiod_request_config *req_cfg = NULL;
    unsigned int offset;

    out->pin = pin;
    out->req = NULL;

    settings = gpiod_line_settings_new();
    line_cfg = gpiod_line_config_new();
    req_cfg = gpiod_request_config_new();
    if (!settings || !line_cfg || !req_cfg) {
        fprintf(stderr, "pitg-gpio: failed to allocate gpiod settings/config\n");
        if (settings) gpiod_line_settings_free(settings);
        if (line_cfg) gpiod_line_config_free(line_cfg);
        if (req_cfg) gpiod_request_config_free(req_cfg);
        return -1;
    }

    if (gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_OUTPUT) < 0 ||
        gpiod_line_settings_set_output_value(settings, GPIOD_LINE_VALUE_ACTIVE) < 0) {
        fprintf(stderr, "pitg-gpio: failed to configure line settings: %s\n", strerror(errno));
        gpiod_line_settings_free(settings);
        gpiod_line_config_free(line_cfg);
        gpiod_request_config_free(req_cfg);
        return -1;
    }

    offset = (unsigned int)pin;
    if (gpiod_line_config_add_line_settings(line_cfg, &offset, 1, settings) < 0) {
        fprintf(stderr, "pitg-gpio: failed to add line settings for GPIO %d: %s\n", pin, strerror(errno));
        gpiod_line_settings_free(settings);
        gpiod_line_config_free(line_cfg);
        gpiod_request_config_free(req_cfg);
        return -1;
    }

    gpiod_request_config_set_consumer(req_cfg, "pitg-gpio");
    out->req = gpiod_chip_request_lines(chip, req_cfg, line_cfg);
    if (!out->req) {
        fprintf(stderr, "pitg-gpio: failed to request GPIO line %d as output: %s\n",
                pin, strerror(errno));
        gpiod_line_settings_free(settings);
        gpiod_line_config_free(line_cfg);
        gpiod_request_config_free(req_cfg);
        return -1;
    }

    gpiod_line_settings_free(settings);
    gpiod_line_config_free(line_cfg);
    gpiod_request_config_free(req_cfg);
    return 0;
}

static speed_t baud_to_speed(int baud)
{
    switch (baud) {
    case 9600: return B9600;
    case 19200: return B19200;
    case 38400: return B38400;
    case 57600: return B57600;
    case 115200: return B115200;
    default: return 0;
    }
}

static int open_uart_port(const char *dev, int baud)
{
    speed_t speed;
    int fd;
    struct termios tio;

    speed = baud_to_speed(baud);
    if (speed == 0) {
        fprintf(stderr, "pitg-gpio: unsupported UART baud %d (use 9600/19200/38400/57600/115200)\n", baud);
        return -1;
    }

    fd = open(dev, O_RDWR | O_NOCTTY);
    if (fd < 0) {
        fprintf(stderr, "pitg-gpio: failed to open UART %s: %s\n", dev, strerror(errno));
        return -1;
    }

    if (tcgetattr(fd, &tio) != 0) {
        fprintf(stderr, "pitg-gpio: tcgetattr(%s) failed: %s\n", dev, strerror(errno));
        close(fd);
        return -1;
    }

    cfmakeraw(&tio);
    tio.c_cflag |= (CLOCAL | CREAD);
    tio.c_cflag &= ~PARENB;
    tio.c_cflag &= ~CSTOPB;
    tio.c_cflag &= ~CSIZE;
    tio.c_cflag |= CS8;

    if (cfsetispeed(&tio, speed) != 0 || cfsetospeed(&tio, speed) != 0) {
        fprintf(stderr, "pitg-gpio: setting UART speed failed for %s: %s\n", dev, strerror(errno));
        close(fd);
        return -1;
    }

    if (tcsetattr(fd, TCSANOW, &tio) != 0) {
        fprintf(stderr, "pitg-gpio: tcsetattr(%s) failed: %s\n", dev, strerror(errno));
        close(fd);
        return -1;
    }

    return fd;
}

static int write_all(int fd, const uint8_t *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

int main(int argc, char **argv)
{
    protocol_mode_t protocol = PROTO_BOTH;
    const char *limitimer_uart = NULL;
    int gpio_limitimer = DEFAULT_GPIO_LIMITIMER;
    int gpio_perfectcue = DEFAULT_GPIO_PERFECTCUE;
    int baud = DEFAULT_BAUD;
    int interval_ms = DEFAULT_INTERVAL_MS;
    int chip_index = DEFAULT_CHIP_INDEX;
    int total_seconds = DEFAULT_TOTAL_SECONDS;
    int sumup_seconds = DEFAULT_SUMUP_SECONDS;
    int oneshot = 0;
    int test_mode = 0;
    perfectcue_cmd_t pc_cmd = PC_NEXT;
    int opt;

    while ((opt = getopt(argc, argv, "p:u:g:q:x:b:i:c:t:T1h")) != -1) {
        switch (opt) {
        case 'p': {
            protocol_mode_t p = parse_protocol(optarg);
            if ((int)p < 0) {
                fprintf(stderr, "pitg-gpio: invalid protocol '%s'\n", optarg);
                usage(argv[0]);
                return 1;
            }
            protocol = p;
            break;
        }
        case 'u':
            limitimer_uart = optarg;
            break;
        case 'g':
            gpio_limitimer = parse_int_arg(optarg, 0, 53);
            if (gpio_limitimer < 0) {
                fprintf(stderr, "pitg-gpio: invalid limitimer GPIO '%s'\n", optarg);
                return 1;
            }
            break;
        case 'q':
            gpio_perfectcue = parse_int_arg(optarg, 0, 53);
            if (gpio_perfectcue < 0) {
                fprintf(stderr, "pitg-gpio: invalid perfectcue GPIO '%s'\n", optarg);
                return 1;
            }
            break;
        case 'x':
            if (parse_perfectcue_cmd(optarg, &pc_cmd) != 0) {
                fprintf(stderr, "pitg-gpio: invalid perfectcue command '%s'\n", optarg);
                return 1;
            }
            break;
        case 'b':
            baud = parse_int_arg(optarg, 300, 1000000);
            if (baud < 0) {
                fprintf(stderr, "pitg-gpio: invalid baud '%s'\n", optarg);
                return 1;
            }
            break;
        case 'i':
            interval_ms = parse_int_arg(optarg, 1, 60000);
            if (interval_ms < 0) {
                fprintf(stderr, "pitg-gpio: invalid interval '%s'\n", optarg);
                return 1;
            }
            break;
        case 'c':
            chip_index = parse_int_arg(optarg, 0, 15);
            if (chip_index < 0) {
                fprintf(stderr, "pitg-gpio: invalid chip index '%s'\n", optarg);
                return 1;
            }
            break;
        case 't':
            total_seconds = parse_time_arg(optarg);
            if (total_seconds < 1) {
                fprintf(stderr, "pitg-gpio: invalid total time '%s' (use MM:SS or seconds)\n", optarg);
                return 1;
            }
            /* Keep sumup sensible relative to total */
            if (sumup_seconds >= total_seconds)
                sumup_seconds = total_seconds > 60 ? 60 : total_seconds / 2;
            break;
        case 'T':
            test_mode = 1;
            break;
        case '1':
            oneshot = 1;
            break;
        case 'h':
            usage(argv[0]);
            return 0;
        default:
            usage(argv[0]);
            return 1;
        }
    }

    {
        int limitimer_enabled = (protocol == PROTO_LIMITIMER || protocol == PROTO_BOTH);
        int perfectcue_enabled = (protocol == PROTO_PERFECTCUE || protocol == PROTO_BOTH);
        int limitimer_uses_gpio = limitimer_enabled && (limitimer_uart == NULL);
        int perfectcue_uses_gpio = perfectcue_enabled;

        if (limitimer_uses_gpio && perfectcue_uses_gpio && gpio_limitimer == gpio_perfectcue) {
            fprintf(stderr,
                    "pitg-gpio: GPIO pin conflict: limitimer and perfectcue cannot share the same pin\n");
            return 1;
        }
    }

    if (protocol == PROTO_PERFECTCUE && limitimer_uart != NULL) {
        fprintf(stderr, "pitg-gpio: warning: -u is ignored when protocol is perfectcue only\n");
    }

    if (protocol == PROTO_BOTH && limitimer_uart == NULL) {
        fprintf(stderr,
                "pitg-gpio: info: no -u provided; limitimer will be sent via GPIO bit-banging\n");
    }

    long bit_ns = 1000000000L / baud;

    signal(SIGINT,  on_signal);
    {
        struct sigaction sa;
        sa.sa_sigaction = on_sigterm;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_SIGINFO;
        sigaction(SIGTERM, &sa, NULL);
    }
    signal(SIGUSR1, on_pause);
    signal(SIGUSR2, on_reset);

    struct gpiod_chip *chip = NULL;
    gpio_output_t out_lt = {0};
    gpio_output_t out_pc = {0};
    int limitimer_uart_fd = -1;
    int need_limitimer = (protocol == PROTO_LIMITIMER || protocol == PROTO_BOTH);
    int need_perfectcue = (protocol == PROTO_PERFECTCUE || protocol == PROTO_BOTH) || test_mode;
    int need_limitimer_gpio = need_limitimer && (limitimer_uart == NULL);
    int need_any_gpio = need_limitimer_gpio || need_perfectcue;

    if (need_limitimer && limitimer_uart != NULL) {
        limitimer_uart_fd = open_uart_port(limitimer_uart, baud);
        if (limitimer_uart_fd < 0)
            return 1;
    }

    if (need_any_gpio) {
        char chip_path[64];
        snprintf(chip_path, sizeof(chip_path), "/dev/gpiochip%d", chip_index);
        chip = gpiod_chip_open(chip_path);
        if (!chip) {
            fprintf(stderr, "pitg-gpio: failed to open gpiochip%d: %s\n",
                    chip_index, strerror(errno));
            if (limitimer_uart_fd >= 0)
                close(limitimer_uart_fd);
            return 1;
        }
    }

    if (need_limitimer_gpio &&
        request_output_line(chip, gpio_limitimer, &out_lt) < 0) {
        if (chip) gpiod_chip_close(chip);
        if (limitimer_uart_fd >= 0) close(limitimer_uart_fd);
        return 1;
    }

    if (need_perfectcue &&
        request_output_line(chip, gpio_perfectcue, &out_pc) < 0) {
        if (out_lt.req) gpiod_line_request_release(out_lt.req);
        if (chip) gpiod_chip_close(chip);
        if (limitimer_uart_fd >= 0) close(limitimer_uart_fd);
        return 1;
    }

    fprintf(stderr,
            "pitg-gpio: protocol=%s chip=gpiochip%d baud=%d interval=%dms\n",
            protocol == PROTO_LIMITIMER ? "limitimer" :
            protocol == PROTO_PERFECTCUE ? "perfectcue" : "both",
            chip_index, baud, interval_ms);
    if (oneshot)
        fprintf(stderr, "pitg-gpio: oneshot mode enabled\n");

    if (limitimer_uart_fd >= 0)
        fprintf(stderr, "pitg-gpio: limitimer UART=%s\n", limitimer_uart);
    if (out_lt.req)
        fprintf(stderr, "pitg-gpio: limitimer GPIO=%d\n", out_lt.pin);
    if (out_pc.req)
        fprintf(stderr, "pitg-gpio: perfectcue GPIO=%d\n", out_pc.pin);
    if (out_pc.req)
        fprintf(stderr, "pitg-gpio: perfectcue command=0x%02X\n",
                (unsigned int)perfectcue_command_byte(pc_cmd));
    if (need_limitimer)
        fprintf(stderr, "pitg-gpio: limitimer total=%d:%02d sumup=%d:%02d\n",
                total_seconds / 60, total_seconds % 60,
                sumup_seconds / 60, sumup_seconds % 60);
    if (test_mode)
        fprintf(stderr, "pitg-gpio: test mode enabled: PerfectCue auto-fire every 10s (next/prev alternating)\n");

    {
        uint8_t seq = 0;
        int elapsed = 0;
        long accum_ms = 0;
        unsigned long packets_sent = 0;
        long status_accum_ms = 0;
        const long status_interval_ms = 60000; /* log status every 60s */
        long test_pc_accum_ms = 0;
        const long test_pc_interval_ms = 10000; /* fire PerfectCue every 10s in test mode */
        int test_pc_toggle = 0; /* 0=next, 1=prev */
        struct timespec next;
        clock_gettime(CLOCK_MONOTONIC, &next);

    while (g_running) {
        uint8_t frame[64];
        size_t frame_len = 0;

        if (need_limitimer) {
            /* Countdown: configCountUp(0x08) → display shows total-elapsed (down arrow)
             * configProgHours(0x04) → MM:SS display format */
            uint8_t config_low = 0x0D; /* configCountUp(0x08) | configProgHours(0x04) | 0x01 */
            frame_len = build_limitimer_status_packet(frame, seq, config_low,
                                                      total_seconds, sumup_seconds,
                                                      elapsed);
            if (limitimer_uart_fd >= 0) {
                if (write_all(limitimer_uart_fd, frame, frame_len) < 0) {
                    fprintf(stderr, "pitg-gpio: UART write error (seq=%u packets_sent=%lu): %s\n",
                            (unsigned)seq, packets_sent, strerror(errno));
                    break;
                }
            } else if (out_lt.req) {
                if (uart_send_bytes(&out_lt, frame, frame_len, bit_ns) < 0)
                    break;
            }
        }

        if (out_pc.req && !test_mode) {
            frame[0] = perfectcue_command_byte(pc_cmd);
            if (uart_send_bytes(&out_pc, frame, 1, bit_ns) < 0)
                break;
        }

        if (oneshot)
            break;

        packets_sent++;
        seq++;
        if (g_reset) {
            elapsed = 0;
            accum_ms = 0;
            g_paused = 0;
            g_reset = 0;
            fprintf(stderr, "pitg-gpio: timer reset (packets_sent=%lu)\n", packets_sent);
        }
        if (!g_paused) {
            accum_ms += interval_ms;
            if (accum_ms >= 1000) {
                accum_ms -= 1000;
                elapsed++;
                if (elapsed >= total_seconds)
                    elapsed = 0; /* loop back to start */
            }
        }

        /* Test mode: auto-fire PerfectCue every 10s, alternating next/prev */
        if (test_mode && out_pc.req) {
            test_pc_accum_ms += interval_ms;
            if (test_pc_accum_ms >= test_pc_interval_ms) {
                test_pc_accum_ms -= test_pc_interval_ms;
                perfectcue_cmd_t fire_cmd = test_pc_toggle ? PC_PREV : PC_NEXT;
                uint8_t pc_frame[1];
                pc_frame[0] = perfectcue_command_byte(fire_cmd);
                fprintf(stderr, "pitg-gpio: test mode firing PerfectCue %s\n",
                        test_pc_toggle ? "prev" : "next");
                if (uart_send_bytes(&out_pc, pc_frame, 1, bit_ns) < 0)
                    break;
                test_pc_toggle = !test_pc_toggle;
            }
        }

        /* Periodic status log */
        status_accum_ms += interval_ms;
        if (status_accum_ms >= status_interval_ms) {
            status_accum_ms -= status_interval_ms;
            int remaining = total_seconds - elapsed;
            fprintf(stderr,
                    "pitg-gpio: status seq=%u elapsed=%d remaining=%d:%02d "
                    "packets_sent=%lu paused=%d\n",
                    (unsigned)seq, elapsed,
                    remaining / 60, remaining % 60,
                    packets_sent, (int)g_paused);
        }

        /* Advance deadline by interval_ms; sleep until that absolute time.
         * This keeps the period accurate even if write_all stalls. */
        next.tv_nsec += (long)interval_ms * 1000000L;
        if (next.tv_nsec >= 1000000000L) {
            next.tv_nsec -= 1000000000L;
            next.tv_sec++;
        }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
    }
    }

    if (out_lt.req) {
        line_write(&out_lt, 1);
        gpiod_line_request_release(out_lt.req);
    }
    if (out_pc.req) {
        line_write(&out_pc, 1);
        gpiod_line_request_release(out_pc.req);
    }
    if (chip)
        gpiod_chip_close(chip);
    if (limitimer_uart_fd >= 0)
        close(limitimer_uart_fd);

    if (g_sigterm_pid != 0) {
        fprintf(stderr, "pitg-gpio: stopped by SIGTERM from pid=%d uid=%d\n",
                (int)g_sigterm_pid, (int)g_sigterm_uid);
    } else {
        fprintf(stderr, "pitg-gpio: stopped\n");
    }
    return 0;
}
