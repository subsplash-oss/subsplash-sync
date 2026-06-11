#include "scheduler.h"

#include "plugin-support.h"

#include <obs-module.h>

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static void scheduler_poll_once(scheduler_t *scheduler);
static void *scheduler_thread_func(void *arg);

/* ------------------------------------------------------------------ */
/* Jittered backoff: 2s, 4s, 8s ... capped at 60s.                   */
/* ------------------------------------------------------------------ */

static int compute_backoff_sec(int consecutive_failures)
{
	if (consecutive_failures <= 0)
		return 0;

	int base_delay_sec = SCHED_BACKOFF_INITIAL_SEC;
	for (int i = 1; i < consecutive_failures && i < 10; i++)
		base_delay_sec *= 2;

	if (base_delay_sec > SCHED_BACKOFF_MAX_SEC)
		base_delay_sec = SCHED_BACKOFF_MAX_SEC;

	/* Add jitter: 50-100% of the computed delay. */
	int jitter = base_delay_sec / 2 + (rand() % (base_delay_sec / 2 + 1));
	return jitter;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                          */
/* ------------------------------------------------------------------ */

bool scheduler_init(scheduler_t *scheduler)
{
	memset(scheduler, 0, sizeof(*scheduler));
	pthread_mutex_init(&scheduler->lock, NULL);
	pthread_mutex_init(&scheduler->stop_mutex, NULL);
	pthread_cond_init(&scheduler->stop_cond, NULL);
	scheduler->poll_interval_sec = 30;
	scheduler->start_lead_minutes = SCHED_DEFAULT_START_LEAD_MINUTES;
	scheduler->stop_lag_minutes = SCHED_DEFAULT_STOP_LAG_MINUTES;
	scheduler->cached_end_epoch = 0;
	return true;
}

void scheduler_configure(scheduler_t *scheduler, const char *base_url, const char *client_id, const char *client_secret,
			 const char *app_key, int poll_interval_sec, int start_lead_minutes, int stop_lag_minutes)
{
	subsplash_client_init(&scheduler->api, base_url, client_id, client_secret, app_key);
	scheduler->poll_interval_sec = poll_interval_sec;
	scheduler->start_lead_minutes = start_lead_minutes;
	scheduler->stop_lag_minutes = stop_lag_minutes;
}

bool scheduler_start(scheduler_t *scheduler)
{
	if (scheduler->running)
		return true;

	scheduler->stop_requested = false;
	scheduler->running = true;
	scheduler->acted_started = false;
	scheduler->acted_stopped = false;
	scheduler->acted_broadcast_id[0] = '\0';
	scheduler->cached_end_epoch = 0;
	scheduler->consecutive_failures = 0;

	if (pthread_create(&scheduler->thread, NULL, scheduler_thread_func, scheduler) != 0) {
		scheduler->running = false;
		obs_log(LOG_ERROR, "Failed to create scheduler thread");
		return false;
	}

	return true;
}

void scheduler_stop(scheduler_t *scheduler)
{
	if (!scheduler->running)
		return;

	pthread_mutex_lock(&scheduler->stop_mutex);
	scheduler->stop_requested = true;
	pthread_cond_signal(&scheduler->stop_cond);
	pthread_mutex_unlock(&scheduler->stop_mutex);

	pthread_join(scheduler->thread, NULL);
	scheduler->running = false;

	subsplash_client_destroy(&scheduler->api);
}

void scheduler_destroy(scheduler_t *scheduler)
{
	if (scheduler->running)
		scheduler_stop(scheduler);

	pthread_mutex_destroy(&scheduler->lock);
	pthread_mutex_destroy(&scheduler->stop_mutex);
	pthread_cond_destroy(&scheduler->stop_cond);
}

long scheduler_consume_action(scheduler_t *scheduler)
{
	return __sync_lock_test_and_set(&scheduler->action, SCHED_ACTION_NONE);
}

void scheduler_get_status(scheduler_t *scheduler, char *status, size_t status_len, char *next_broadcast,
			  size_t next_broadcast_len, char *last_activity, size_t last_activity_len)
{
	pthread_mutex_lock(&scheduler->lock);
	if (status && status_len > 0)
		snprintf(status, status_len, "%s", scheduler->status_text);
	if (next_broadcast && next_broadcast_len > 0)
		snprintf(next_broadcast, next_broadcast_len, "%s", scheduler->next_broadcast_info);
	if (last_activity && last_activity_len > 0)
		snprintf(last_activity, last_activity_len, "%s", scheduler->last_activity);
	pthread_mutex_unlock(&scheduler->lock);
}

/* ------------------------------------------------------------------ */
/* Thread entry point                                                 */
/* ------------------------------------------------------------------ */

static void *scheduler_thread_func(void *arg)
{
	scheduler_t *scheduler = (scheduler_t *)arg;

	obs_log(LOG_INFO, "Scheduler thread started");

	/* Poll immediately on startup for crash recovery. */
	scheduler_poll_once(scheduler);

	while (!scheduler->stop_requested) {
		int wait_sec = scheduler->poll_interval_sec;

		if (scheduler->consecutive_failures > 0) {
			int backoff_sec = compute_backoff_sec(scheduler->consecutive_failures);
			if (backoff_sec > wait_sec)
				wait_sec = backoff_sec;
		}

		struct timespec ts;
		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_sec += wait_sec;

		pthread_mutex_lock(&scheduler->stop_mutex);
		pthread_cond_timedwait(&scheduler->stop_cond, &scheduler->stop_mutex, &ts);
		pthread_mutex_unlock(&scheduler->stop_mutex);

		if (scheduler->stop_requested)
			break;

		scheduler_poll_once(scheduler);
	}

	obs_log(LOG_INFO, "Scheduler thread stopped");
	return NULL;
}

/* ------------------------------------------------------------------ */
/* Format a UTC epoch as a local-time string for the UI.              */
/* ------------------------------------------------------------------ */

static void format_local_time(time_t epoch, char *buf, size_t buf_size)
{
	struct tm local_time;
#if defined(_WIN32)
	localtime_s(&local_time, &epoch);
#else
	localtime_r(&epoch, &local_time);
#endif
	strftime(buf, buf_size, "%Y-%m-%d %H:%M", &local_time);
}

/* ------------------------------------------------------------------ */
/* Helper: update UI status strings under the lock.                   */
/* ------------------------------------------------------------------ */

static void set_status(scheduler_t *scheduler, const char *status, const char *broadcast_info)
{
	char time_str[16];
	time_t now = time(NULL);
	struct tm local_time;
#if defined(_WIN32)
	localtime_s(&local_time, &now);
#else
	localtime_r(&now, &local_time);
#endif
	strftime(time_str, sizeof(time_str), "%H:%M:%S", &local_time);

	pthread_mutex_lock(&scheduler->lock);
	snprintf(scheduler->status_text, sizeof(scheduler->status_text), "%s", status);
	if (broadcast_info) {
		snprintf(scheduler->next_broadcast_info, sizeof(scheduler->next_broadcast_info), "%s", broadcast_info);
	}
	snprintf(scheduler->last_activity, sizeof(scheduler->last_activity), "[%s] %s", time_str, status);
	pthread_mutex_unlock(&scheduler->lock);
}

/* ------------------------------------------------------------------ */
/* Check the cached end time as a safety net when the API might be    */
/* unreachable or the broadcast has dropped from list results.        */
/* ------------------------------------------------------------------ */

static void check_cached_stop(scheduler_t *scheduler, time_t now)
{
	if (scheduler->cached_end_epoch <= 0)
		return;
	if (scheduler->acted_stopped)
		return;

	time_t trigger_stop = scheduler->cached_end_epoch + (scheduler->stop_lag_minutes * 60);

	if (now >= trigger_stop) {
		obs_log(LOG_INFO, "Signaling STOP from cached end time for broadcast %s",
			scheduler->acted_broadcast_id);
		__sync_lock_test_and_set(&scheduler->action, SCHED_ACTION_STOP);
		scheduler->acted_stopped = true;
	}
}

/* ------------------------------------------------------------------ */
/* Core poll logic                                                    */
/* ------------------------------------------------------------------ */

static void scheduler_poll_once(scheduler_t *scheduler)
{
	subsplash_broadcast_t broadcast;
	int fetch_result = subsplash_client_fetch_broadcasts(&scheduler->api, &broadcast);

	if (fetch_result == SUBSPLASH_FETCH_AUTH_ERROR) {
		scheduler->consecutive_failures++;
		obs_log(LOG_WARNING, "subsplash: not authorized (attempt %d), backing off",
			scheduler->consecutive_failures);

		/*
		 * Failure-open: keep existing state and check cached
		 * end time so STOP still fires if scheduled.
		 */
		check_cached_stop(scheduler, time(NULL));
		set_status(scheduler, "Not authorized", "Check client role and app key");
		return;
	}

	if (fetch_result == SUBSPLASH_FETCH_API_ERROR) {
		scheduler->consecutive_failures++;
		obs_log(LOG_WARNING, "subsplash: API error (attempt %d), backing off", scheduler->consecutive_failures);

		/*
		 * Failure-open: keep existing state and check cached
		 * end time so STOP still fires if scheduled.
		 */
		check_cached_stop(scheduler, time(NULL));
		set_status(scheduler, "API error (retrying)", NULL);
		return;
	}

	scheduler->consecutive_failures = 0;

	if (fetch_result == SUBSPLASH_FETCH_NO_DATA || !broadcast.valid) {
		/*
		 * No broadcasts in the window. If we were tracking one,
		 * use GetOne to confirm its final status before giving up.
		 */
		if (scheduler->acted_broadcast_id[0] != '\0' && !scheduler->acted_stopped) {
			subsplash_broadcast_t tracked;
			int by_id_result =
				subsplash_client_fetch_by_id(&scheduler->api, scheduler->acted_broadcast_id, &tracked);

			if (by_id_result == SUBSPLASH_FETCH_OK && tracked.valid) {
				bool is_terminal = strcmp(tracked.status, "ended") == 0 ||
						   strcmp(tracked.status, "never-happened") == 0 ||
						   strcmp(tracked.status, "on-demand") == 0;

				if (is_terminal && !tracked.simulated_live) {
					obs_log(LOG_INFO, "Signaling STOP: tracked broadcast %s is now %s", tracked.id,
						tracked.status);
					__sync_lock_test_and_set(&scheduler->action, SCHED_ACTION_STOP);
					scheduler->acted_stopped = true;
				}
			}

			check_cached_stop(scheduler, time(NULL));
		}

		set_status(scheduler, "No upcoming broadcasts", "");
		return;
	}

	time_t now = time(NULL);
	time_t trigger_start = broadcast.start_epoch - (scheduler->start_lead_minutes * 60);
	time_t trigger_stop = broadcast.end_epoch + (scheduler->stop_lag_minutes * 60);

	/* Track a new broadcast when the ID changes. */
	if (strcmp(scheduler->acted_broadcast_id, broadcast.id) != 0) {
		/*
		 * If we started streaming for the previous broadcast but
		 * never stopped, we owe a clean disconnect so the backend
		 * closes the old session before we connect for the new
		 * event.  Return immediately so the RESTART action isn't
		 * overwritten by a START for the new broadcast in the
		 * same poll cycle.
		 */
		if (scheduler->acted_started && !scheduler->acted_stopped) {
			obs_log(LOG_INFO,
				"Broadcast transition: %s -> %s, "
				"signaling RESTART",
				scheduler->acted_broadcast_id, broadcast.id);
			__sync_lock_test_and_set(&scheduler->action, SCHED_ACTION_RESTART);
		}

		scheduler->acted_started = false;
		scheduler->acted_stopped = false;
		snprintf(scheduler->acted_broadcast_id, sizeof(scheduler->acted_broadcast_id), "%s", broadcast.id);
		scheduler->cached_end_epoch = broadcast.end_epoch;
		obs_log(LOG_INFO, "Now tracking broadcast %s", broadcast.id);

		if (__sync_fetch_and_add(&scheduler->action, 0) == SCHED_ACTION_RESTART) {
			char start_local[32], end_local[32];
			format_local_time(broadcast.start_epoch, start_local, sizeof(start_local));
			format_local_time(broadcast.end_epoch, end_local, sizeof(end_local));
			char broadcast_info[256];
			snprintf(broadcast_info, sizeof(broadcast_info), "%s to %s [%s]%s", start_local, end_local,
				 broadcast.status, broadcast.simulated_live ? " (simulated)" : "");
			set_status(scheduler, "Transitioning", broadcast_info);
			return;
		}
	}

	scheduler->cached_end_epoch = broadcast.end_epoch;

	bool is_scheduled = strcmp(broadcast.status, "scheduled") == 0;
	bool is_live = strcmp(broadcast.status, "live") == 0;
	bool is_terminal = strcmp(broadcast.status, "ended") == 0 || strcmp(broadcast.status, "never-happened") == 0 ||
			   strcmp(broadcast.status, "on-demand") == 0;

	/*
	 * Early termination: broadcast was canceled or ended before the
	 * scheduled end time.
	 */
	if (is_terminal && scheduler->acted_started && !scheduler->acted_stopped) {
		if (!broadcast.simulated_live) {
			obs_log(LOG_INFO, "Signaling STOP: broadcast %s status is %s", broadcast.id, broadcast.status);
			__sync_lock_test_and_set(&scheduler->action, SCHED_ACTION_STOP);
		}
		scheduler->acted_stopped = true;
	}

	/*
	 * START: trigger when the broadcast is scheduled (or live for crash
	 * recovery) and we're within the start window.
	 */
	if ((is_scheduled || is_live) && !scheduler->acted_started && now >= trigger_start &&
	    now < broadcast.end_epoch) {
		if (broadcast.simulated_live) {
			obs_log(LOG_INFO, "Skipping START for simulated-live broadcast %s", broadcast.id);
		} else {
			__sync_lock_test_and_set(&scheduler->action, SCHED_ACTION_START);
			obs_log(LOG_INFO, "Signaling START for broadcast %s", broadcast.id);
		}
		scheduler->acted_started = true;
	}

	/*
	 * STOP: primary time-based signal. Fires when the scheduled end
	 * time (plus lag) has passed, regardless of broadcast status.
	 */
	if ((is_scheduled || is_live) && !scheduler->acted_stopped && now >= trigger_stop) {
		if (broadcast.simulated_live) {
			obs_log(LOG_INFO, "Skipping STOP for simulated-live broadcast %s", broadcast.id);
		} else {
			__sync_lock_test_and_set(&scheduler->action, SCHED_ACTION_STOP);
			obs_log(LOG_INFO, "Signaling STOP for broadcast %s (end time reached)", broadcast.id);
		}
		scheduler->acted_stopped = true;
	}

	char start_local[32], end_local[32];
	format_local_time(broadcast.start_epoch, start_local, sizeof(start_local));
	format_local_time(broadcast.end_epoch, end_local, sizeof(end_local));
	char broadcast_info[256];
	snprintf(broadcast_info, sizeof(broadcast_info), "%s to %s [%s]%s", start_local, end_local, broadcast.status,
		 broadcast.simulated_live ? " (simulated)" : "");
	set_status(scheduler, "Monitoring", broadcast_info);
}
