/*
Subsplash Live Stream Scheduler
Copyright (C) 2026 Subsplash <api@subsplash.com>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/platform.h>
#include <QDockWidget>
#include <QTimer>
#include <QMainWindow>
#include "plugin-support.h"
#include "scheduler-panel.hpp"

extern "C" {
#include "scheduler.h"
}

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-subsplash-scheduler", "en-US")

/* ------------------------------------------------------------------ */
/* Global state                                                       */
/* ------------------------------------------------------------------ */

scheduler_t g_scheduler;
bool g_scheduler_enabled = false;

static char g_config_path[512];
static QTimer *g_action_timer = nullptr;

/*
 * Restart state machine: after a RESTART action we stop the stream,
 * wait for OBS_FRONTEND_EVENT_STREAMING_STOPPED (full cleanup) plus
 * a safety delay, then start again.  Polling
 * obs_frontend_streaming_active() alone isn't safe -- OBS flips that
 * flag before the RTMP connect thread has exited, and starting a new
 * stream while the old thread is still tearing down causes a crash
 * inside obs-outputs (null-pointer memmove in the connect thread).
 */
static bool g_restart_pending = false;
static bool g_restart_stream_fully_stopped = false;
static time_t g_restart_stop_time = 0;

/* Exposed to scheduler-panel.cpp via extern "C" */
extern "C" const char *sched_get_config_path(void)
{
	return g_config_path;
}

/* ------------------------------------------------------------------ */
/* Persistent action-drain timer (runs even when dialog is closed)    */
/* ------------------------------------------------------------------ */

static void on_action_tick()
{
	if (!g_scheduler.running)
		return;

	if (g_restart_pending) {
		if (!g_restart_stream_fully_stopped)
			return;

		bool delay_elapsed = (time(NULL) - g_restart_stop_time) >= SCHED_RESTART_DELAY_SEC;

		if (delay_elapsed) {
			g_restart_pending = false;
			g_restart_stream_fully_stopped = false;
			scheduler_consume_action(&g_scheduler);
			obs_frontend_streaming_start();
			obs_log(LOG_INFO, "Auto-started streaming after restart delay");
		}
		return;
	}

	long action = scheduler_consume_action(&g_scheduler);
	if (action == SCHED_ACTION_START) {
		obs_frontend_streaming_start();
		obs_log(LOG_INFO, "Auto-started streaming from schedule");
	} else if (action == SCHED_ACTION_STOP) {
		obs_frontend_streaming_stop();
		obs_log(LOG_INFO, "Auto-stopped streaming from schedule");
	} else if (action == SCHED_ACTION_RESTART) {
		obs_frontend_streaming_stop();
		g_restart_pending = true;
		g_restart_stream_fully_stopped = false;
		g_restart_stop_time = time(NULL);
		obs_log(LOG_INFO,
			"Auto-stopped streaming for broadcast transition, "
			"will restart in %ds after cleanup completes",
			SCHED_RESTART_DELAY_SEC);
	}
}

/* ------------------------------------------------------------------ */
/* Frontend event: register dock and auto-start scheduler on load     */
/* ------------------------------------------------------------------ */

static void on_frontend_event(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_STREAMING_STOPPED && g_restart_pending) {
		g_restart_stream_fully_stopped = true;
		g_restart_stop_time = time(NULL);
		obs_log(LOG_INFO, "Stream fully stopped, restart delay begins now");
	}

	if (event != OBS_FRONTEND_EVENT_FINISHED_LOADING)
		return;

	/* Register the dockable panel */
	QMainWindow *main_window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	auto *panel = new SchedulerPanel(main_window);
	obs_frontend_add_dock_by_id("subsplash-scheduler", "Subsplash Live Scheduler", panel);

	auto *dock = main_window->findChild<QDockWidget *>("subsplash-scheduler");
	if (dock)
		dock->setVisible(true);

	/* Auto-start scheduler if previously enabled */
	obs_data_t *cfg = obs_data_create_from_json_file(g_config_path);
	if (!cfg)
		return;

	bool show_on_start = obs_data_get_bool(cfg, "show_on_start");
	if (show_on_start) {
		auto *dock = main_window->findChild<QDockWidget *>("subsplash-scheduler");
		if (dock)
			dock->setVisible(true);
	}

	bool enabled = obs_data_get_bool(cfg, "enabled");
	if (!enabled) {
		obs_data_release(cfg);
		return;
	}

	const char *client_id = obs_data_get_string(cfg, "client_id");
	const char *client_secret = obs_data_get_string(cfg, "client_secret");
	const char *app_key = obs_data_get_string(cfg, "app_key");
	const char *base_url = obs_data_get_string(cfg, "base_url");
	int poll = (int)obs_data_get_int(cfg, "poll_interval");
	int start_lead = (int)obs_data_get_int(cfg, "start_lead");
	int stop_lag = (int)obs_data_get_int(cfg, "stop_lag");

	if (!base_url || base_url[0] == '\0')
		base_url = "https://core.subsplash.com";

	if (client_id[0] != '\0' && client_secret[0] != '\0' && app_key[0] != '\0') {
		scheduler_configure(&g_scheduler, base_url, client_id, client_secret, app_key, poll > 0 ? poll : 30,
				    start_lead > 0 ? start_lead : SCHED_DEFAULT_START_LEAD_MINUTES,
				    stop_lag > 0 ? stop_lag : SCHED_DEFAULT_STOP_LAG_MINUTES);
		scheduler_start(&g_scheduler);
		g_scheduler_enabled = true;
		obs_log(LOG_INFO, "Auto-started scheduler from saved config");
	}

	obs_data_release(cfg);
}

/* ------------------------------------------------------------------ */
/* Module lifecycle                                                   */
/* ------------------------------------------------------------------ */

bool obs_module_load(void)
{
	obs_log(LOG_INFO, "plugin loaded (version %s)", PLUGIN_VERSION);

	scheduler_init(&g_scheduler);

	/* Build the config directory and file paths */
	char *config_dir = obs_module_config_path("");
	if (config_dir) {
		os_mkdirs(config_dir);
		bfree(config_dir);
	}

	char *module_config = obs_module_config_path("config.json");
	if (module_config) {
		snprintf(g_config_path, sizeof(g_config_path), "%s", module_config);
		bfree(module_config);
	}

	obs_frontend_add_event_callback(on_frontend_event, NULL);

	/* Create a 1-second timer on the main thread to drain actions.
	   This ensures obs_frontend_streaming_start/stop are always called
	   from the Qt main thread, even when the settings dialog is closed. */
	g_action_timer = new QTimer();
	g_action_timer->setInterval(1000);
	QObject::connect(g_action_timer, &QTimer::timeout, on_action_tick);
	g_action_timer->start();

	return true;
}

void obs_module_unload(void)
{
	if (g_action_timer) {
		g_action_timer->stop();
		delete g_action_timer;
		g_action_timer = nullptr;
	}

	obs_frontend_remove_dock("subsplash-scheduler");
	obs_frontend_remove_event_callback(on_frontend_event, NULL);
	scheduler_destroy(&g_scheduler);

	obs_log(LOG_INFO, "plugin unloaded");
}
