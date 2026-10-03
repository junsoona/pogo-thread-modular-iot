/*
 * Power base entry point (see docs/power-base.md). This is a SEPARATE
 * ESP-IDF project/binary from `module` -- the power base is never a sensor
 * module role and never runs role_manager/child_service/sensor_provider/
 * sensor_service at all (this file deliberately never touches any of those
 * headers). It reuses parent_service and pogo_port UNMODIFIED, wiring
 * parent_service's small context struct (see parent_service.h) to its own
 * always-power-base identity instead of role_manager/child_service.
 *
 * FIXED application role vs. DYNAMIC Thread role (this follow-up): the
 * power base's fixed, permanent application role is the physical root of
 * the POGO chain, the downstream provisioning coordinator, the UUID/slot/
 * topology registry owner, the sensor-data aggregator, and (in a future
 * revision) the POGO 5V supplier and the gateway in front of a Linux Matter
 * Bridge -- see docs/power-base.md. It is NOT described here as Thread's
 * "ROOT" or "Leader": `otDeviceRole` is entirely Thread's own decision, and
 * this file never calls thread_manager_start_as_root() (that name/wrapper
 * exists only for module's DEV_ROOT regression fixture -- see
 * thread_manager.h) -- it calls the neutral
 * thread_manager_start_as_always_on_ftd() instead, and logs this device's
 * own boot/attach progress as "power-base FTD:", never "ROOT:". This device
 * always runs as an always-on FTD (rx-on-when-idle=true, router-eligible=true
 * -- same link mode as before) and accepts CHILD, ROUTER, or LEADER alike as
 * "attached and ready"; only DISABLED/DETACHED are "not ready". Nothing
 * below waits specifically for LEADER, and a first attach that takes longer
 * than any fixed budget (e.g. an OTBR/another FTD only becoming reachable
 * much later) is NOT a fatal error -- see "attach supervisor" below.
 *
 * STANDALONE vs. EXTERNAL_OTBR (this follow-up, see docs/otbr-rcp.md and
 * docs/power-base.md): power_base_thread_config_load() (components/
 * power_base_thread_config, a small config component separate from
 * power_base_config -- see its own header) decides which of the two this
 * boot uses. STANDALONE is the pre-existing behavior, completely unchanged:
 * thread_manager_start_as_always_on_ftd() forms/resumes this device's OWN
 * network. EXTERNAL_OTBR instead joins a Linux OTBR + separate ESP32-H2 RCP
 * project's (rcp/, see docs/otbr-rcp.md) EXISTING Thread network, using its
 * real Active Dataset TLVs (read from the "cfg" partition's "pb_thread"
 * namespace, provisioned offline by tools/gen_power_base_otbr_config.sh --
 * NEVER generated/invented by this firmware) via
 * thread_manager_start_as_always_on_ftd_with_dataset(). Both paths apply the
 * IDENTICAL always-on-FTD link mode and both feed the SAME
 * attach_supervisor_task()/start_services_once() below -- CHILD/ROUTER/
 * LEADER are all "attached and ready" either way, and every downstream
 * service (parent_service, power_base_transport, topology registry) is
 * completely unaware of which mode got it there. EXTERNAL_OTBR NEVER falls
 * back to forming a standalone network on any failure (a bad/missing
 * dataset, a real OpenThread rejection, ...) -- see halt_forever()'s
 * callers below for both the config-load and the Thread-join failure paths.
 *
 * AWAIT_COMMISSIONING (pb_thread:mode=2, components/power_base_enrollment,
 * enrollment_runtime.c): steps 1-5 below, then Thread is disabled and step 3
 * (power rail) is skipped. Thread is enabled only on a Dataset OpenThread
 * already stored -- never formed -- and only when (a) the enrollment marker
 * is valid for this EUI-64 and matches that complete Dataset (boot resume),
 * or (b) a Joiner run succeeded (bench builds: "pb_join" console). In case
 * (b) the board counts as enrolled only once attached with a complete
 * Dataset and the marker is committed; on failure it stops Thread and waits
 * again. Either way on_enrollment_ready() then runs step 3 and step 7 once,
 * so steps 8-13 (including the exporter and DETECT_OUT) never run before.
 *
 * Boot sequence:
 *   1. nvs_flash_init()            -- dynamic "nvs" partition (OpenThread's
 *                                      own Active Dataset, see
 *                                      thread_manager_start_as_always_on_ftd()
 *                                      -- STANDALONE mode only; EXTERNAL_OTBR
 *                                      always overwrites whatever commissioned
 *                                      dataset this partition holds with the
 *                                      "cfg" partition's own, see section 7 of
 *                                      docs/power-base.md).
 *   2. power_base_config_load()    -- fixed "cfg" partition. CONFIG_ERROR
 *                                      halts here, same policy as module_config_load().
 *   2b. power_base_thread_config_load() -- SAME "cfg" partition, separate
 *                                      "pb_thread" namespace (see above).
 *                                      Decides STANDALONE vs. EXTERNAL_OTBR.
 *                                      A legacy image with no "pb_thread"
 *                                      namespace/key loads as STANDALONE,
 *                                      not an error. An EXTERNAL_OTBR image
 *                                      with a missing/empty/oversized
 *                                      active_tlvs halts here (fail closed).
 *   3. Power rail sequencing (power_enable_gpio/...) -- see section 8 of
 *                                      docs/power-base.md. A no-op today (no
 *                                      switching hardware decided yet).
 *   4. esp_efuse_mac_get_default() -- this power base's own stable 8-byte
 *                                      identity (its EUI-64), used both as
 *                                      its POGO UUID (for ASSIGN_PARENT_INFO
 *                                      and SET_COLLECTOR) and its topology-
 *                                      registry key. Never regenerated,
 *                                      never logged in full.
 *   5. thread_manager_init()       -- OpenThread FTD stack up, no network yet.
 *   6. thread_manager_start_as_always_on_ftd() (STANDALONE) or
 *      thread_manager_start_as_always_on_ftd_with_dataset() (EXTERNAL_OTBR)
 *                                   -- STANDALONE: the only place in the whole
 *                                      system that ever forms or resumes an
 *                                      Active Dataset. If NVS already holds a
 *                                      commissioned dataset, it is resumed;
 *                                      otherwise a new one is formed from
 *                                      Kconfig defaults. Neither the dataset
 *                                      bytes nor the Network Key are ever
 *                                      logged. A failure HERE (a genuine
 *                                      OpenThread API error, not "not yet
 *                                      attached") halts immediately -- see
 *                                      halt_forever()'s callers below.
 *   7. attach_supervisor_task() started, app_main() returns. This task is
 *                                      the ONLY thing that ever calls
 *                                      start_services_once() (step 8-12
 *                                      below) -- it polls thread_manager_get_role()
 *                                      (bounded, non-busy: 200ms between
 *                                      checks) until this device's FIRST
 *                                      attach (CHILD, ROUTER, or LEADER),
 *                                      with NO timeout and NO halt_forever()
 *                                      for simply "not attached yet" -- a
 *                                      Thread/OTBR network that only becomes
 *                                      reachable much later must still work.
 *                                      A rate-limited ESP_LOGW appears once
 *                                      30s have passed without attaching, and
 *                                      repeats at most once every 60s after
 *                                      that (never a flood, never a busy
 *                                      loop) -- see ATTACH_WARN_*_MS below.
 *                                      Once attached, start_services_once()
 *                                      runs EXACTLY ONCE and this task then
 *                                      deletes itself: no further role
 *                                      fluctuation (including a LATER drop to
 *                                      DISABLED/DETACHED and a subsequent
 *                                      reattach) ever re-enters this task's
 *                                      logic or re-creates anything -- see
 *                                      "role fluctuation" below.
 *   8. power_base_transport_init() -- registers the CoAP "/s" (sensor data)
 *                                      and "/t" (topology report) resources.
 *   9. parent_service_init()       -- wired to the power base's own context:
 *                                      pb_provisioning_allowed() re-checks
 *                                      "is otDeviceRole currently CHILD,
 *                                      ROUTER, or LEADER" on every call
 *                                      (parent_service's own loop already
 *                                      polls this continuously -- no
 *                                      separate watcher task is needed
 *                                      here), get_own_depth()==0,
 *                                      pb_get_collector() supplies this
 *                                      power base's own Mesh-Local EID +
 *                                      CoAP port whenever attached.
 *  10. pogo_port_init()            -- registers the downstream port's
 *                                      config; touches no GPIO yet.
 *  11. Stale-sweep task creation CONFIRMED (xTaskCreate() checked) -- must
 *                                      succeed BEFORE pogo_port_enable()
 *                                      below, or a sensor module could start
 *                                      docking while this power base can
 *                                      never mark a stale node offline
 *                                      again. Failure here halts -- a
 *                                      genuine resource-creation failure,
 *                                      not "not attached yet".
 *  12. pogo_port_enable()          -- the ONE downstream POGO port. This is
 *                                      also what drives DETECT_OUT active
 *                                      (Low) -- see pogo_port.c's
 *                                      pogo_port_enable(): there is no
 *                                      separate step to sequence.
 *                                      DISABLED/DETACHED never reaches this
 *                                      line at all (it only runs inside
 *                                      start_services_once(), only called
 *                                      after a confirmed attach), so
 *                                      DETECT_OUT never activates before
 *                                      this device is attached AND the
 *                                      stale-sweep task is confirmed running.
 *  13. "power base ready" logged   -- only reachable once every step above
 *                                      inside start_services_once() succeeded.
 *
 * Role fluctuation AFTER services start: none of the CoAP resources, the
 * parent_service task, the pogo_port task/GPIOs, or the stale-sweep task are
 * ever torn down or re-created because `otDeviceRole` later changes among
 * CHILD/ROUTER/LEADER, or even drops to DETACHED/DISABLED. Instead:
 *   - pb_provisioning_allowed() (re-checked by parent_service on every idle
 *     loop iteration, never cached) simply starts returning false the moment
 *     role becomes DISABLED/DETACHED -- no NEW downstream provisioning
 *     session is started, but an already-connected/JOINED child is left
 *     alone (parent_service does not tear down an existing session just
 *     because a NEW one could not start).
 *   - pb_get_collector() likewise starts returning false while
 *     DISABLED/DETACHED -- a session that reaches its own SET_COLLECTOR step
 *     during that window is abandoned by parent_service (same as any other
 *     failed provisioning step) and naturally retried once this device
 *     reattaches, without any code here needing to notice the transition
 *     itself. A peer that is already PEER_READY and waiting
 *     (`s_peer_ready_pending` in parent_service.c) is automatically
 *     re-evaluated on parent_service's own next idle-loop iteration once
 *     pb_provisioning_allowed() starts returning true again -- nothing here
 *     needs to "wake" it up explicitly.
 *   - Once this device reattaches (any of CHILD/ROUTER/LEADER again), both
 *     callbacks resume returning true/valid on their very next call -- no
 *     explicit "resume" step, no busy-retry loop; it is simply the same
 *     live otDeviceRole check as always.
 */
