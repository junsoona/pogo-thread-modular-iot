/*
 * Host test: after an enrollment that delivered the Linux bridge endpoint,
 * and after every reboot, does the REAL exporter send to that endpoint --
 * and to nothing else when the stored record is damaged, foreign or for
 * another network?
 *
 * Linked code, all real and unmodified:
 *   components/power_base_enrollment/power_base_enrollment.c   (controller, record v1/v2)
 *   components/power_base_bridge_config/power_base_bridge_config.c
 *       (cfg/pb_bridge loader, power_base_bridge_config_apply_delivered())
 *   components/linux_bridge_exporter/linux_bridge_exporter.c + removed_journal.c
 *   components/power_base_transport and what it needs
 * against the NVS simulator (dynamic "nvs" partition persisted in a file,
 * "cfg" set up per boot like a flashed prepared image) and the OpenThread
 * CoAP simulator, which records the address and port of every send.
 *
 * power_base/main/app_main.c itself is ESP-IDF/OpenThread glue and cannot be
 * compiled here. boot() below is its mode 2 path in the same order:
 *   power_base_bridge_config_load()            app_main()
 *   power_base_bridge_config_load_enroll_policy()   app_main(), mode 2 branch
 *   controller: policy, boot rule, enrollment  enrollment_runtime.c
 *   on_enrollment_ready(): a delivered endpoint replaces the bridge config
 *       BEFORE the services start
 *   start_services_once(): linux_bridge_exporter_init() only if enabled
 * tools/test_power_base_ble_enrollment_paths.py checks that app_main.c has
 * that order.
 *
 * Each boot is a child process: the exporter's task and static state exist
 * once per process, like on the board, and whatever was not committed to the
 * simulated flash is gone at the next boot.
 *
 * Build & run (from the repo root):
 *   cc -Wall -Wextra -pthread \
 *      -I tools/host_test_stubs/pogo_fast_bridge_timers -I tools/host_test_stubs/nvs_sim -I tools/host_test_stubs \
 *      -I components/power_base_enrollment/include \
 *      -I components/linux_bridge_exporter/include -I components/power_base_transport/include \
 *      -I components/power_base_bridge_config/include -I components/bridge_export_proto/include \
 *      -I components/topology_registry/include -I components/topology_proto/include \
 *      -I components/sensor_channel/include -I components/sensor_root_aggregator/include \
 *      -I components/pogo_proto/include -I components/coap_response_timing/include \
 *      -o /tmp/test_power_base_enrollment_exporter_endpoint tools/test_power_base_enrollment_exporter_endpoint.c \
 *      components/power_base_enrollment/power_base_enrollment.c \
 *      components/power_base_bridge_config/power_base_bridge_config.c \
 *      components/linux_bridge_exporter/linux_bridge_exporter.c \
 *      components/linux_bridge_exporter/removed_journal.c \
 *      components/power_base_transport/power_base_transport.c \
 *      components/bridge_export_proto/bridge_export_proto.c \
 *      components/topology_registry/topology_registry.c components/topology_proto/topology_proto.c \
 *      components/sensor_channel/sensor_channel.c components/sensor_root_aggregator/sensor_root_aggregator.c \
 *      components/pogo_proto/pogo_crc16.c components/coap_response_timing/coap_response_timing.c \
 *      tools/host_test_stubs/host_pb_bridge_full_sim.c tools/host_test_stubs/nvs_sim/nvs_sim.c \
 *   && /tmp/test_power_base_enrollment_exporter_endpoint
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "host_pb_bridge_full_sim.h"
#include "linux_bridge_exporter/linux_bridge_exporter.h"
#include "nvs.h"
#include "nvs_sim.h"
#include "pogo_proto/pogo_crc16.h"
#include "power_base_bridge_config/power_base_bridge_config.h"
#include "power_base_enrollment/power_base_enrollment.h"
#include "power_base_transport/power_base_transport.h"

static int g_failures = 0;

#define CHECK(cond, msg)                                                                                             \
    do {                                                                                                             \
        if (!(cond)) {                                                                                               \
            printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);                                                   \
            g_failures++;                                                                                            \
        } else {                                                                                                     \
            printf("ok:   %s\n", msg);                                                                               \
        }                                                                                                            \
    } while (0)

/* Made-up identifiers; nothing here is a real board, network or host. */
static const uint8_t EUI_A[8] = {0x60, 0x55, 0xf9, 0xff, 0xfe, 0x00, 0x12, 0x34};
static const uint8_t EUI_B[8] = {0x60, 0x55, 0xf9, 0xff, 0xfe, 0x00, 0x56, 0x78};
static const pb_enroll_network_id_t NET_OTBR = {
    .ext_pan_id = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88},
    .mesh_local_prefix = {0xfd, 0x11, 0x22, 0x33, 0x44, 0x55, 0x00, 0x00},
};
static const pb_enroll_network_id_t NET_OTHER = {
    .ext_pan_id = {0x99, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22},
    .mesh_local_prefix = {0xfd, 0x99, 0x88, 0x77, 0x66, 0x55, 0x00, 0x00},
};
/* What the hub delivered with the enrollment. */
static const pb_enroll_bridge_endpoint_t EP_DELIVERED = {
    .addr = {0xfd, 0x11, 0x22, 0x33, 0x44, 0x55, 0x00, 0x00, 0x8c, 0x1f, 0x2e, 0x3d, 0x4c, 0x5b, 0x6a, 0x79},
    .port = 5701,
};
/* What an OLD prepared image carries in cfg/pb_bridge: another host, another
 * port -- so a send to it can never be mistaken for a send to the delivered
 * endpoint. */
