/*
 * Bench-only "pb_join" console command (CONFIG_POWER_BASE_BENCH_JOINER_CONSOLE,
 * set only by power_base/sdkconfig.bench_joiner). Development/physical-trial
 * path: the PSKd shown by the app's DEBUG enrollment test is typed on this
 * board's USB-Serial/JTAG console. Not the product transport (BLE/QR).
 *
 *   pb_join status          state, last result, this board's EUI-64
 *   pb_join start           prompts for the PSKd WITHOUT echo, starts the Joiner
 *   pb_join cancel          stops a running Joiner / attach attempt
 *   pb_join forget <XXXX>   XXXX = last 4 hex digits of this board's EUI-64:
 *                           erases the enrollment marker and OpenThread's
 *                           stored Dataset on THIS board, then restarts
 *
 * The PSKd is never a command argument: esp_console keeps every command line
 * in its RAM history (linenoiseHistoryAdd), so `pb_join start` reads it
 * separately, straight from stdin, without echo and without history. Its
 * buffer is wiped on every path. Limits that remain: the characters travel
 * over USB in clear; a terminal with LOCAL echo turned on would still show
 * them; OpenThread keeps its own copy inside the Joiner/DTLS objects until
 * they are reused.
 */
#include "enrollment_runtime.h"

#if CONFIG_POWER_BASE_BENCH_JOINER_CONSOLE

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <unistd.h>
#include "esp_console.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define PSKD_INPUT_TIMEOUT_MS 90000

typedef enum {
    HIDDEN_OK = 0,
    HIDDEN_ABORTED,
    HIDDEN_TIMEOUT,
    HIDDEN_TOO_LONG,
    HIDDEN_IO_ERROR,
} hidden_result_t;

/* Reads one line from stdin into buf (NUL-terminated) with no echo. Leading
 * empty lines (the tail of the command's own CR/LF) are skipped. a-z is
 * upper-cased (a Thread PSKd is upper case). */
static hidden_result_t read_hidden_line(char buf[PB_ENROLL_PSKD_BUF_LEN])
{
    memset(buf, 0, PB_ENROLL_PSKD_BUF_LEN);
    int fd = fileno(stdin);
    size_t len = 0;
    bool overflow = false;
    TickType_t start = xTaskGetTickCount();
    hidden_result_t result = HIDDEN_IO_ERROR;

    while (1) {
        if ((xTaskGetTickCount() - start) * portTICK_PERIOD_MS >= PSKD_INPUT_TIMEOUT_MS) {
            result = HIDDEN_TIMEOUT;
            break;
        }
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
        int s = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (s < 0) {
            if (errno == EINTR) {
                continue;
            }
            result = HIDDEN_IO_ERROR;
            break;
        }
        if (s == 0) {
            continue;
        }
        char c = 0;
        int n = read(fd, &c, 1);
        if (n <= 0) {
            continue;
        }
        if (c == '\r' || c == '\n') {
            if (len == 0 && !overflow) {
                continue;
            }
            result = overflow ? HIDDEN_TOO_LONG : HIDDEN_OK;
            c = 0;
            break;
        }
        if (c == 0x03 || c == 0x1b) { /* Ctrl-C, Esc */
            result = HIDDEN_ABORTED;
            c = 0;
            break;
        }
        if (c == 0x08 || c == 0x7f) {
            if (len > 0) {
                buf[--len] = 0;
            }
            continue;
        }
        if (!isprint((unsigned char)c)) {
            continue;
        }
        if (len < PB_ENROLL_PSKD_MAX_LEN) {
            buf[len++] = (char)toupper((unsigned char)c);
        } else {
            overflow = true;
        }
        c = 0;
    }
    if (result != HIDDEN_OK) {
        pb_enroll_wipe(buf, PB_ENROLL_PSKD_BUF_LEN);
    }
    return result;
}

static void print_status(void)
{
    pb_enrollment_status_t st;
    pb_enrollment_get_status(&st);
    printf("pb_join: state=%s last=%s attempt=%u resumed=%s\n", pb_enroll_state_name(st.state),
           pb_enroll_result_name(st.last), (unsigned)st.attempt, st.resumed ? "yes" : "no");
    printf("pb_join: eui64=%02x%02x%02x%02x%02x%02x%02x%02x (enter this in the app DEBUG test)%s\n", st.eui64[0],
           st.eui64[1], st.eui64[2], st.eui64[3], st.eui64[4], st.eui64[5], st.eui64[6], st.eui64[7],
           st.joiner_eui64_matches ? "" : " -- WARNING: Joiner EUI-64 differs, Joiner refused");
    printf("pb_join: 'enrolled'/'resumed' means attached + marker stored; the app's heartbeat_received is decided "
           "by the hub alone\n");
}