#include <stdint.h>
#include <string.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "openthread/coap.h" /* OT_DEFAULT_COAP_PORT */

#include "pogo_proto/pogo_crc16.h"
#include "pogo_port/pogo_port.h"
#include "parent_service/parent_service.h"
#include "power_base_config/power_base_config.h"
#include "power_base_thread_config/power_base_thread_config.h"
#include "power_base_bridge_config/power_base_bridge_config.h"
#include "power_base_transport/power_base_transport.h"
#include "linux_bridge_exporter/linux_bridge_exporter.h"
#include "thread_manager/thread_manager.h"
#include "enrollment_runtime.h"

static const char *TAG = "power_base_main";

/* First rate-limited "still not attached" warning, and the minimum spacing
 * between every warning after that -- see attach_supervisor_task(). Neither
 * of these is a halt condition; they exist purely so a real console shows
 * SOMETHING if this device is stuck DISABLED/DETACHED for a long time,
 * without flooding the log while it waits (e.g. for an OTBR to come up). */
#define ATTACH_WARN_FIRST_MS 30000
#define ATTACH_WARN_REPEAT_MS 60000
#define ATTACH_POLL_INTERVAL_MS 200

static power_base_config_t s_cfg;
static power_base_thread_config_t s_thread_cfg;
/*
 * This follow-up (Power Base -> Linux OTBR northbound bridge transport):
 * enabled=false by default, and stays that way for the rest of this boot on
 * ANY power_base_bridge_config_load() failure -- unlike s_cfg/s_thread_cfg
 * above, a pb_bridge problem is NEVER a reason to halt the whole power base
 * (see components/power_base_bridge_config's own header comment's "boot
 * policy" section: this optional mirror's own config errors must not newly
 * brick an otherwise-healthy power base).
 */
