/*
 * Unit tests for scheduler.c
 *
 * Static functions are accessed by #include-ing the source file
 * directly.  The subsplash API functions called by the scheduler
 * are replaced with controllable mocks defined below.
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <string.h>
#include <time.h>

/* Pull in headers for types before defining mocks. */
#include "subsplash-api.h"
#include "scheduler.h"

/* ------------------------------------------------------------------ */
/* Mock API layer                                                     */
/* ------------------------------------------------------------------ */

static int mock_fetch_result;
static subsplash_broadcast_t mock_broadcast;
static int mock_by_id_result;
static subsplash_broadcast_t mock_by_id_broadcast;

bool subsplash_client_init(subsplash_client_t *client, const char *base_url, const char *client_id,
			   const char *client_secret, const char *app_key)
{
	(void)client;
	(void)base_url;
	(void)client_id;
	(void)client_secret;
	(void)app_key;
	return true;
}

void subsplash_client_destroy(subsplash_client_t *client)
{
	(void)client;
}

void subsplash_client_abort(subsplash_client_t *client)
{
	(void)client;
}

int subsplash_client_fetch_broadcasts(subsplash_client_t *client, subsplash_broadcast_t *out)
{
	(void)client;
	if (out)
		*out = mock_broadcast;
	return mock_fetch_result;
}

int subsplash_client_fetch_by_id(subsplash_client_t *client, const char *id, subsplash_broadcast_t *out)
{
	(void)client;
	(void)id;
	if (out)
		*out = mock_by_id_broadcast;
	return mock_by_id_result;
}

/*
 * Include scheduler.c to gain access to its static functions.
 * Headers already included above are skipped via #pragma once.
 */
#include "scheduler.c"

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

static void reset_mocks(void)
{
	mock_fetch_result = SUBSPLASH_FETCH_NO_DATA;
	memset(&mock_broadcast, 0, sizeof(mock_broadcast));
	mock_by_id_result = SUBSPLASH_FETCH_NO_DATA;
	memset(&mock_by_id_broadcast, 0, sizeof(mock_by_id_broadcast));
}

static void init_test_scheduler(scheduler_t *s)
{
	memset(s, 0, sizeof(*s));
	pthread_mutex_init(&s->lock, NULL);
	pthread_mutex_init(&s->stop_mutex, NULL);
	pthread_cond_init(&s->stop_cond, NULL);
	s->start_lead_minutes = SCHED_DEFAULT_START_LEAD_MINUTES;
	s->stop_lag_minutes = SCHED_DEFAULT_STOP_LAG_MINUTES;
	s->cached_start_epoch = 0;
	s->cached_end_epoch = 0;
}

static void destroy_test_scheduler(scheduler_t *s)
{
	pthread_mutex_destroy(&s->lock);
	pthread_mutex_destroy(&s->stop_mutex);
	pthread_cond_destroy(&s->stop_cond);
}

static void make_broadcast(subsplash_broadcast_t *b, const char *id, const char *status, time_t start, time_t end,
			   bool simulated_live)
{
	memset(b, 0, sizeof(*b));
	snprintf(b->id, sizeof(b->id), "%s", id);
	snprintf(b->status, sizeof(b->status), "%s", status);
	b->start_epoch = start;
	b->end_epoch = end;
	b->simulated_live = simulated_live;
	b->valid = true;
	snprintf(b->start_at, sizeof(b->start_at), "start");
	snprintf(b->end_at, sizeof(b->end_at), "end");
}

/* ================================================================== */
/* compute_backoff_sec tests                                          */
/* ================================================================== */

static void test_backoff_zero_failures(void **state)
{
	(void)state;
	assert_int_equal(compute_backoff_sec(0), 0);
}

static void test_backoff_negative_failures(void **state)
{
	(void)state;
	assert_int_equal(compute_backoff_sec(-1), 0);
}

static void test_backoff_one_failure(void **state)
{
	(void)state;
	/* base_delay = 2, jitter in [1, 2]. */
	for (int i = 0; i < 50; i++) {
		int val = compute_backoff_sec(1);
		assert_in_range(val, 1, SCHED_BACKOFF_INITIAL_SEC);
	}
}

