#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "subsplash-api.h"
#include <pthread.h>
#include <stdbool.h>

#define SCHED_ACTION_NONE  0
#define SCHED_ACTION_START 1
#define SCHED_ACTION_STOP  2

typedef struct {
	subsplash_client_t api;
	pthread_t thread;
	pthread_mutex_t lock;
	volatile bool running;
	volatile long action;
	int poll_interval_sec;
	int start_lead_minutes;
	int stop_lag_minutes;

	char acted_broadcast_id[64];
	bool acted_started;
	bool acted_stopped;

	char status_text[256];
	char next_broadcast_info[256];

	pthread_cond_t stop_cond;
	pthread_mutex_t stop_mutex;
	volatile bool stop_requested;
} scheduler_t;

bool scheduler_init(scheduler_t *sched);
void scheduler_configure(scheduler_t *sched, const char *base_url,
			 const char *client_id, const char *client_secret,
			 const char *app_key, int poll_interval_sec,
			 int start_lead_minutes, int stop_lag_minutes);
bool scheduler_start(scheduler_t *sched);
void scheduler_stop(scheduler_t *sched);
void scheduler_destroy(scheduler_t *sched);
long scheduler_consume_action(scheduler_t *sched);
void scheduler_get_status(scheduler_t *sched, char *status, size_t status_len,
			  char *next_bc, size_t next_bc_len);

#ifdef __cplusplus
}
#endif