static power_base_bridge_config_t s_bridge_cfg;
static uint8_t s_own_uuid[POGO_UUID_LEN];
static parent_service_context_t s_parent_ctx;
/* Informational provisioning epoch handed out in every SET_COLLECTOR this
 * boot sends (pogo_collector_endpoint_t.generation) -- not read back or
 * relied on for any wire-level decision today; a random-per-boot value is
 * enough to let a future revision distinguish "this boot's collector" from
 * an earlier one if it ever needs to (see pogo_protocol.h's own comment). */
static uint32_t s_collector_generation;
/* Set exactly once, by start_services_once(), the first (and only) time it
 * runs -- guards against it ever being called twice (see
 * attach_supervisor_task()). */
static volatile bool s_services_started;

static void halt_forever(const char *why)
{
    ESP_LOGE(TAG, "%s -- halting (no sensor, no POGO pairing, no Thread network without a valid config/role)", why);
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

/*
 * Power rail sequencing (section 4): no power-switching GPIO has been
 * decided on current hardware. power_enable_gpio<0 means the 5V rail is
 * assumed ALWAYS ON -- this firmware never claims to be able to cut real 5V
 * power when that is the case (see docs/power-base.md's safety notes).
 */
static void apply_power_rail_sequencing(void)
{
    if (s_cfg.power_enable_gpio < 0) {
        ESP_LOGI(TAG, "power_enable_gpio not configured -- 5V rail assumed always-on (no switching hardware yet)");
        return;
    }
    gpio_config_t out_cfg = {
        .pin_bit_mask = 1ULL << s_cfg.power_enable_gpio,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&out_cfg);
    if (err != ESP_OK) {
        halt_forever("power_enable_gpio: gpio_config failed -- refusing to continue with an unconfigured power rail "
                     "control pin");
    }
    err = gpio_set_level((gpio_num_t)s_cfg.power_enable_gpio, s_cfg.power_enable_active_high ? 1 : 0);
    if (err != ESP_OK) {
        halt_forever("power_enable_gpio: gpio_set_level failed -- the power rail's actual state is now unknown");
    }
    ESP_LOGI(TAG, "power_enable_gpio=%d driven %s -- waiting power_startup_delay_ms=%u before continuing",
             (int)s_cfg.power_enable_gpio, s_cfg.power_enable_active_high ? "HIGH" : "LOW",
             (unsigned)s_cfg.power_startup_delay_ms);
    if (s_cfg.power_startup_delay_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(s_cfg.power_startup_delay_ms));
    }
}