static void test_backoff_two_failures(void **state)
{
	(void)state;
	/* base_delay = 4, jitter in [2, 4]. */
	for (int i = 0; i < 50; i++) {
		int val = compute_backoff_sec(2);
		assert_in_range(val, 2, 4);
	}
}

static void test_backoff_cap(void **state)
{
	(void)state;
	for (int i = 0; i < 50; i++) {
		int val = compute_backoff_sec(100);
		assert_in_range(val, SCHED_BACKOFF_MAX_SEC / 2, SCHED_BACKOFF_MAX_SEC);
	}
}

/* ================================================================== */
/* check_cached_stop tests                                            */
/* ================================================================== */

static void test_cached_stop_no_cache(void **state)
{
	(void)state;
	scheduler_t s;
	init_test_scheduler(&s);
	s.cached_end_epoch = 0;

	check_cached_stop(&s, time(NULL) + 9999);
	assert_int_equal(s.action, SCHED_ACTION_NONE);

	destroy_test_scheduler(&s);
}

static void test_cached_stop_before_trigger(void **state)
{
	(void)state;
	scheduler_t s;
	init_test_scheduler(&s);
	s.stop_lag_minutes = 2;
	s.cached_end_epoch = time(NULL) + 600;

	check_cached_stop(&s, time(NULL));
	assert_int_equal(s.action, SCHED_ACTION_NONE);
	assert_false(s.acted_stopped);

	destroy_test_scheduler(&s);
}

static void test_cached_stop_at_trigger(void **state)
{
	(void)state;
	scheduler_t s;
	init_test_scheduler(&s);
	s.stop_lag_minutes = 2;
	s.cached_end_epoch = 1000;
	snprintf(s.acted_broadcast_id, sizeof(s.acted_broadcast_id), "bc-1");

	/* trigger_stop = 1000 + 120 = 1120. */
	check_cached_stop(&s, 1120);
	assert_int_equal(s.action, SCHED_ACTION_STOP);
	assert_true(s.acted_stopped);

	destroy_test_scheduler(&s);
}

static void test_cached_stop_already_stopped(void **state)
{
	(void)state;
	scheduler_t s;
	init_test_scheduler(&s);
	s.stop_lag_minutes = 0;
	s.cached_end_epoch = 100;
	s.acted_stopped = true;

	check_cached_stop(&s, 9999);
	assert_int_equal(s.action, SCHED_ACTION_NONE);

	destroy_test_scheduler(&s);
}

/* ================================================================== */
/* scheduler_poll_once tests                                          */
/* ================================================================== */

static void test_poll_api_error(void **state)
{
	(void)state;
	scheduler_t s;
	init_test_scheduler(&s);
	reset_mocks();
	mock_fetch_result = SUBSPLASH_FETCH_API_ERROR;

	scheduler_poll_once(&s);
	assert_int_equal(s.consecutive_failures, 1);

	scheduler_poll_once(&s);
	assert_int_equal(s.consecutive_failures, 2);

	destroy_test_scheduler(&s);
}

static void test_poll_no_data(void **state)
{
	(void)state;
	scheduler_t s;
	init_test_scheduler(&s);
	reset_mocks();
	mock_fetch_result = SUBSPLASH_FETCH_NO_DATA;

	scheduler_poll_once(&s);
	assert_string_equal(s.status_text, "No upcoming broadcasts");

	destroy_test_scheduler(&s);
}

static void test_poll_start_in_window(void **state)
{
	(void)state;
	scheduler_t s;
	init_test_scheduler(&s);
	reset_mocks();

	time_t now = time(NULL);
	make_broadcast(&mock_broadcast, "bc-100", "scheduled", now + 60, now + 3600, false);
	mock_fetch_result = SUBSPLASH_FETCH_OK;

	scheduler_poll_once(&s);
	assert_int_equal(s.action, SCHED_ACTION_START);
	assert_true(s.acted_started);
	assert_int_equal(s.consecutive_failures, 0);

	destroy_test_scheduler(&s);
}

