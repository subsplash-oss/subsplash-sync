/*
Subsplash Sync
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
#include <QByteArray>
#include <QString>
#include "credential-store.hpp"
#include "plugin-support.h"
#include "scheduler-panel.hpp"

extern "C" {
#include "scheduler.h"
}

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-subsplash-sync", "en-US")

/* ------------------------------------------------------------------ */
/* Global state                                                       */
/* ------------------------------------------------------------------ */

extern "C" {
scheduler_t g_scheduler;
bool g_scheduler_enabled = false;
}

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

/* Read one credential from the OS keychain, logging (but not failing) on a hard
 * error so each key is reported independently. Returns an empty string when the
 * entry is absent or unreadable. */
static QString read_credential(const QString &key)
{
	QString value, err;
	cred_store::Read(key, value, &err);
	if (!err.isEmpty())
		obs_log(LOG_WARNING, "Keychain read for '%s' failed: %s", key.toUtf8().constData(),
			err.toUtf8().constData());
	return value;
}

/* ------------------------------------------------------------------ */
/* Self-managed dock layout persistence                               */
/*                                                                    */
/* OBS registers plugin docks (obs_frontend_add_dock_by_id) after the */
/* main window's restoreState() has already run, and leaves them      */
/* floating + hidden. QMainWindow::restoreDockWidget() does not        */
/* reliably re-dock a panel added this late -- it returns true yet     */
/* leaves the dock floating, especially when it was part of a tab      */
/* group or split. So we snapshot the whole layout ourselves and       */
/* re-apply it on load instead of trusting OBS to restore our dock.    */
/* ------------------------------------------------------------------ */

/*
 * Snapshot the entire main-window dock layout. This is the same data OBS
 * persists itself, so reapplying it on load is idempotent for the built-in
 * docks while reproducing our panel's exact position -- split, tab grouping
 * and size -- which a piecemeal area/tab restore cannot.
 */
static void save_dock_layout(QMainWindow *main_window)
{
	obs_data_t *d = obs_data_create();
	QByteArray state = main_window->saveState().toBase64();
	obs_data_set_string(d, "main_state", state.constData());

	char *path = obs_module_config_path("dock.json");
	if (path) {
		obs_data_save_json_safe(d, path, "tmp", "bak");
		bfree(path);
	}
	obs_data_release(d);
}

static bool restore_dock_layout(QMainWindow *main_window)
{
	char *path = obs_module_config_path("dock.json");
	if (!path)
		return false;
	obs_data_t *d = obs_data_create_from_json_file(path);
	bfree(path);
	if (!d)
		return false; /* first run -- nothing saved yet, leave OBS defaults */

	const char *state = obs_data_get_string(d, "main_state");
	bool restored = false;
	if (state && *state)
		restored = main_window->restoreState(QByteArray::fromBase64(QByteArray(state)));

	obs_data_release(d);
	return restored;
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

	/* Persist the final dock layout on shutdown. The frontend API and main
	 * window are still valid during the EXIT event. */
	if (event == OBS_FRONTEND_EVENT_EXIT) {
		auto *mw = static_cast<QMainWindow *>(obs_frontend_get_main_window());
		if (mw && mw->findChild<QDockWidget *>("subsplash-sync"))
			save_dock_layout(mw);
		return;
	}

	if (event != OBS_FRONTEND_EVENT_FINISHED_LOADING)
		return;

	/* Register the dockable panel. */
	QMainWindow *main_window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	auto *panel = new SchedulerPanel(main_window);
	obs_frontend_add_dock_by_id("subsplash-sync", "Subsplash Sync", panel);

	/* OBS leaves the dock floating + hidden and does not reliably re-dock a
	 * panel registered this late, so re-apply our own saved layout. The
	 * snapshot is taken only on EXIT (handled above), which keeps dock.json in
	 * lockstep with OBS's own layout save and avoids overriding sibling docks
	 * with a mid-session snapshot. */
	auto *dock = main_window->findChild<QDockWidget *>("subsplash-sync");
	if (dock) {
		bool restored = restore_dock_layout(main_window);
		/* First run (no saved layout yet): surface the dock once so it is
		 * discoverable. After that, the restored state carries the dock's
		 * own visibility -- left open it reopens, closed it stays closed --
		 * matching native OBS dock behavior. */
		if (!restored)
			dock->setVisible(true);
	}

	/* Auto-start scheduler if previously enabled */
	obs_data_t *cfg = obs_data_create_from_json_file(g_config_path);
	if (!cfg)
		return;

	bool enabled = obs_data_get_bool(cfg, "enabled");
	if (!enabled) {
		obs_data_release(cfg);
		return;
	}

	obs_data_set_default_int(cfg, "start_lead", SCHED_DEFAULT_START_LEAD_MINUTES);
	obs_data_set_default_int(cfg, "stop_lag", SCHED_DEFAULT_STOP_LAG_MINUTES);

	const char *base_url = obs_data_get_string(cfg, "base_url");
	int start_lead = (int)obs_data_get_int(cfg, "start_lead");
	int stop_lag = (int)obs_data_get_int(cfg, "stop_lag");

	if (!base_url || base_url[0] == '\0')
		base_url = "https://core.subsplash.com";

	/* Credentials live in the OS keychain, not config.json. If the keychain is
	 * unavailable we log (per key) and skip auto-start rather than crash. */
	QString client_id = read_credential(cred_store::kClientId);
	QString client_secret = read_credential(cred_store::kClientSecret);
	QString app_key = read_credential(cred_store::kAppKey);

	if (!client_id.isEmpty() && !client_secret.isEmpty() && !app_key.isEmpty()) {
		scheduler_configure(&g_scheduler, base_url, client_id.toUtf8().constData(),
				    client_secret.toUtf8().constData(), app_key.toUtf8().constData(), start_lead,
				    stop_lag);
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

	obs_frontend_remove_dock("subsplash-sync");
	obs_frontend_remove_event_callback(on_frontend_event, NULL);
	scheduler_destroy(&g_scheduler);

	obs_log(LOG_INFO, "plugin unloaded");
}