static bool role_is_attached(otDeviceRole role)
{
    return role == OT_DEVICE_ROLE_CHILD || role == OT_DEVICE_ROLE_ROUTER || role == OT_DEVICE_ROLE_LEADER;
}

/* --- parent_service_context_t: this power base's own identity/readiness,
 * reusing parent_service completely unmodified (see parent_service.h). --- */

/*
 * Re-checked live on every call (parent_service's own idle loop already does
 * this every iteration -- see parent_service.c's parent_service_task()), so
 * this needs no caching and no separate watcher task: CHILD, ROUTER, and
 * LEADER are all normal, provisioning-ready Thread connectivity states for
 * this power base (its fixed application role never depended on Thread
 * Leadership in the first place -- see this file's top comment).
 * DISABLED/DETACHED are the only "not ready" states.
 */
static bool pb_provisioning_allowed(void *ctx)
{
    (void)ctx;
    return role_is_attached(thread_manager_get_role());
}

static uint8_t pb_get_own_depth(void *ctx)
{
    (void)ctx;
    return 0; /* the true network root -- its direct child is always assigned depth=1 */
}

static void pb_get_own_uuid(uint8_t out_uuid[POGO_UUID_LEN], void *ctx)
{
    (void)ctx;
    memcpy(out_uuid, s_own_uuid, POGO_UUID_LEN);
}

static const char *pb_role_name(void *ctx)
{
    (void)ctx;
    return "POWER_BASE";
}

/*
 * Fills *out with this power base's own identity as the collector every
 * sensor module in the chain should send "/s"/"/t" traffic to: its own
 * permanent UUID + its own Thread Mesh-Local EID (via
 * thread_manager_get_mesh_local_eid(), confirmed against ESP-IDF v6.1's real
 * otThreadGetMeshLocalEid() -- see thread_manager.h) + the CoAP port
 * power_base_transport_init() actually bound. Returns false (collector not
 * currently valid) while DISABLED/DETACHED -- see this file's top comment on
 * role fluctuation for what that means for an in-flight provisioning
 * session.
 */
static bool pb_get_collector(pogo_collector_endpoint_t *out, void *ctx)
{
    (void)ctx;
    if (!role_is_attached(thread_manager_get_role())) {
        return false;
    }
    otIp6Address mleid;
    if (thread_manager_get_mesh_local_eid(&mleid) != ESP_OK) {
        return false;
    }
    memcpy(out->power_base_uuid, s_own_uuid, POGO_UUID_LEN);
    memcpy(out->addr, mleid.mFields.m8, sizeof(out->addr));
    out->port = OT_DEFAULT_COAP_PORT;
    out->generation = s_collector_generation;
    return pogo_collector_endpoint_is_valid(out);
}

static void port_event_dispatcher(pogo_port_id_t port_id, pogo_port_event_t event, const pogo_port_event_data_t *data,
                                   void *user_ctx)
{
    (void)user_ctx;
    if (port_id != POGO_PORT_DOWNSTREAM) {
        return; /* the power base has no UPSTREAM port at all */
    }
    parent_service_on_port_event(port_id, event, data);
    if (event == POGO_PORT_EVENT_DETACHED) {
        /* Immediate hardware signal: the direct child (and its whole
         * subtree) is unreachable the instant this physical link drops --
         * see docs/power-base.md's detach handling. */
        power_base_transport_on_direct_downstream_detached();
    }
}