static int cmd_start(void)
{
    pb_enrollment_status_t st;
    pb_enrollment_get_status(&st);
    if (st.state != PB_ENROLL_STATE_WAITING) {
        printf("pb_join: not waiting (state=%s) -- nothing started\n", pb_enroll_state_name(st.state));
        return 1;
    }

    char pskd[PB_ENROLL_PSKD_BUF_LEN];
    printf("PSKd from the app (NOT echoed; Enter = done, Ctrl-C = abort, %d s): ", PSKD_INPUT_TIMEOUT_MS / 1000);
    fflush(stdout);
    hidden_result_t r = read_hidden_line(pskd);
    printf("\n");
    switch (r) {
    case HIDDEN_OK: break;
    case HIDDEN_ABORTED: printf("pb_join: aborted -- nothing sent\n"); return 1;
    case HIDDEN_TIMEOUT: printf("pb_join: no input -- nothing sent\n"); return 1;
    case HIDDEN_TOO_LONG: printf("pb_join: longer than 32 characters -- nothing sent\n"); return 1;
    case HIDDEN_IO_ERROR:
    default: printf("pb_join: console read failed -- nothing sent\n"); return 1;
    }

    esp_err_t err = pb_enrollment_request_join(pskd); /* wipes pskd */
    pb_enroll_wipe(pskd, sizeof(pskd));
    switch (err) {
    case ESP_OK:
        printf("pb_join: Joiner started. Watch 'pb_join status'. Joiner success alone is not enrollment.\n");
        return 0;
    case ESP_ERR_INVALID_ARG:
        printf("pb_join: PSKd rejected (6-32 of 0-9 A-Z without I O Q Z) -- nothing sent\n");
        return 1;
    case ESP_ERR_INVALID_STATE:
        printf("pb_join: not waiting -- nothing started\n");
        return 1;
    case ESP_ERR_NOT_ALLOWED:
        /* cfg/pb_bridge's enrollment endpoint policy: this board must be given
         * the bridge endpoint with its enrollment, which the console cannot do
         * (or the policy is unusable). */
        printf("pb_join: refused -- this board's prepared image requires the bridge endpoint with the enrollment "
               "(BLE enrollment), or its endpoint policy is unusable -- nothing started, nothing erased\n");
        return 1;
    default:
        print_status();
        return 1;
    }
}

static int cmd_forget(const char *token)
{
    pb_enrollment_status_t st;
    pb_enrollment_get_status(&st);
    char expect[5];
    snprintf(expect, sizeof(expect), "%02x%02x", st.eui64[6], st.eui64[7]);
    if (token == NULL || strcasecmp(token, expect) != 0) {
        printf("pb_join: usage: pb_join forget <last 4 hex digits of this board's EUI-64> -- nothing erased\n");
        return 1;
    }
    esp_err_t err = pb_enrollment_forget();
    if (err != ESP_OK) {
        printf("pb_join: forget failed (%s) -- not restarting\n", esp_err_to_name(err));
        return 1;
    }
    printf("pb_join: marker and OpenThread Dataset erased on this board -- restarting\n");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return 0;
}

static int pb_join_cmd(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "status") == 0) {
        print_status();
        return 0;
    }
    if (strcmp(argv[1], "start") == 0 && argc == 2) {
        return cmd_start();
    }
    if (strcmp(argv[1], "cancel") == 0 && argc == 2) {
        esp_err_t err = pb_enrollment_cancel();
        printf("pb_join: %s\n", err == ESP_OK ? "cancelled -- Thread stopped, waiting" : "nothing to cancel");
        return err == ESP_OK ? 0 : 1;
    }
    if (strcmp(argv[1], "forget") == 0 && argc <= 3) {
        return cmd_forget(argc == 3 ? argv[2] : NULL);
    }
    /* Never echo the arguments back: someone may have typed a PSKd here. */
    printf("pb_join: usage: pb_join [status|start|cancel|forget <XXXX>] (the PSKd is never an argument)\n");
    return 1;
}

esp_err_t pb_enrollment_console_start(void)
{
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "pb>";
    repl_config.max_history_len = 8;
    esp_console_dev_usb_serial_jtag_config_t usb_config = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    esp_console_repl_t *repl = NULL;
    esp_err_t err = esp_console_new_repl_usb_serial_jtag(&usb_config, &repl_config, &repl);
    if (err != ESP_OK) {
        return err;
    }
    const esp_console_cmd_t cmd = {
        .command = "pb_join",
        .help = "Bench Power Base enrollment: status | start (PSKd prompted, not echoed) | cancel | forget <XXXX>",
        .hint = NULL,
        .func = pb_join_cmd,
    };
    err = esp_console_cmd_register(&cmd);
    if (err != ESP_OK) {
        return err;
    }
    return esp_console_start_repl(repl);
}

#endif /* CONFIG_POWER_BASE_BENCH_JOINER_CONSOLE */