static void test_poll_before_start_window(void **state)
{
	(void)state;
	scheduler_t s;
	init_test_scheduler(&s);
	reset_mocks();

	time_t now = time(NULL);
	/* start_lead = 2 min, start is 5 min away. */
	make_broadcast(&mock_broadcast, "bc-101", "scheduled", now + 300, now + 3600, false);
	mock_fetch_result = SUBSPLASH_FETCH_OK;

	scheduler_poll_once(&s);
	assert_int_equal(s.action, SCHED_ACTION_NONE);
	assert_false(s.acted_started);

	destroy_test_scheduler(&s);
}

static void test_poll_stop_past_end(void **state)
{
	(void)state;
	scheduler_t s;
	init_test_scheduler(&s);
	reset_mocks();

	time_t now = time(NULL);
	/* Broadcast already ended, end + lag has passed. */
	make_broadcast(&mock_broadcast, "bc-200", "scheduled", now - 7200, now - 600, false);
	mock_fetch_result = SUBSPLASH_FETCH_OK;

	/*
	 * First poll: broadcast is past end, also past start window
	 * so both START and STOP fire.  STOP overwrites START in the
	 * action field.
	 */
	scheduler_poll_once(&s);
	assert_int_equal(s.action, SCHED_ACTION_STOP);
	assert_true(s.acted_stopped);

	destroy_test_scheduler(&s);
}

static void test_poll_transition_far_future_stops(void **state)
{
	(void)state;
	scheduler_t s;
	init_test_scheduler(&s);
	reset_mocks();

	time_t now = time(NULL);

	/* First broadcast: started but not stopped. */
	make_broadcast(&mock_broadcast, "bc-300", "live", now - 3600, now + 3600, false);
	mock_fetch_result = SUBSPLASH_FETCH_OK;

	scheduler_poll_once(&s);
	assert_true(s.acted_started);
	s.action = SCHED_ACTION_NONE;

	/*
	 * The live broadcast ended and dropped from results; the only
	 * remaining broadcast is scheduled far in the future, well
	 * outside its start window.  The ended broadcast must be
	 * STOPped, not RESTARTed into the future event.
	 */
	make_broadcast(&mock_broadcast, "bc-301", "scheduled", now + 600, now + 7200, false);

	scheduler_poll_once(&s);
	assert_int_equal(s.action, SCHED_ACTION_STOP);
	/* The future broadcast is now tracked for its eventual start. */
	assert_string_equal(s.acted_broadcast_id, "bc-301");
	assert_false(s.acted_started);

	destroy_test_scheduler(&s);
}

static void test_poll_broadcast_transition_in_start_window(void **state)
{
	(void)state;
	scheduler_t s;
	init_test_scheduler(&s);
	reset_mocks();

	time_t now = time(NULL);

	/* First broadcast: started, streaming. */
	make_broadcast(&mock_broadcast, "bc-310", "live", now - 3600, now + 3600, false);
	mock_fetch_result = SUBSPLASH_FETCH_OK;

	scheduler_poll_once(&s);
	assert_true(s.acted_started);
	s.action = SCHED_ACTION_NONE;

	/*
	 * Second broadcast starts soon -- within the start_lead
	 * window.  Before the fix this would overwrite RESTART with
	 * START in the same poll cycle.
	 */
	make_broadcast(&mock_broadcast, "bc-311", "scheduled", now + 30, now + 7200, false);

	scheduler_poll_once(&s);
	assert_int_equal(s.action, SCHED_ACTION_RESTART);

	/*
	 * Next poll (after the restart delay): RESTART was consumed,
	 * now the scheduler should signal START for the new broadcast.
	 */
	s.action = SCHED_ACTION_NONE;
	scheduler_poll_once(&s);
	assert_int_equal(s.action, SCHED_ACTION_START);
	assert_true(s.acted_started);

	destroy_test_scheduler(&s);
}

