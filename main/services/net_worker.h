#pragma once

// One task runs every network job (TLS, downloads, parsing), one at a time:
//  - periodic "steps" registered by the services (weather, warnings, traffic, rain map), called
//    about once a second; each decides itself whether a fetch is due and returns quickly if not;
//  - on-demand jobs posted by screens (fuel search, city search, radar loader).
// Serialising them keeps only one TLS session alive (internal-RAM peaks used to hit ~8 KB when
// five services connected at boot), and nothing creates tasks at runtime, so a screen can't
// fail with "out of memory" because internal RAM is fragmented.
// The worker's stack is internal: jobs may write flash (tile cache, NVS).

typedef void (*net_job_fn_t)(void *arg);

void net_worker_init(void);
void net_worker_register_step(void (*step)(void));
// Queue a one-off job; false if the queue is full (the caller should report an error).
bool net_worker_post(net_job_fn_t fn, void *arg);
