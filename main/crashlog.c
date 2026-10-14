#include "crashlog.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_attr.h"
#include "esp_cpu_utils.h"
#include "esp_debug_helpers.h"
#include "esp_log.h"
#include "esp_private/panic_internal.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "settings.h"
#include "xtensa_context.h"

static const char *TAG = "crashlog";

#define CRASH_MAGIC 0xC4A5DEADu
#define MAX_PCS     16

typedef struct {
    uint32_t magic;
    int32_t  core;
    uint32_t uptime_s;
    char     task[16];
    uint32_t npc;
    uint32_t pcs[MAX_PCS];
    int64_t  when;                // UTC when the next boot found it (0 if the clock wasn't set)
} crash_rec_t;

static RTC_NOINIT_ATTR crash_rec_t s_rtc;   // written by the panic wrapper, survives the restart
static crash_rec_t s_last;                  // what /  shows (from RTC or NVS)
static bool s_have;

void __real_esp_panic_handler(panic_info_t *info);

// Runs in the panic context (cache may be off): IRAM code, RTC data, no logging, no allocation.
void IRAM_ATTR __wrap_esp_panic_handler(panic_info_t *info)
{
    crash_rec_t *r = &s_rtc;
    r->core = info->core;
    r->uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
    r->npc = 0;
    const char *name = NULL;
    TaskHandle_t t = xTaskGetCurrentTaskHandleForCore(info->core);
    if (t) {
        name = pcTaskGetName(t);
    }
    size_t i = 0;
    for (; name && i < sizeof(r->task) - 1 && name[i]; i++) {
        r->task[i] = name[i];
    }
    r->task[i] = '\0';
    const XtExcFrame *f = info->frame;
    if (f) {
        esp_backtrace_frame_t fr = { .pc = f->pc, .sp = f->a1, .next_pc = f->a0, .exc_frame = (void *)f };
        r->pcs[r->npc++] = esp_cpu_process_stack_pc(fr.pc);
        while (r->npc < MAX_PCS && fr.next_pc && esp_backtrace_get_next_frame(&fr)) {
            r->pcs[r->npc++] = esp_cpu_process_stack_pc(fr.pc);
        }
    }
    r->magic = CRASH_MAGIC;
    __real_esp_panic_handler(info);
}

void crashlog_init(void)
{
    const esp_reset_reason_t why = esp_reset_reason();
    if ((why == ESP_RST_PANIC || why == ESP_RST_INT_WDT || why == ESP_RST_TASK_WDT || why == ESP_RST_WDT) &&
        s_rtc.magic == CRASH_MAGIC && s_rtc.npc <= MAX_PCS) {
        s_last = s_rtc;
        s_last.when = time(NULL) > 1700000000 ? time(NULL) : 0;  // RTC chip time at this point of boot
        s_have = true;
        settings_set_cache("crash", &s_last, sizeof(s_last));  // keep it across a power-off
        char buf[300];
        crashlog_text(buf, sizeof(buf));
        ESP_LOGW(TAG, "previous run crashed: %s", buf);
    } else {
        s_have = settings_get_cache("crash", &s_last, sizeof(s_last)) && s_last.magic == CRASH_MAGIC &&
                 s_last.npc <= MAX_PCS;
    }
    s_rtc.magic = 0;  // consumed (a later normal restart must not report it again)
}

void crashlog_text(char *buf, size_t len)
{
    if (!s_have) {
        snprintf(buf, len, "none recorded");
        return;
    }
    char when[24] = "time unknown";
    if (s_last.when) {
        const time_t t = (time_t)s_last.when;
        struct tm tm;
        gmtime_r(&t, &tm);
        strftime(when, sizeof(when), "%d %b %H:%M UTC", &tm);
    }
    int n = snprintf(buf, len, "%s, after %lu s, core %ld, task %s, PCs", when, (unsigned long)s_last.uptime_s,
                     (long)s_last.core, s_last.task[0] ? s_last.task : "?");
    for (uint32_t i = 0; i < s_last.npc && n < (int)len - 12; i++) {
        n += snprintf(buf + n, len - n, " 0x%08lx", (unsigned long)s_last.pcs[i]);
    }
}