static const uint8_t CFG_STATIC_ADDR[16] = {0xfd, 0x11, 0x22, 0x33, 0x44, 0x55, 0x00, 0x00,
                                            0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
#define CFG_STATIC_PORT 5684
static const char *PSKD = "J01NME7K";
#define URI_HEARTBEAT "pogo/v1/heartbeat"

/* ---- fake OpenThread under the real controller ---- */

typedef struct {
    bool has_key;
    bool complete;
    pb_enroll_network_id_t id;
    bool thread_enabled;
    uint32_t last_attempt;
} fake_ot_t;

static fake_ot_t g_ot;
static int g_joiner_starts;

static esp_err_t f_joiner_start(const char *pskd, uint32_t attempt, void *ctx)
{
    (void)pskd;
    (void)ctx;
    g_joiner_starts++;
    g_ot.last_attempt = attempt;
    return ESP_OK;
}

static void f_joiner_stop(void *ctx)
{
    (void)ctx;
}

static esp_err_t f_thread_start_stored(bool require_complete, void *ctx)
{
    (void)ctx;
    if (!g_ot.has_key || (require_complete && !g_ot.complete)) {
        return ESP_ERR_INVALID_STATE;
    }
    g_ot.thread_enabled = true;
    return ESP_OK;
}

static void f_thread_stop(void *ctx)
{
    (void)ctx;
    g_ot.thread_enabled = false;
}

static bool f_read_dataset(pb_enroll_dataset_info_t *out, void *ctx)
{
    (void)ctx;
    out->has_network_key = g_ot.has_key;
    out->complete = g_ot.complete;
    if (g_ot.complete) {
        out->id = g_ot.id;
    }
    return true;
}

static const pb_enroll_ops_t k_ops = {
    .joiner_start = f_joiner_start,
    .joiner_stop = f_joiner_stop,
    .thread_start_stored = f_thread_start_stored,
    .thread_stop = f_thread_stop,
    .read_dataset = f_read_dataset,
    .ctx = NULL,
};

/* ---- the prepared image's cfg/pb_bridge ---- */

typedef enum {
    CFG_NO_BRIDGE = 0,        /* no pb_bridge namespace at all */
    CFG_DISABLED,             /* exporter switched off, NO policy (enabled=0, policy byte 0) */
    CFG_STATIC,               /* an image prepared with --bridge-host/--bridge-port */
    CFG_ENDPOINT_REQUIRED,    /* --bridge-from-enrollment: enabled=0, policy byte 1 */
    CFG_POLICY_UNKNOWN_VALUE, /* enabled=0 with a policy byte this firmware does not know */
} cfg_kind_t;

static void put_cfg(cfg_kind_t kind)
{
    if (kind == CFG_NO_BRIDGE) {
        nvs_sim_create_namespace("cfg", "power_base_cfg");
        return;
    }
    uint8_t blob[24] = {0};
    blob[3] = 1; /* schema_version 1, big-endian */
    blob[4] = (kind == CFG_STATIC) ? 1 : 0;
    blob[5] = (kind == CFG_ENDPOINT_REQUIRED) ? 1 : (kind == CFG_POLICY_UNKNOWN_VALUE) ? 2 : 0;
    if (kind == CFG_STATIC) {
        memcpy(&blob[6], CFG_STATIC_ADDR, 16);
        blob[22] = (uint8_t)(CFG_STATIC_PORT >> 8);
        blob[23] = (uint8_t)(CFG_STATIC_PORT & 0xff);
    }
    nvs_sim_put_blob("cfg", "pb_bridge", "blob", blob, sizeof(blob));
    nvs_sim_put_u16("cfg", "pb_bridge", "crc", pogo_crc16_ccitt(blob, sizeof(blob)));
}

/* ---- one boot ---- */

typedef enum {
    ENROLL_NONE = 0,        /* just boot */
    ENROLL_WITH_ENDPOINT,   /* this boot enrolls, delivering EP_DELIVERED */
    ENROLL_WITHOUT_ENDPOINT /* this boot enrolls the version 1 way */
} enroll_kind_t;

typedef struct {
    cfg_kind_t cfg;
    enroll_kind_t enroll;
    /* The Dataset OpenThread holds at boot: 0 none, 1 NET_OTBR, 2 NET_OTHER. */
    int stored_dataset;
    /* Fail the record commit of this boot's enrollment. */
    bool commit_fails;
} boot_plan_t;

/* What a boot did, as the child's exit code. */
enum {
    BOOT_WAITING_NO_EXPORTER = 20, /* not enrolled: Thread off, exporter never started */
    BOOT_EXPORTER_OFF = 21,        /* enrolled, services started, exporter disabled */
    BOOT_SENT_TO_DELIVERED = 22,   /* every send went to EP_DELIVERED */
    BOOT_SENT_TO_CFG = 23,         /* every send went to the prepared image's address */
    BOOT_SENT_ELSEWHERE = 24,      /* a send went somewhere else, or to both: always a failure */
    BOOT_EXPORTER_SILENT = 25,     /* exporter started but sent nothing in time */
    BOOT_SENT_WHILE_WAITING = 26,  /* something was sent although not enrolled: always a failure */
    BOOT_REQUEST_REFUSED = 27,     /* the enrollment request was refused by the endpoint policy; nothing started,
                                      no Joiner, no exporter, nothing sent */
    BOOT_SETUP_ERROR = 90,
};

static char g_flash[64];

static void sleep_ms(int ms)
{
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

static int classify_sends(void)
{
    size_t total = host_pb_bridge_full_sim_captured_count();
    int to_delivered = 0, to_cfg = 0, elsewhere = 0, heartbeats = 0;
    for (size_t i = 0; i < total; i++) {
        host_pb_bridge_full_sim_sent_t cap;
        if (!host_pb_bridge_full_sim_get_captured(i, &cap)) {
            continue;
        }
        if (strcmp(cap.uri_path, URI_HEARTBEAT) == 0) {
            heartbeats++;
        }
        if (memcmp(cap.peer_addr, EP_DELIVERED.addr, 16) == 0 && cap.peer_port == EP_DELIVERED.port) {
            to_delivered++;
        } else if (memcmp(cap.peer_addr, CFG_STATIC_ADDR, 16) == 0 && cap.peer_port == CFG_STATIC_PORT) {
            to_cfg++;
        } else {
            elsewhere++;
        }
    }
    if (total == 0 || heartbeats == 0) {
        return BOOT_EXPORTER_SILENT;
    }
    if (elsewhere > 0 || (to_delivered > 0 && to_cfg > 0)) {
        return BOOT_SENT_ELSEWHERE;
    }
    return to_delivered > 0 ? BOOT_SENT_TO_DELIVERED : BOOT_SENT_TO_CFG;
}

/* Runs in the child; never returns. */
static void boot(const boot_plan_t *plan)
{
    if (!nvs_sim_set_backing_file(g_flash)) {
        _exit(BOOT_SETUP_ERROR);
    }
    put_cfg(plan->cfg);
    memset(&g_ot, 0, sizeof(g_ot));
    if (plan->stored_dataset != 0) {
        g_ot.has_key = true;
        g_ot.complete = true;
        g_ot.id = plan->stored_dataset == 1 ? NET_OTBR : NET_OTHER;
    }
    host_pb_bridge_full_sim_reset();
    host_pb_bridge_full_sim_set_default_response(OT_COAP_CODE_CHANGED);

    /* app_main(): the prepared image's bridge config; a load error only
     * disables the exporter. */
    power_base_bridge_config_t bridge_cfg;
    if (power_base_bridge_config_load(&bridge_cfg) != ESP_OK) {
        bridge_cfg.enabled = false;
    }

    /* app_main(), mode 2 branch: the image's enrollment endpoint policy. */
    pb_enroll_endpoint_policy_t policy;
    switch (power_base_bridge_config_load_enroll_policy()) {
    case POWER_BASE_BRIDGE_ENROLL_POLICY_NONE: policy = PB_ENROLL_ENDPOINT_POLICY_OPTIONAL; break;
    case POWER_BASE_BRIDGE_ENROLL_POLICY_ENDPOINT_REQUIRED: policy = PB_ENROLL_ENDPOINT_POLICY_REQUIRED; break;
    case POWER_BASE_BRIDGE_ENROLL_POLICY_INVALID:
    default: policy = PB_ENROLL_ENDPOINT_POLICY_INVALID; break;
    }

    /* enrollment_runtime.c: policy, boot rule, then (optionally) one enrollment. */
    pb_enroll_ctl_t ctl;
    pb_enroll_ctl_init(&ctl, &k_ops, EUI_A);
    pb_enroll_ctl_set_endpoint_policy(&ctl, policy);
    pb_enroll_ctl_boot(&ctl);
    if (plan->enroll != ENROLL_NONE) {
        char pskd[PB_ENROLL_PSKD_BUF_LEN] = {0};
        strncpy(pskd, PSKD, sizeof(pskd) - 1);
        esp_err_t err = plan->enroll == ENROLL_WITH_ENDPOINT
                            ? pb_enroll_ctl_request_join_with_endpoint(&ctl, pskd, &EP_DELIVERED, 0)
                            : pb_enroll_ctl_request_join(&ctl, pskd, 0);
        if (err == ESP_ERR_NOT_ALLOWED) {
            /* Refused by the endpoint policy. Nothing may have been started,
             * and nothing may be sent. */
            sleep_ms(400);
            _exit(g_joiner_starts == 0 && !pb_enroll_ctl_services_allowed(&ctl) &&
                          host_pb_bridge_full_sim_send_count() == 0
                      ? BOOT_REQUEST_REFUSED
                      : BOOT_SENT_WHILE_WAITING);
        }
        if (err != ESP_OK) {
            _exit(BOOT_SETUP_ERROR + 1);
        }
        g_ot.has_key = true; /* the Joiner's partial Dataset */
        g_ot.complete = false;
        pb_enroll_ctl_on_joiner_done(&ctl, g_ot.last_attempt, PB_ENROLL_JOINER_OK, 10);
        g_ot.complete = true; /* attached: the network delivered the full Dataset */
        g_ot.id = NET_OTBR;
        if (plan->commit_fails) {
            nvs_sim_force_next_commit_error(ESP_FAIL);
        }
        pb_enroll_ctl_tick(&ctl, true, 20);
    }

    if (!pb_enroll_ctl_services_allowed(&ctl)) {
        /* Safe wait: on_enrollment_ready() never runs, so no service -- and
         * no exporter -- is started. Give a wrongly started task time to
         * show itself anyway. */
        sleep_ms(400);
        _exit(host_pb_bridge_full_sim_send_count() == 0 ? BOOT_WAITING_NO_EXPORTER : BOOT_SENT_WHILE_WAITING);
    }

    /* on_enrollment_ready(): before any service starts. */
    if (ctl.has_bridge_endpoint) {
        (void)power_base_bridge_config_apply_delivered(&bridge_cfg, ctl.bridge_endpoint.addr, ctl.bridge_endpoint.port);
    }

    /* start_services_once(): exactly once per boot. */
    if (power_base_transport_init(EUI_A) != ESP_OK) {
        _exit(BOOT_SETUP_ERROR + 2);
    }
    if (!bridge_cfg.enabled) {
        sleep_ms(400);
        _exit(host_pb_bridge_full_sim_send_count() == 0 ? BOOT_EXPORTER_OFF : BOOT_SENT_ELSEWHERE);
    }
    if (linux_bridge_exporter_init(EUI_A, &bridge_cfg) != ESP_OK) {
        _exit(BOOT_SETUP_ERROR + 3);
    }
    /* Changing the caller's struct after init must not move the exporter:
     * it took its copy at start. */
    memset(&bridge_cfg, 0, sizeof(bridge_cfg));

    /* The heartbeat interval of this build's stub sdkconfig is 250 ms. Wait
     * for two heartbeats so a send to a second destination would show. */
    for (int waited = 0; waited < 4000; waited += 10) {
        int heartbeats = 0;
        size_t total = host_pb_bridge_full_sim_captured_count();
        for (size_t i = 0; i < total; i++) {
            host_pb_bridge_full_sim_sent_t cap;
            if (host_pb_bridge_full_sim_get_captured(i, &cap) && strcmp(cap.uri_path, URI_HEARTBEAT) == 0) {
                heartbeats++;
            }
        }
        if (heartbeats >= 2) {
            break;
        }
        sleep_ms(10);
    }
    _exit(classify_sends());
}

static int run_boot(boot_plan_t plan)
{
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        boot(&plan);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 99;
}

/* Rewrites the stored record in the simulated flash (a child, so the parent
 * never holds a store of its own). `mutate` gets the 56 raw bytes. */
static uint32_t crc32_ieee(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
    }
    return ~crc;
}

typedef enum {
    MUT_FLIP_ADDRESS_BIT,     /* one bit of the address, CRC left as it was */
    MUT_FLIP_PORT_BIT,
    MUT_TRUNCATE,             /* the write stopped after 40 bytes */
    MUT_FOREIGN_EUI,          /* another board's record, CRC correct */
    MUT_OTHER_PREFIX_ADDRESS, /* address moved to another prefix, CRC correct */
    MUT_PORT_ZERO,            /* CRC correct */
} mutation_t;

static bool mutate_record(mutation_t m)
{
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        if (!nvs_sim_set_backing_file(g_flash)) {
            _exit(1);
        }
        nvs_handle_t h;
        uint8_t raw[PB_ENROLL_MARKER_V2_LEN];
        size_t len = sizeof(raw);
        if (nvs_open(PB_ENROLL_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK ||
            nvs_get_blob(h, PB_ENROLL_NVS_KEY, raw, &len) != ESP_OK || len != PB_ENROLL_MARKER_V2_LEN) {
            _exit(2);
        }
        bool reseal = false;
        switch (m) {
        case MUT_FLIP_ADDRESS_BIT: raw[40] ^= 0x04; break;
        case MUT_FLIP_PORT_BIT: raw[49] ^= 0x01; break;
        case MUT_TRUNCATE: len = 40; break;
        case MUT_FOREIGN_EUI:
            memcpy(&raw[8], EUI_B, 8);
            reseal = true;
            break;
        case MUT_OTHER_PREFIX_ADDRESS:
            memcpy(&raw[32], NET_OTHER.mesh_local_prefix, 8);
            reseal = true;
            break;
        case MUT_PORT_ZERO:
            raw[48] = 0;
            raw[49] = 0;
            reseal = true;
            break;
        }
        if (reseal) {
            uint32_t crc = crc32_ieee(raw, 52);
            raw[52] = (uint8_t)crc;
            raw[53] = (uint8_t)(crc >> 8);
            raw[54] = (uint8_t)(crc >> 16);
            raw[55] = (uint8_t)(crc >> 24);
        }
        if (nvs_set_blob(h, PB_ENROLL_NVS_KEY, raw, len) != ESP_OK || nvs_commit(h) != ESP_OK) {
            _exit(3);
        }
        nvs_close(h);
        _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static void flash_reset(void)
{
    unlink(g_flash);
}

/* A board enrolled with the delivered endpoint, on an address-less image. */
static bool enroll_fresh(cfg_kind_t cfg)
{
    flash_reset();
    return run_boot((boot_plan_t){.cfg = cfg, .enroll = ENROLL_WITH_ENDPOINT}) == BOOT_SENT_TO_DELIVERED;
}

int main(void)
{
    snprintf(g_flash, sizeof(g_flash), "/tmp/pb_enroll_exporter_flash_%d", (int)getpid());

    printf("\n-- address-less prepared image: the exporter runs on the delivered endpoint --\n");
    flash_reset();
    CHECK(run_boot((boot_plan_t){.cfg = CFG_DISABLED}) == BOOT_WAITING_NO_EXPORTER,
          "not enrolled: safe wait, no exporter, nothing sent");
    CHECK(run_boot((boot_plan_t){.cfg = CFG_DISABLED, .enroll = ENROLL_WITH_ENDPOINT}) == BOOT_SENT_TO_DELIVERED,
          "boot 1: enrolls with the endpoint; the exporter's heartbeats go to that address and port");
    CHECK(run_boot((boot_plan_t){.cfg = CFG_DISABLED, .stored_dataset = 1}) == BOOT_SENT_TO_DELIVERED,
          "boot 2 (power cycle): the endpoint is restored from the record before the exporter starts");
    CHECK(run_boot((boot_plan_t){.cfg = CFG_DISABLED, .stored_dataset = 1}) == BOOT_SENT_TO_DELIVERED,
          "boot 3: again");
    CHECK(run_boot((boot_plan_t){.cfg = CFG_NO_BRIDGE, .stored_dataset = 1}) == BOOT_SENT_TO_DELIVERED,
          "an image without any pb_bridge namespace: same -- cfg is not needed for a delivered endpoint");

    printf("\n-- a prepared image that also carries a static address: the delivered endpoint wins --\n");
    CHECK(enroll_fresh(CFG_STATIC), "enrollment: sends go to the delivered endpoint, none to the image's address");
    CHECK(run_boot((boot_plan_t){.cfg = CFG_STATIC, .stored_dataset = 1}) == BOOT_SENT_TO_DELIVERED,
          "after a reboot: still only the delivered endpoint");

    printf("\n-- a damaged, foreign or mismatching record never starts the exporter -- not on the image's address either --\n");
    static const struct {
        mutation_t m;
        const char *what;
    } mutations[] = {
        {MUT_FLIP_ADDRESS_BIT, "one flipped bit in the stored address"},
        {MUT_FLIP_PORT_BIT, "one flipped bit in the stored port"},
        {MUT_TRUNCATE, "a record write cut short"},
        {MUT_FOREIGN_EUI, "another board's record (CRC intact)"},
        {MUT_OTHER_PREFIX_ADDRESS, "an address outside the record's network (CRC intact)"},
        {MUT_PORT_ZERO, "port 0 (CRC intact)"},
    };
    for (size_t i = 0; i < sizeof(mutations) / sizeof(mutations[0]); i++) {
        printf("      (%s)\n", mutations[i].what);
        CHECK(enroll_fresh(CFG_STATIC) && mutate_record(mutations[i].m), "(setup) enrolled, then the record is altered");
        CHECK(run_boot((boot_plan_t){.cfg = CFG_STATIC, .stored_dataset = 1}) == BOOT_WAITING_NO_EXPORTER,
              "next boot: safe wait -- no exporter, nothing sent to the image's static address");
    }
    CHECK(enroll_fresh(CFG_STATIC), "(setup) enrolled");
    CHECK(run_boot((boot_plan_t){.cfg = CFG_STATIC, .stored_dataset = 2}) == BOOT_WAITING_NO_EXPORTER,
          "the stored Dataset is another network's: safe wait, no exporter on either address");
    CHECK(run_boot((boot_plan_t){.cfg = CFG_STATIC, .stored_dataset = 0}) == BOOT_WAITING_NO_EXPORTER,
          "the stored Dataset is gone: safe wait, no exporter");

    printf("\n-- storing the record fails: not enrolled, no exporter --\n");
    flash_reset();
    CHECK(run_boot((boot_plan_t){.cfg = CFG_STATIC, .enroll = ENROLL_WITH_ENDPOINT, .commit_fails = true}) ==
              BOOT_WAITING_NO_EXPORTER,
          "the commit failed: services are not started, nothing is sent anywhere");
    CHECK(run_boot((boot_plan_t){.cfg = CFG_STATIC, .stored_dataset = 1}) == BOOT_WAITING_NO_EXPORTER,
          "next boot: still not enrolled");

    printf("\n-- enrollments that delivered no endpoint keep the old behaviour --\n");
    flash_reset();
    CHECK(run_boot((boot_plan_t){.cfg = CFG_STATIC, .enroll = ENROLL_WITHOUT_ENDPOINT}) == BOOT_SENT_TO_CFG,
          "version 1 enrollment on an image with a static address: the exporter uses that address");
    CHECK(run_boot((boot_plan_t){.cfg = CFG_STATIC, .stored_dataset = 1}) == BOOT_SENT_TO_CFG,
          "after a reboot (version 1 record): the same");
    flash_reset();
    CHECK(run_boot((boot_plan_t){.cfg = CFG_DISABLED, .enroll = ENROLL_WITHOUT_ENDPOINT}) == BOOT_EXPORTER_OFF,
          "version 1 enrollment on an address-less image: enrolled, services start, the exporter stays off");
    CHECK(run_boot((boot_plan_t){.cfg = CFG_DISABLED, .stored_dataset = 1}) == BOOT_EXPORTER_OFF,
          "after a reboot: still off (no address is invented)");

    printf("\n-- an image that REQUIRES the endpoint (--bridge-from-enrollment): never enrolled or resumed without one --\n");
    flash_reset();
    CHECK(run_boot((boot_plan_t){.cfg = CFG_ENDPOINT_REQUIRED}) == BOOT_WAITING_NO_EXPORTER,
          "not enrolled: safe wait");
    CHECK(run_boot((boot_plan_t){.cfg = CFG_ENDPOINT_REQUIRED, .enroll = ENROLL_WITHOUT_ENDPOINT}) == BOOT_REQUEST_REFUSED,
          "an enrollment request WITHOUT an endpoint: refused -- no Joiner, no services, nothing sent");
    CHECK(run_boot((boot_plan_t){.cfg = CFG_ENDPOINT_REQUIRED}) == BOOT_WAITING_NO_EXPORTER,
          "next boot: still not enrolled (the refused request left nothing behind)");
    CHECK(run_boot((boot_plan_t){.cfg = CFG_ENDPOINT_REQUIRED, .enroll = ENROLL_WITH_ENDPOINT}) == BOOT_SENT_TO_DELIVERED,
          "an enrollment request WITH the endpoint: enrolls, heartbeats go to the delivered address and port");
    CHECK(run_boot((boot_plan_t){.cfg = CFG_ENDPOINT_REQUIRED, .stored_dataset = 1}) == BOOT_SENT_TO_DELIVERED,
          "after a reboot: the version 2 record is resumed and the exporter runs on the restored endpoint");

    /* A board enrolled without an endpoint whose image is then replaced by
     * one that requires it. */
    flash_reset();
    CHECK(run_boot((boot_plan_t){.cfg = CFG_DISABLED, .enroll = ENROLL_WITHOUT_ENDPOINT}) == BOOT_EXPORTER_OFF,
          "(setup) enrolled the old way on an image without the policy: version 1 record, services on, exporter off");
    CHECK(run_boot((boot_plan_t){.cfg = CFG_ENDPOINT_REQUIRED, .stored_dataset = 1}) == BOOT_WAITING_NO_EXPORTER,
          "the same record under the policy: safe wait -- the services are NOT started without an exporter");
    CHECK(run_boot((boot_plan_t){.cfg = CFG_DISABLED, .stored_dataset = 1}) == BOOT_EXPORTER_OFF,
          "booting under the policy erased nothing: on the image without the policy that record resumes as before");
    CHECK(run_boot((boot_plan_t){.cfg = CFG_ENDPOINT_REQUIRED, .stored_dataset = 1, .enroll = ENROLL_WITHOUT_ENDPOINT}) ==
              BOOT_REQUEST_REFUSED,
          "under the policy a request without an endpoint is refused in that state too");
    CHECK(run_boot((boot_plan_t){.cfg = CFG_DISABLED, .stored_dataset = 1}) == BOOT_EXPORTER_OFF,
          "and that refusal erased neither the record nor the Dataset");
    CHECK(run_boot((boot_plan_t){.cfg = CFG_ENDPOINT_REQUIRED, .stored_dataset = 1, .enroll = ENROLL_WITH_ENDPOINT}) ==
              BOOT_SENT_TO_DELIVERED,
          "a request with the endpoint replaces the old record and the exporter runs");
    CHECK(run_boot((boot_plan_t){.cfg = CFG_ENDPOINT_REQUIRED, .stored_dataset = 1}) == BOOT_SENT_TO_DELIVERED,
          "and keeps running on that endpoint after a reboot");
    flash_reset();
    CHECK(run_boot((boot_plan_t){.cfg = CFG_STATIC, .enroll = ENROLL_WITHOUT_ENDPOINT}) == BOOT_SENT_TO_CFG,
          "(setup) enrolled the old way on an image with a static address");
    CHECK(run_boot((boot_plan_t){.cfg = CFG_ENDPOINT_REQUIRED, .stored_dataset = 1}) == BOOT_WAITING_NO_EXPORTER,
          "that record under an endpoint-required image: safe wait, nothing sent");

    printf("\n-- an image whose policy byte this firmware does not know: no new enrollment --\n");
    flash_reset();
    CHECK(run_boot((boot_plan_t){.cfg = CFG_POLICY_UNKNOWN_VALUE, .enroll = ENROLL_WITHOUT_ENDPOINT}) == BOOT_REQUEST_REFUSED,
          "a request without an endpoint: refused");
    CHECK(run_boot((boot_plan_t){.cfg = CFG_POLICY_UNKNOWN_VALUE, .enroll = ENROLL_WITH_ENDPOINT}) == BOOT_REQUEST_REFUSED,
          "a request with an endpoint: refused too");
    CHECK(run_boot((boot_plan_t){.cfg = CFG_POLICY_UNKNOWN_VALUE}) == BOOT_WAITING_NO_EXPORTER, "nothing was enrolled");
    CHECK(enroll_fresh(CFG_ENDPOINT_REQUIRED), "(setup) a board enrolled with an endpoint");
    CHECK(run_boot((boot_plan_t){.cfg = CFG_POLICY_UNKNOWN_VALUE, .stored_dataset = 1}) == BOOT_SENT_TO_DELIVERED,
          "an already enrolled board with a version 2 record still resumes on its endpoint");

    printf("\n-- images without the policy are untouched by it --\n");
    flash_reset();
    CHECK(run_boot((boot_plan_t){.cfg = CFG_NO_BRIDGE, .enroll = ENROLL_WITHOUT_ENDPOINT}) == BOOT_EXPORTER_OFF,
          "no pb_bridge namespace at all: a request without an endpoint enrolls as before (exporter off)");
    CHECK(run_boot((boot_plan_t){.cfg = CFG_NO_BRIDGE, .stored_dataset = 1}) == BOOT_EXPORTER_OFF, "and resumes as before");

    flash_reset();
    printf("\n%s: %d failure(s)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