static void stale_sweep_task(void *arg)
{
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        power_base_transport_sweep_stale(now_ms, s_cfg.topology_stale_timeout_ms);
    }
}

/*
 * Runs the fixed, one-time service-startup steps (boot-sequence steps 8-13
 * in this file's top comment) -- called EXACTLY ONCE, by
 * attach_supervisor_task(), the first time this device is ever observed
 * attached (CHILD, ROUTER, or LEADER). Every failure inside here is a
 * genuine resource-creation failure (CoAP resource registration, task
 * creation, GPIO/UART bring-up) -- NOT "not attached yet" -- so halting is
 * still the correct policy, same as the rest of this file's boot-time
 * failures.
 *
 * Only reads/writes static (file-scope) state -- s_cfg/s_own_uuid/
 * s_collector_generation/s_parent_ctx are all already-initialized globals
 * by the time this can ever run, never a stack-local address handed to a
 * longer-lived task.
 */
static void start_services_once(otDeviceRole role)
{
    ESP_ERROR_CHECK(power_base_transport_init(s_own_uuid));

    /*
     * This follow-up: the optional Linux bridge exporter, started AFTER
     * power_base_transport_init() (it registers itself as that component's
     * bridge observer -- see power_base_transport.h's own top comment) and
     * here in start_services_once() specifically because it also needs
     * esp_openthread_get_instance() to be meaningful, same ordering
     * constraint as parent_service_init()/pogo_port_init() below. A failure
     * here (a genuine resource-creation failure -- see
     * linux_bridge_exporter.h's own doc comment) is logged and otherwise
     * ignored: this optional mirror never gates POGO provisioning, Thread
     * membership, or any other subsystem below.
     */
    if (s_bridge_cfg.enabled) {
        esp_err_t bridge_err = linux_bridge_exporter_init(s_own_uuid, &s_bridge_cfg);
        if (bridge_err != ESP_OK) {
            ESP_LOGW(TAG, "linux_bridge_exporter_init failed (%s) -- continuing without the optional Linux bridge "
                          "mirror this boot",
                     esp_err_to_name(bridge_err));
        }
    } else {
        ESP_LOGI(TAG, "pb_bridge exporter disabled -- no Linux OTBR mirror this boot");
    }

    s_parent_ctx = (parent_service_context_t){
        .provisioning_allowed = pb_provisioning_allowed,
        .get_own_depth = pb_get_own_depth,
        .get_own_uuid = pb_get_own_uuid,
        .role_name = pb_role_name,
        .is_power_base = true,
        .get_collector = pb_get_collector,
        .ack_timeout_ms = s_cfg.ack_timeout_ms,
        .ctx = NULL,
    };
    ESP_ERROR_CHECK(parent_service_init(&s_parent_ctx));

    pogo_port_config_t down_cfg = {
        .enabled = true,
        .uart_num = s_cfg.downstream_port.uart_num,
        .tx_gpio = s_cfg.downstream_port.tx_gpio,
        .rx_gpio = s_cfg.downstream_port.rx_gpio,
        .detect_in_gpio = s_cfg.downstream_port.detect_in_gpio,
        .detect_out_gpio = s_cfg.downstream_port.detect_out_gpio,
        .base_id_gpio = -1, /* BASE_ID is read on a MODULE's UPSTREAM side only -- meaningless here */
        .detect_out_active_low = s_cfg.downstream_port.detect_out_active_low,
        .debounce_ms = (s_cfg.downstream_port.debounce_ms != 0) ? s_cfg.downstream_port.debounce_ms : 30,
        .baud_rate = CONFIG_POGO_UART_BAUD_RATE,
    };
    ESP_ERROR_CHECK(pogo_port_init(POGO_PORT_DOWNSTREAM, &down_cfg, port_event_dispatcher, NULL));

    /*
     * The stale-sweep task must be CONFIRMED running BEFORE pogo_port_enable()
     * -- otherwise a sensor module could already be docking (DETECT_OUT
     * active, provisioning under way) while this power base's own ability to
     * ever mark a stale node offline again was silently broken. If this task
     * cannot be created, halt here -- exactly like every other resource
     * failure -- WITHOUT ever calling pogo_port_enable(): DETECT_OUT stays
     * released, "power base ready" is never logged, and no sensor module is
     * ever invited to dock against a power base that cannot maintain its own
     * topology registry.
     */
    BaseType_t sweep_task_ok = xTaskCreate(stale_sweep_task, "topology_stale_sweep", 3072, NULL, 3, NULL);
    if (sweep_task_ok != pdPASS) {
        halt_forever("STALE_SWEEP_TASK_FAILED: xTaskCreate failed -- refusing to enable downstream POGO "
                     "provisioning with no way to ever mark a stale node offline again");
    }

    /* pogo_port_enable() itself drives DETECT_OUT active (Low) as part of
     * bringing the port up -- simply never having called it before this
     * device was first attached AND the stale-sweep task was confirmed
     * running (just above) IS this system's "DETECT_OUT only once fully
     * ready" guarantee; there is no separate GPIO write to sequence. This
     * call happens exactly once per boot -- start_services_once() itself is
     * only ever invoked once, see attach_supervisor_task(). */
    ESP_ERROR_CHECK(pogo_port_enable(POGO_PORT_DOWNSTREAM, POGO_PEER_TYPE_POWER_BASE));
    ESP_LOGI(TAG, "pogo_port DOWNSTREAM: task created and enabled");

    s_services_started = true;
    ESP_LOGI(TAG, "power base ready: uuid=..%02x%02x otDeviceRole=%u(%s) stale_sweep=running -- awaiting the "
                  "first sensor module",
             s_own_uuid[6], s_own_uuid[7], (unsigned)role, thread_manager_role_to_str(role));
}

