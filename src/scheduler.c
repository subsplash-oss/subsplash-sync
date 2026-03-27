#include "scheduler.h"
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <obs-module.h>
#include "plugin-support.h"

static void scheduler_poll_once(scheduler_t *sched);
static void *scheduler_thread_func(void *arg);

bool scheduler_init(scheduler_t *sched)
{
	memset(sched, 0, sizeof(*sched));
	pthread_mutex_init(&sched->lock, NULL);
	pthread_mutex_init(&sched->stop_mutex, NULL);
	pthread_cond_init(&sched->stop_cond, NULL);
	sched->poll_interval_sec = 30;
	sched->start_lead_minutes = 1;
	sched->stop_lag_minutes = 0;
	return true;
}

void scheduler_configure(scheduler_t *sched, const char *base_url,
			 const char *client_id, const char *client_secret,
			 const char *app_key, int poll_interval_sec,
			 int start_lead_minutes, int stop_lag_minutes)
{
	subsplash_client_init(&sched->api, base_url, client_id, client_secret,
			      app_key);
	sched->poll_interval_sec = poll_interval_sec;
	sched->start_lead_minutes = start_lead_minutes;
	sched->stop_lag_minutes = stop_lag_minutes;
}

bool scheduler_start(scheduler_t *sched)
{
	if (sched->running)
		return true;

	sched->stop_requested = false;
	sched->running = true;

	if (pthread_create(&sched->thread, NULL, scheduler_thread_func,
			   sched) != 0) {
		sched->running = false;
		obs_log(LOG_ERROR, "Failed to create scheduler thread");
		return false;
	}

	return true;
}

void scheduler_stop(scheduler_t *sched)
{
	if (!sched->running)
		return;

	pthread_mutex_lock(&sched->stop_mutex);
	sched->stop_requested = true;
	pthread_cond_signal(&sched->stop_cond);
	pthread_mutex_unlock(&sched->stop_mutex);

	pthread_join(sched->thread, NULL);
	sched->running = false;

	subsplash_client_destroy(&sched->api);
}

void scheduler_destroy(scheduler_t *sched)
{
	if (sched->running)
		scheduler_stop(sched);

	pthread_mutex_destroy(&sched->lock);
	pthread_mutex_destroy(&sched->stop_mutex);
	pthread_cond_destroy(&sched->stop_cond);
}

long scheduler_consume_action(scheduler_t *sched)
{
	return __sync_lock_test_and_set(&sched->action, SCHED_ACTION_NONE);
}

void scheduler_get_status(scheduler_t *sched, char *status, size_t status_len,
			  char *next_bc, size_t next_bc_len)
{
	pthread_mutex_lock(&sched->lock);
	if (status && status_len > 0)
		snprintf(status, status_len, "%s", sched->status_text);
	if (next_bc && next_bc_len > 0)
		snprintf(next_bc, next_bc_len, "%s",
			 sched->next_broadcast_info);
	pthread_mutex_unlock(&sched->lock);
}

static void *scheduler_thread_func(void *arg)
{
	scheduler_t *sched = (scheduler_t *)arg;

	obs_log(LOG_INFO, "Scheduler thread started");

	while (!sched->stop_requested) {
		struct timespec ts;
		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_sec += sched->poll_interval_sec;

		pthread_mutex_lock(&sched->stop_mutex);
		pthread_cond_timedwait(&sched->stop_cond, &sched->stop_mutex,
				       &ts);
		pthread_mutex_unlock(&sched->stop_mutex);

		if (sched->stop_requested)
			break;

		scheduler_poll_once(sched);
	}

	obs_log(LOG_INFO, "Scheduler thread stopped");
	return NULL;
}

static void scheduler_poll_once(scheduler_t *sched)
{
	subsplash_broadcast_t broadcast;

	if (!subsplash_client_fetch_upcoming(&sched->api, &broadcast)) {
		pthread_mutex_lock(&sched->lock);
		snprintf(sched->status_text, sizeof(sched->status_text),
			 "API error");
		pthread_mutex_unlock(&sched->lock);
		return;
	}

	if (!broadcast.valid) {
		pthread_mutex_lock(&sched->lock);
		snprintf(sched->status_text, sizeof(sched->status_text),
			 "No upcoming broadcasts");
		sched->next_broadcast_info[0] = '\0';
		pthread_mutex_unlock(&sched->lock);
		return;
	}

	time_t now = time(NULL);
	time_t trigger_start =
		broadcast.start_epoch - (sched->start_lead_minutes * 60);
	time_t trigger_stop =
		broadcast.end_epoch + (sched->stop_lag_minutes * 60);

	if (strcmp(sched->acted_broadcast_id, broadcast.id) != 0) {
		sched->acted_started = false;
		sched->acted_stopped = false;
		snprintf(sched->acted_broadcast_id,
			 sizeof(sched->acted_broadcast_id), "%s",
			 broadcast.id);
	}

	if (strcmp(broadcast.status, "scheduled") == 0 &&
	    !sched->acted_started && now >= trigger_start &&
	    now < broadcast.end_epoch) {
		if (broadcast.simulated_live) {
			obs_log(LOG_INFO,
				"Skipping START for simulated-live broadcast %s",
				broadcast.id);
		} else {
			__sync_lock_test_and_set(&sched->action,
						 SCHED_ACTION_START);
			obs_log(LOG_INFO, "Signaling START for broadcast %s",
				broadcast.id);
		}
		sched->acted_started = true;
	}

	if ((strcmp(broadcast.status, "live") == 0 ||
	     strcmp(broadcast.status, "scheduled") == 0) &&
	    !sched->acted_stopped && now >= trigger_stop) {
		if (broadcast.simulated_live) {
			obs_log(LOG_INFO,
				"Skipping STOP for simulated-live broadcast %s",
				broadcast.id);
		} else {
			__sync_lock_test_and_set(&sched->action,
						 SCHED_ACTION_STOP);
			obs_log(LOG_INFO, "Signaling STOP for broadcast %s",
				broadcast.id);
		}
		sched->acted_stopped = true;
	}

	pthread_mutex_lock(&sched->lock);
	snprintf(sched->status_text, sizeof(sched->status_text), "Monitoring");
	snprintf(sched->next_broadcast_info,
		 sizeof(sched->next_broadcast_info),
		 "Next: %s to %s [%s]%s", broadcast.start_at,
		 broadcast.end_at, broadcast.status,
		 broadcast.simulated_live ? " (simulated)" : "");
	pthread_mutex_unlock(&sched->lock);
}