static void test_poll_simulated_live_skip(void **state)
{
	(void)state;
	scheduler_t s;
	init_test_scheduler(&s);
	reset_mocks();

	time_t now = time(NULL);
	make_broadcast(&mock_broadcast, "bc-400", "scheduled", now + 60, now + 3600, true);
	mock_fetch_result = SUBSPLASH_FETCH_OK;

	scheduler_poll_once(&s);
	assert_int_equal(s.action, SCHED_ACTION_NONE);
	/* acted_started is still set -- the scheduler tracks it. */
	assert_true(s.acted_started);
	/*
	 * A simulated-live broadcast is marked fully handled (acted_stopped)
	 * so a later broadcast transition cannot signal a spurious RESTART.
	 */
	assert_true(s.acted_stopped);

	destroy_test_scheduler(&s);
}

static void test_poll_simulated_then_transition_no_restart(void **state)
{
	(void)state;
	scheduler_t s;
	init_test_scheduler(&s);
	reset_mocks();

	time_t now = time(NULL);

	/*
	 * Simulated-live broadcast in its start window: tracked and marked
	 * handled, but OBS is never started.
	 */
	make_broadcast(&mock_broadcast, "bc-410", "live", now - 60, now + 3600, true);
	mock_fetch_result = SUBSPLASH_FETCH_OK;
	scheduler_poll_once(&s);
	assert_int_equal(s.action, SCHED_ACTION_NONE);
	assert_true(s.acted_started);
	assert_true(s.acted_stopped);

	/*
	 * The simulated broadcast ends and a different broadcast appears.
	 * Because nothing was ever streamed, the transition must NOT signal
	 * RESTART (which would start OBS). The new broadcast is far in the
	 * future, so no START fires either.
	 */
	make_broadcast(&mock_broadcast, "bc-411", "scheduled", now + 600, now + 7200, false);
	scheduler_poll_once(&s);
	assert_int_not_equal(s.action, SCHED_ACTION_RESTART);
	assert_int_equal(s.action, SCHED_ACTION_NONE);
	assert_string_equal(s.acted_broadcast_id, "bc-411");

	destroy_test_scheduler(&s);
}

static void test_poll_terminal_status_stop(void **state)
{
	(void)state;
	scheduler_t s;
	init_test_scheduler(&s);
	reset_mocks();

	time_t now = time(NULL);

	/* Simulate a broadcast that was started, then ended early. */
	make_broadcast(&mock_broadcast, "bc-500", "live", now - 3600, now + 3600, false);
	mock_fetch_result = SUBSPLASH_FETCH_OK;

	scheduler_poll_once(&s);
	assert_true(s.acted_started);
	s.action = SCHED_ACTION_NONE;

	/* Broadcast transitions to "ended". */
	make_broadcast(&mock_broadcast, "bc-500", "ended", now - 3600, now + 3600, false);

	scheduler_poll_once(&s);
	assert_int_equal(s.action, SCHED_ACTION_STOP);
	assert_true(s.acted_stopped);

	destroy_test_scheduler(&s);
}

static void test_poll_no_data_fallback_by_id(void **state)
{
	(void)state;
	scheduler_t s;
	init_test_scheduler(&s);
	reset_mocks();

	time_t now = time(NULL);

	/* First poll picks up a broadcast and starts. */
	make_broadcast(&mock_broadcast, "bc-600", "live", now - 3600, now + 3600, false);
	mock_fetch_result = SUBSPLASH_FETCH_OK;
	scheduler_poll_once(&s);
	assert_true(s.acted_started);
	s.action = SCHED_ACTION_NONE;

	/*
	 * Next poll returns no data, but the by-id fallback finds
	 * the broadcast in terminal state.
	 */
	mock_fetch_result = SUBSPLASH_FETCH_NO_DATA;
	memset(&mock_broadcast, 0, sizeof(mock_broadcast));

	make_broadcast(&mock_by_id_broadcast, "bc-600", "ended", now - 3600, now + 3600, false);
	mock_by_id_result = SUBSPLASH_FETCH_OK;

	scheduler_poll_once(&s);
	assert_int_equal(s.action, SCHED_ACTION_STOP);

	destroy_test_scheduler(&s);
}

