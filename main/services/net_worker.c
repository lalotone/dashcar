#include <stdbool.h>
#include "net_worker.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "net_worker";

#define STACK      10240
#define QUEUE_LEN  8
#define MAX_STEPS  6

typedef struct {
    net_job_fn_t fn;
    void *arg;
} job_t;

static QueueHandle_t s_queue;
static void (*s_steps[MAX_STEPS])(void);
static int s_nsteps;

static void worker_task(void *arg)
{
    for (;;) {
        job_t job;
        // On-demand jobs first (a screen is waiting); otherwise poll the services.
        if (xQueueReceive(s_queue, &job, pdMS_TO_TICKS(1000)) == pdTRUE) {
            job.fn(job.arg);
            continue;
        }
        for (int i = 0; i < s_nsteps; i++) {
            s_steps[i]();
            if (uxQueueMessagesWaiting(s_queue) > 0) {
                break;  // let the waiting job run before the remaining (slower) steps
            }
        }
    }
}

void net_worker_init(void)
{
    s_queue = xQueueCreate(QUEUE_LEN, sizeof(job_t));
    xTaskCreatePinnedToCore(worker_task, "net_worker", STACK, NULL, 3, NULL, 0);
}

void net_worker_register_step(void (*step)(void))
{
    if (s_nsteps < MAX_STEPS) {
        s_steps[s_nsteps++] = step;
    } else {
        ESP_LOGE(TAG, "too many steps");
    }
}

bool net_worker_post(net_job_fn_t fn, void *arg)
{
    job_t job = { fn, arg };
    return xQueueSend(s_queue, &job, 0) == pdTRUE;
}