/*
 * Owns this device's FIRST attach wait, with NO fixed timeout and NO
 * halt_forever() for simply "not attached yet" -- a Thread/OTBR network
 * that only becomes reachable much later (or a bench setup with no OTBR at
 * all, forming its own standalone network, which may itself take a moment)
 * must still work correctly, not permanently wedge this power base. See
 * this file's top comment for the full policy.
 *
 * Polls thread_manager_get_role() every ATTACH_POLL_INTERVAL_MS (200ms) --
 * bounded, non-busy, same cadence the pre-existing boot-time wait already
 * used. thread_manager_get_role() itself is a cheap, non-blocking read (an
 * OpenThread API-lock-guarded field read), so this loop never risks
 * starving anything else.
 *
 * Once attached, calls start_services_once() EXACTLY ONCE and then deletes
 * itself -- there is nothing further for this task to do afterward:
 * pb_provisioning_allowed()/pb_get_collector() both read
 * thread_manager_get_role() live, on every call, directly from
 * parent_service's own code, so no supervisor involvement is needed for
 * any LATER role fluctuation (including a drop back to DISABLED/DETACHED
 * and a subsequent reattach) -- see "role fluctuation" in this file's top
 * comment.
 */
static void attach_supervisor_task(void *arg)
{
    (void)arg;
    TickType_t boot_tick = xTaskGetTickCount();
    bool warned_once = false;
    TickType_t last_warn_tick = 0;

    while (1) {
        otDeviceRole role = thread_manager_get_role();
        if (role_is_attached(role)) {
            ESP_LOGI(TAG, "power-base FTD: attached, otDeviceRole=%u(%s) -- starting services (this runs exactly "
                          "once; a later role change never re-triggers it)",
                     (unsigned)role, thread_manager_role_to_str(role));
            start_services_once(role);
            vTaskDelete(NULL);
            return;
        }

        TickType_t now = xTaskGetTickCount();
        TickType_t elapsed_ms = (now - boot_tick) * portTICK_PERIOD_MS;
        if (elapsed_ms >= ATTACH_WARN_FIRST_MS &&
            (!warned_once || (now - last_warn_tick) * portTICK_PERIOD_MS >= ATTACH_WARN_REPEAT_MS)) {
            ESP_LOGW(TAG, "power-base FTD: still not attached after %ums (otDeviceRole=%u(%s)) -- OpenThread attach "
                          "attempt continues; services will start automatically once attached (e.g. once an OTBR "
                          "or another Thread device becomes reachable) -- not halting",
                     (unsigned)elapsed_ms, (unsigned)role, thread_manager_role_to_str(role));
            warned_once = true;
            last_warn_tick = now;
        }
        vTaskDelay(pdMS_TO_TICKS(ATTACH_POLL_INTERVAL_MS));
    }
}

static void start_attach_supervisor(void)
{
    BaseType_t supervisor_ok = xTaskCreate(attach_supervisor_task, "attach_supervisor", 3584, NULL, 4, NULL);
    if (supervisor_ok != pdPASS) {
        halt_forever("ATTACH_SUPERVISOR_TASK_FAILED: xTaskCreate failed -- refusing to continue with no way to ever "
                     "notice this device attaching to Thread");
    }
}