static void test_poll_deleted_broadcast_stops(void **state)
{
	(void)state;
	scheduler_t s;
	init_test_scheduler(&s);
	reset_mocks();

	time_t now = time(NULL);

	/* First poll picks up a broadcast and starts. */
	make_broadcast(&mock_broadcast, "bc-700", "live", now - 3600, now + 3600, false);
	mock_fetch_result = SUBSPLASH_FETCH_OK;
	scheduler_poll_once(&s);
	assert_true(s.acted_started);
	s.action = SCHED_ACTION_NONE;

	/*
	 * Broadcast is deleted: it drops from list results and the
	 * by-id fallback returns NOT_FOUND (HTTP 404). The scheduler
	 * must STOP promptly rather than wait out the cached end time.
	 */
	mock_fetch_result = SUBSPLASH_FETCH_NO_DATA;
	memset(&mock_broadcast, 0, sizeof(mock_broadcast));
	mock_by_id_result = SUBSPLASH_FETCH_NOT_FOUND;

	scheduler_poll_once(&s);
	assert_int_equal(s.action, SCHED_ACTION_STOP);
	assert_true(s.acted_stopped);

	destroy_test_scheduler(&s);
}

static void test_poll_deleted_broadcast_not_started_no_stop(void **state)
{
	(void)state;
	scheduler_t s;
	init_test_scheduler(&s);
	reset_mocks();

	time_t now = time(NULL);

	/*
	 * Track a broadcast scheduled in the future, outside its start
	 * window, so it is tracked but never started.
	 */
	make_broadcast(&mock_broadcast, "bc-701", "scheduled", now + 600, now + 7200, false);
	mock_fetch_result = SUBSPLASH_FETCH_OK;
	scheduler_poll_once(&s);
	assert_string_equal(s.acted_broadcast_id, "bc-701");
	assert_false(s.acted_started);

	/*
	 * The future broadcast is deleted before it ever started. With
	 * nothing streaming, a 404 must not signal a spurious STOP.
	 */
	mock_fetch_result = SUBSPLASH_FETCH_NO_DATA;
	memset(&mock_broadcast, 0, sizeof(mock_broadcast));
	mock_by_id_result = SUBSPLASH_FETCH_NOT_FOUND;

	scheduler_poll_once(&s);
	assert_int_equal(s.action, SCHED_ACTION_NONE);
	assert_false(s.acted_stopped);

	destroy_test_scheduler(&s);
}

/* ================================================================== */
/* compute_poll_interval tests                                        */
/* ================================================================== */

static void test_poll_interval_no_cached_event(void **state)
{
	(void)state;
	scheduler_t s;
	init_test_scheduler(&s);

	int interval = compute_poll_interval(&s);
	assert_int_equal(interval, SCHED_POLL_IDLE_SEC);

	destroy_test_scheduler(&s);
}

static void test_poll_interval_event_far_away(void **state)
{
	(void)state;
	scheduler_t s;
	init_test_scheduler(&s);

	/* Event 2 hours from now => secs_to_event ~7080 => 7080/10 = 708,
	 * capped at 300. Jitter adds 0-25% on top: 300-375. */
	s.cached_start_epoch = time(NULL) + 7200;

	int interval = compute_poll_interval(&s);
	assert_true(interval >= SCHED_POLL_IDLE_SEC && interval <= SCHED_POLL_IDLE_SEC * 5 / 4);

	destroy_test_scheduler(&s);
}

static void test_poll_interval_event_proportional(void **state)
{
	(void)state;
	scheduler_t s;
	init_test_scheduler(&s);

	/* Event 10 minutes from now, lead 2 min => trigger in 8 min (480s).
	 * 480/10 = 48. Jitter adds 0-25%: 48-60. */
	s.cached_start_epoch = time(NULL) + 600;

	int interval = compute_poll_interval(&s);
	assert_true(interval >= 40 && interval <= 65);

	destroy_test_scheduler(&s);
}

