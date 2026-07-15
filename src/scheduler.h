#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "subsplash-api.h"
#include <pthread.h>
#include <stdbool.h>

#define SCHED_ACTION_NONE    0
#define SCHED_ACTION_START   1
#define SCHED_ACTION_STOP    2
#define SCHED_ACTION_RESTART 3

/*
 * Start lead must not exceed 2 minutes or the stream won't be
 * associated with the scheduled broadcast event at start time.
 */
#define SCHED_DEFAULT_START_LEAD_MINUTES 2
#define SCHED_DEFAULT_STOP_LAG_MINUTES   2

#define SCHED_BACKOFF_INITIAL_SEC 2
#define SCHED_BACKOFF_MAX_SEC     60

#define SCHED_POLL_MIN_SEC  30
#define SCHED_POLL_IDLE_SEC 300

/*
 * Delay between stop and start during a broadcast transition.
 * Must exceed 20s so the old session is fully ended before the
 * new connection is made.
 */
#define SCHED_RESTART_DELAY_SEC 30

typedef struct {
	subsplash_client_t api;
	pthread_t thread;
	pthread_mutex_t lock;
	volatile bool running;
	volatile long action;
	int start_lead_minutes;
	int stop_lag_minutes;

	/* Tracked broadcast state. */
	char acted_broadcast_id[SUBSPLASH_MAX_ID];
	bool acted_started;
	bool acted_stopped;

	/*
	 * Cached start/end times from the tracked broadcast. Start is
	 * used to derive the adaptive poll interval; end is the primary
	 * STOP signal and safety net when the API is unreachable.
	 */
	time_t cached_start_epoch;
	time_t cached_end_epoch;

	/* Retry backoff state for transient API failures. */
	int consecutive_failures;

	/*
	 * Set once the initial poll completes so the UI can distinguish a
	 * not-yet-confirmed "connecting" state from a genuinely connected one
	 * (consecutive_failures == 0 is true both before the first poll and
	 * after a successful one). Reset on each start. Accessed atomically as
	 * it's written by the poll thread and read by the UI thread.
	 */
	volatile long first_poll_done;

	/*
	 * Absolute (CLOCK_REALTIME) time of the next scheduled automatic
	 * poll, so the dock can show a "Next refresh" countdown. Set by the
	 * poll loop before each wait; 0 when not yet computed or stopped.
	 */
	time_t next_poll_epoch;

	/* UI-visible status strings. */
	char status_text[256];
	char next_broadcast_info[256];
	char last_activity[256];

	pthread_cond_t stop_cond;
	pthread_mutex_t stop_mutex;
	volatile bool stop_requested;
} scheduler_t;

bool scheduler_init(scheduler_t *scheduler);
void scheduler_configure(scheduler_t *scheduler, const char *base_url, const char *client_id, const char *client_secret,
			 const char *app_key, int start_lead_minutes, int stop_lag_minutes);
bool scheduler_start(scheduler_t *scheduler);
void scheduler_stop(scheduler_t *scheduler);
void scheduler_destroy(scheduler_t *scheduler);
long scheduler_consume_action(scheduler_t *scheduler);
void scheduler_wake(scheduler_t *scheduler);
void scheduler_get_status(scheduler_t *scheduler, char *status, size_t status_len, char *next_broadcast,
			  size_t next_broadcast_len, char *last_activity, size_t last_activity_len);
int compute_poll_interval(const scheduler_t *scheduler);
time_t scheduler_get_next_poll_epoch(scheduler_t *scheduler);

#ifdef __cplusplus
}
#endif