/*
 * AWAIT_COMMISSIONING only: called exactly once by enrollment_runtime.c when
 * this board is enrolled -- either at boot (valid marker + matching stored
 * Dataset, Thread just enabled on it) or after a Joiner success, a
 * CHILD/ROUTER/LEADER attach with a complete Dataset, and a committed marker.
 * From here on the board behaves like mode 0/1: the supervisor (re)checks the
 * role and runs start_services_once() exactly once.
 */
static void on_enrollment_ready(void)
{
    ESP_LOGI(TAG, "AWAIT_COMMISSIONING: enrolled -- enabling the power rail and the normal attach supervisor");
    apply_power_rail_sequencing();
    start_attach_supervisor();
}

void app_main(void)
{
    /* Minimal, unconditional boot-diagnostic trail -- see module/main/
     * app_main.c's identical note: this does not by itself fix "no serial
     * output until reset"; if these lines never appear at all on a real
     * board, that points at USB re-enumeration / power / monitor-connection,
     * not firmware logic. */
    ESP_LOGI(TAG, "app_main: entry (reset_reason=%d)", (int)esp_reset_reason());

    if (!pogo_crc16_selftest()) {
        ESP_LOGE(TAG, "CRC16 self-test failed -- refusing to start");
        abort();
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    if (power_base_config_load(&s_cfg) != ESP_OK) {
        /* power_base_config_load() already logged a specific CONFIG_ERROR line. */
        halt_forever("CONFIG_ERROR loading power_base_config");
    }
    ESP_LOGI(TAG, "power_base_config_load: OK");

    /*
     * Thread dataset provenance (this follow-up): STANDALONE (this power
     * base forms/resumes its own network, exactly as before) or
     * EXTERNAL_OTBR (join a Linux OTBR + separate ESP32-H2 RCP's EXISTING
     * network using its real Active Dataset -- see docs/otbr-rcp.md,
     * docs/power-base.md). power_base_thread_config_load() already fails
     * closed and logs its own CONFIG_ERROR for any EXTERNAL_OTBR storage
     * problem (missing/empty/oversized active_tlvs, unsupported mode) --
     * halting here, same policy as power_base_config_load() above, is the
     * ONLY thing this call site does: it never falls back to STANDALONE on
     * its own. A legacy image with no "pb_thread" namespace/key at all
     * loads as STANDALONE without any error (see that function's own
     * comment) -- this is what keeps every pre-existing power_base_config.bin
     * working unchanged. */
    if (power_base_thread_config_load(&s_thread_cfg) != ESP_OK) {
        halt_forever("CONFIG_ERROR loading power_base_thread_config (EXTERNAL_OTBR mode -- no standalone fallback)");
    }
    if (s_thread_cfg.mode == POWER_BASE_THREAD_MODE_EXTERNAL_OTBR) {
        ESP_LOGI(TAG, "Thread dataset mode: EXTERNAL_OTBR (%u bytes; contents hidden)",
                 (unsigned)s_thread_cfg.active_tlvs_len);
    } else if (s_thread_cfg.mode == POWER_BASE_THREAD_MODE_AWAIT_COMMISSIONING) {
        ESP_LOGI(TAG, "Thread dataset mode: AWAIT_COMMISSIONING");
    } else {
        ESP_LOGI(TAG, "Thread dataset mode: STANDALONE");
    }

    /*
     * This follow-up (Power Base -> Linux OTBR northbound bridge transport):
     * UNLIKE the two loads above, a non-ESP_OK return here is NEVER a halt
     * condition -- see components/power_base_bridge_config's own header
     * comment's "boot policy" section. A missing "pb_bridge" namespace
     * (every pre-existing image) or an explicit enabled=false both already
     * return ESP_OK with enabled=false; only a genuinely malformed/
     * CRC-mismatched/invalid-when-enabled config reaches this branch, and it
     * simply disables the optional exporter for the rest of this boot
     * instead of halting the whole power base.
     */
    if (power_base_bridge_config_load(&s_bridge_cfg) != ESP_OK) {
        ESP_LOGW(TAG, "pb_bridge CONFIG_ERROR -- the optional Linux bridge exporter will not start this boot; "
                      "POGO provisioning, Thread join, and every other subsystem are unaffected");
        s_bridge_cfg.enabled = false; /* fail-safe: never start the exporter on a load error */
    }

    if (s_thread_cfg.mode != POWER_BASE_THREAD_MODE_AWAIT_COMMISSIONING) {
        apply_power_rail_sequencing();
    }

    /* Stable identity (section 6): the ESP32-H2's own efuse-provisioned
     * EUI-64 is used directly as this power base's 8-byte UUID -- permanent
     * hardware identity, never regenerated, never logged in full (only the
     * last-byte suffix, same convention as every other UUID log line in this
     * codebase). */
    uint8_t mac8[8];
    if (esp_efuse_mac_get_default(mac8) != ESP_OK) {
        halt_forever("esp_efuse_mac_get_default failed -- cannot derive a stable power-base identity");
    }
    static const uint8_t all_zero_mac[8] = {0};
    if (memcmp(mac8, all_zero_mac, sizeof(mac8)) == 0) {
        /* An all-zero efuse read is never a valid permanent identifier --
         * same policy child_service.c's compute_permanent_uuid() applies to
         * a sensor module's own uuid (code review follow-up section 1).
         * Proceeding would give every unprovisioned power base the exact
         * same identity, which would be indistinguishable from a real
         * collision in the topology registry. */
        halt_forever("esp_efuse_mac_get_default returned an all-zero EUI-64 -- refusing to use it as this power "
                     "base's identity");
    }
    memcpy(s_own_uuid, mac8, POGO_UUID_LEN);
    s_collector_generation = esp_random();
    if (s_collector_generation == 0) {
        s_collector_generation = 1; /* purely informational field -- 0 is never handed out, no semantic meaning either way */
    }

    ESP_ERROR_CHECK(thread_manager_init());

    if (s_thread_cfg.mode == POWER_BASE_THREAD_MODE_AWAIT_COMMISSIONING) {
        /* Unregistered spare-board state (components/power_base_enrollment).
         * Thread stays disabled: no Dataset left in dynamic NVS is resumed and
         * no network is formed unless the enrollment marker says this board
         * completed a Joiner enrollment AND the stored Dataset matches it.
         * The power rail, exporter, POGO downstream and DETECT_OUT start only
         * from on_enrollment_ready(), i.e. once enrolled -- through the same
         * attach_supervisor_task()/start_services_once() path as modes 0/1. */
        ESP_ERROR_CHECK(thread_manager_stop());
        ESP_LOGW(TAG, "AWAIT_COMMISSIONING: Thread and downstream services are disabled until enrolled");
        if (pb_enrollment_runtime_start(s_own_uuid, on_enrollment_ready) != ESP_OK) {
            halt_forever("ENROLLMENT_RUNTIME_FAILED: could not create the enrollment runtime");
        }
        return;
    }

    /* A failure HERE is a genuine OpenThread API error (bad return from the
     * underlying otThreadSetEnabled/otThreadSetLinkMode/... calls, or
     * esp_openthread_auto_start() itself failing) -- fundamentally different
     * from "has not attached yet", which is NOT a halt condition (see
     * attach_supervisor_task() below). Exactly one of the two branches below
     * ever runs, matching s_thread_cfg.mode determined above -- neither
     * ever falls back to the other on failure (EXTERNAL_OTBR failing here
     * never falls back to forming a standalone network of its own). */
    if (s_thread_cfg.mode == POWER_BASE_THREAD_MODE_EXTERNAL_OTBR) {
        esp_err_t join_err =
            thread_manager_start_as_always_on_ftd_with_dataset(s_thread_cfg.active_tlvs, s_thread_cfg.active_tlvs_len);
        /* The in-RAM copy of the dataset is no longer needed once handed to
         * OpenThread (which keeps its OWN internal copy/storage, untouched
         * by this) -- zeroize this application-level buffer now, regardless
         * of outcome. Never logged before or after. */
        memset(s_thread_cfg.active_tlvs, 0, sizeof(s_thread_cfg.active_tlvs));
        s_thread_cfg.active_tlvs_len = 0;
        if (join_err != ESP_OK) {
            halt_forever("ROLE_TRANSITION_ERROR: thread_manager_start_as_always_on_ftd_with_dataset failed "
                         "(EXTERNAL_OTBR mode -- no standalone fallback)");
        }
    } else {
        if (thread_manager_start_as_always_on_ftd() != ESP_OK) {
            halt_forever("ROLE_TRANSITION_ERROR: thread_manager_start_as_always_on_ftd failed");
        }
    }

    /*
     * The attach wait (and everything that depends on it -- CoAP resources,
     * parent_service, the downstream POGO port, the stale-sweep task) now
     * runs entirely inside attach_supervisor_task(), which owns only
     * file-scope static state (never a pointer to one of THIS function's
     * own locals) so it remains valid for as long as the task itself runs,
     * independent of app_main() returning. app_main() itself has nothing
     * further to do once this task is launched -- see this file's top
     * comment for the full policy (no fixed timeout, no halt_forever() for
     * "not attached yet", exactly-once service startup).
     */
    start_attach_supervisor();
}