static void test_poll_interval_event_imminent(void **state)
{
	(void)state;
	scheduler_t s;
	init_test_scheduler(&s);

	/* Event 2.5 minutes from now, lead 2 min => 30s to trigger.
	 * 30/10 = 3, clamped up to 30. Jitter could push it above 30,
	 * but the exact-clamp (secs_to_event=30) caps it back. */
	s.cached_start_epoch = time(NULL) + 150;

	int interval = compute_poll_interval(&s);
	assert_int_equal(interval, SCHED_POLL_MIN_SEC);

	destroy_test_scheduler(&s);
}

static void test_poll_interval_event_past_trigger(void **state)
{
	(void)state;
	scheduler_t s;
	init_test_scheduler(&s);

	/* Event already within start_lead (past the trigger point). */
	s.cached_start_epoch = time(NULL) + 60;

	int interval = compute_poll_interval(&s);
	assert_int_equal(interval, SCHED_POLL_MIN_SEC);

	destroy_test_scheduler(&s);
}

static void test_poll_interval_clamp_to_exact(void **state)
{
	(void)state;
	scheduler_t s;
	init_test_scheduler(&s);
	s.start_lead_minutes = 0;

	/* Event 45s away with no lead => secs_to_event = 45.
	 * 45/10 = 4 => clamped to 30. But 45 < 30 is false,
	 * so the exact-clamp doesn't fire. Interval = 30.
	 *
	 * Try 25s instead: 25/10 = 2 => clamped to 30.
	 * But secs_to_event (25) < interval (30), so exact-clamp
	 * fires: interval = 25. */
	s.cached_start_epoch = time(NULL) + 25;

	int interval = compute_poll_interval(&s);
	assert_true(interval >= 23 && interval <= 27);

	destroy_test_scheduler(&s);
}

static void test_poll_interval_pending_stop_after_list_drop(void **state)
{
	(void)state;
	scheduler_t s;
	init_test_scheduler(&s);

	/*
	 * Broadcast ended and dropped from the upcoming list, so
	 * cached_start_epoch was cleared to 0. A stop is still pending
	 * (started, not stopped) with stop lag = 2 min and the end 30s ago,
	 * so the stop trigger is ~90s out. Without the fix the interval would
	 * idle at 300s and the stop would fire ~5 min late; it must instead
	 * clamp to the ~90s remaining until end + lag.
	 */
	s.stop_lag_minutes = 2;
	s.acted_started = true;
	s.acted_stopped = false;
	s.cached_start_epoch = 0;
	s.cached_end_epoch = time(NULL) - 30;

	int interval = compute_poll_interval(&s);
	assert_true(interval >= 85 && interval <= 95);

	destroy_test_scheduler(&s);
}

static void test_poll_interval_pending_stop_clamps_start_interval(void **state)
{
	(void)state;
	scheduler_t s;
	init_test_scheduler(&s);

	/*
	 * Broadcast still listed and live: cached_start_epoch is set and the
	 * start trigger has passed, so the start-based interval would be the
	 * 30s minimum. The stop trigger is only 10s out, so the pending-stop
	 * clamp must pull the interval down to ~10s so the loop wakes exactly
	 * at end + lag.
	 */
	s.stop_lag_minutes = 0;
	s.acted_started = true;
	s.acted_stopped = false;
	s.cached_start_epoch = time(NULL) - 3600;
	s.cached_end_epoch = time(NULL) + 10;

	int interval = compute_poll_interval(&s);
	assert_true(interval >= 8 && interval <= 12);

	destroy_test_scheduler(&s);
}

static void test_poll_interval_pending_stop_past_due(void **state)
{
	(void)state;
	scheduler_t s;
	init_test_scheduler(&s);

	/*
	 * Stop trigger already passed (e.g. the machine was suspended) with
	 * the broadcast dropped from the list. The interval must not idle at
	 * 300s; it should poll promptly (<= 30s) to fire the pending stop.
	 */
	s.stop_lag_minutes = 2;
	s.acted_started = true;
	s.acted_stopped = false;
	s.cached_start_epoch = 0;
	s.cached_end_epoch = time(NULL) - 600;

	int interval = compute_poll_interval(&s);
	assert_int_equal(interval, SCHED_POLL_MIN_SEC);

	destroy_test_scheduler(&s);
}

static void test_poll_interval_not_started_ignores_end(void **state)
{
	(void)state;
	scheduler_t s;
	init_test_scheduler(&s);

	/*
	 * A cached end time with nothing streaming (not started) must not
	 * influence the interval -- only pending stops matter. With no start
	 * cached, the interval stays idle.
	 */
	s.stop_lag_minutes = 2;
	s.acted_started = false;
	s.acted_stopped = false;
	s.cached_start_epoch = 0;
	s.cached_end_epoch = time(NULL) + 10;

	int interval = compute_poll_interval(&s);
	assert_int_equal(interval, SCHED_POLL_IDLE_SEC);

	destroy_test_scheduler(&s);
}

static void test_poll_interval_already_stopped_ignores_end(void **state)
{
	(void)state;
	scheduler_t s;
	init_test_scheduler(&s);

	/*
	 * Once the stop has already fired, a cached end must not keep the
	 * loop polling tightly -- it returns to the idle interval.
	 */
	s.stop_lag_minutes = 2;
	s.acted_started = true;
	s.acted_stopped = true;
	s.cached_start_epoch = 0;
	s.cached_end_epoch = time(NULL) + 10;

	int interval = compute_poll_interval(&s);
	assert_int_equal(interval, SCHED_POLL_IDLE_SEC);

	destroy_test_scheduler(&s);
}

/* ================================================================== */
/* main                                                               */
/* ================================================================== */

int main(void)
{
	const struct CMUnitTest tests[] = {
		/* backoff */
		cmocka_unit_test(test_backoff_zero_failures),
		cmocka_unit_test(test_backoff_negative_failures),
		cmocka_unit_test(test_backoff_one_failure),
		cmocka_unit_test(test_backoff_two_failures),
		cmocka_unit_test(test_backoff_cap),
		/* cached stop */
		cmocka_unit_test(test_cached_stop_no_cache),
		cmocka_unit_test(test_cached_stop_before_trigger),
		cmocka_unit_test(test_cached_stop_at_trigger),
		cmocka_unit_test(test_cached_stop_already_stopped),
		/* poll */
		cmocka_unit_test(test_poll_api_error),
		cmocka_unit_test(test_poll_no_data),
		cmocka_unit_test(test_poll_start_in_window),
		cmocka_unit_test(test_poll_before_start_window),
		cmocka_unit_test(test_poll_stop_past_end),
		cmocka_unit_test(test_poll_transition_far_future_stops),
		cmocka_unit_test(test_poll_broadcast_transition_in_start_window),
		cmocka_unit_test(test_poll_simulated_live_skip),
		cmocka_unit_test(test_poll_simulated_then_transition_no_restart),
		cmocka_unit_test(test_poll_terminal_status_stop),
		cmocka_unit_test(test_poll_no_data_fallback_by_id),
		cmocka_unit_test(test_poll_deleted_broadcast_stops),
		cmocka_unit_test(test_poll_deleted_broadcast_not_started_no_stop),
		/* adaptive poll interval */
		cmocka_unit_test(test_poll_interval_no_cached_event),
		cmocka_unit_test(test_poll_interval_event_far_away),
		cmocka_unit_test(test_poll_interval_event_proportional),
		cmocka_unit_test(test_poll_interval_event_imminent),
		cmocka_unit_test(test_poll_interval_event_past_trigger),
		cmocka_unit_test(test_poll_interval_clamp_to_exact),
		cmocka_unit_test(test_poll_interval_pending_stop_after_list_drop),
		cmocka_unit_test(test_poll_interval_pending_stop_clamps_start_interval),
		cmocka_unit_test(test_poll_interval_pending_stop_past_due),
		cmocka_unit_test(test_poll_interval_not_started_ignores_end),
		cmocka_unit_test(test_poll_interval_already_stopped_ignores_end),
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}
