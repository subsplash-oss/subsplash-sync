#include "scheduler-panel.hpp"

#include "plugin-support.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QMessageBox>
#include <obs-module.h>
#include <obs-frontend-api.h>

#define T(key) obs_module_text(key)

static const char *DEFAULT_BASE_URL = "https://core.subsplash.com";

extern "C" {
const char *sched_get_config_path(void);

#include "scheduler.h"

extern scheduler_t g_scheduler;
extern bool g_scheduler_enabled;
}

SchedulerPanel::SchedulerPanel(QWidget *parent) : QWidget(parent)
{
	SetupUI();

	status_timer = new QTimer(this);
	status_timer->setInterval(1000);
	connect(status_timer, &QTimer::timeout, this, &SchedulerPanel::OnStatusTick);

	LoadSettings();
	status_timer->start();
}

SchedulerPanel::~SchedulerPanel()
{
	status_timer->stop();
}

void SchedulerPanel::SetupUI()
{
	setMinimumWidth(450);

	auto *main_layout = new QVBoxLayout(this);
	main_layout->setContentsMargins(6, 6, 6, 6);
	main_layout->setSpacing(4);

	/* ---- API Credentials ---- */
	auto *cred_group = new QGroupBox(T("Credentials.Title"), this);
	auto *cred_form = new QFormLayout;
	cred_form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
	cred_form->setContentsMargins(6, 0, 6, 4);
	cred_form->setVerticalSpacing(4);

	client_id_edit = new QLineEdit(this);
	client_secret_edit = new QLineEdit(this);
	client_secret_edit->setEchoMode(QLineEdit::Password);
	app_key_edit = new QLineEdit(this);
	app_key_edit->setMaxLength(6);

	cred_form->addRow(T("Credentials.ClientId"), client_id_edit);
	cred_form->addRow(T("Credentials.ClientSecret"), client_secret_edit);
	cred_form->addRow(T("Credentials.AppKey"), app_key_edit);
	cred_group->setLayout(cred_form);
	main_layout->addWidget(cred_group);

	/* ---- Schedule Settings ---- */
	auto *sched_group = new QGroupBox(T("Schedule.Title"), this);
	auto *sched_grid = new QGridLayout;
	sched_grid->setContentsMargins(6, 0, 6, 4);
	sched_grid->setVerticalSpacing(4);

	start_lead_spin = new QSpinBox(this);
	start_lead_spin->setRange(0, 2);
	start_lead_spin->setValue(SCHED_DEFAULT_START_LEAD_MINUTES);
	start_lead_spin->setMinimumWidth(80);

	stop_lag_spin = new QSpinBox(this);
	stop_lag_spin->setRange(0, 10);
	stop_lag_spin->setValue(SCHED_DEFAULT_STOP_LAG_MINUTES);
	stop_lag_spin->setMinimumWidth(80);

	show_on_start_check = new QCheckBox(T("Schedule.ShowOnStart"), this);

	sched_grid->addWidget(new QLabel(T("Schedule.StartLead"), this), 0, 0);
	sched_grid->addWidget(start_lead_spin, 0, 1);

	sched_grid->addWidget(new QLabel(T("Schedule.StopLag"), this), 1, 0);
	sched_grid->addWidget(stop_lag_spin, 1, 1);
	sched_grid->addWidget(show_on_start_check, 1, 2, 1, 2);

	sched_group->setLayout(sched_grid);
	main_layout->addWidget(sched_group);

	/* ---- Buttons ---- */
	auto *btn_layout = new QHBoxLayout;

	test_btn = new QPushButton(T("Buttons.TestConnection"), this);
	save_btn = new QPushButton(T("Buttons.Save"), this);
	refresh_btn = new QPushButton(T("Buttons.Refresh"), this);
	refresh_btn->setEnabled(false);

	btn_layout->addWidget(test_btn);
	btn_layout->addWidget(save_btn);
	btn_layout->addWidget(refresh_btn);
	main_layout->addLayout(btn_layout);

	enable_btn = new QPushButton(T("Buttons.Enable"), this);
	enable_btn->setCheckable(true);
	main_layout->addWidget(enable_btn);

	/* ---- Status ---- */
	auto *status_group = new QGroupBox(T("Status.Title"), this);
	auto *status_grid = new QGridLayout;
	status_grid->setContentsMargins(6, 0, 6, 4);
	status_grid->setVerticalSpacing(2);
	status_grid->setColumnStretch(1, 1);

	conn_status_label = new QLabel(T("Status.Unknown"), this);
	sched_status_label = new QLabel(T("Status.Stopped"), this);
	next_broadcast_label = new QLabel(T("Status.NA"), this);
	next_broadcast_label->setWordWrap(true);
	next_broadcast_label->setMinimumHeight(next_broadcast_label->fontMetrics().lineSpacing() * 2);
	last_activity_label = new QLabel("", this);
	last_activity_label->setWordWrap(true);
	last_activity_label->setMinimumHeight(last_activity_label->fontMetrics().lineSpacing() * 2);

	status_grid->addWidget(new QLabel(T("Status.Connection"), this), 0, 0, Qt::AlignRight);
	status_grid->addWidget(conn_status_label, 0, 1);
	status_grid->addWidget(new QLabel(T("Status.Scheduler"), this), 1, 0, Qt::AlignRight);
	status_grid->addWidget(sched_status_label, 1, 1);
	status_grid->addWidget(new QLabel(T("Status.NextBroadcast"), this), 2, 0, Qt::AlignRight | Qt::AlignTop);
	next_broadcast_label->setAlignment(Qt::AlignTop | Qt::AlignLeft);
	status_grid->addWidget(next_broadcast_label, 2, 1, Qt::AlignTop);
	status_grid->addWidget(new QLabel(T("Status.LastActivity"), this), 3, 0, Qt::AlignRight);
	status_grid->addWidget(last_activity_label, 3, 1);
	status_group->setLayout(status_grid);
	main_layout->addWidget(status_group);

	main_layout->addStretch();

	setLayout(main_layout);

	connect(test_btn, &QPushButton::clicked, this, &SchedulerPanel::OnTestConnection);
	connect(save_btn, &QPushButton::clicked, this, &SchedulerPanel::OnSave);
	connect(refresh_btn, &QPushButton::clicked, this, &SchedulerPanel::OnRefresh);
	connect(enable_btn, &QPushButton::toggled, this, &SchedulerPanel::OnEnableToggled);
}

void SchedulerPanel::LoadSettings()
{
	const char *path = sched_get_config_path();
	obs_data_t *data = obs_data_create_from_json_file(path);
	if (!data)
		data = obs_data_create();

	obs_data_set_default_int(data, "start_lead", SCHED_DEFAULT_START_LEAD_MINUTES);
	obs_data_set_default_int(data, "stop_lag", SCHED_DEFAULT_STOP_LAG_MINUTES);

	client_id_edit->setText(obs_data_get_string(data, "client_id"));
	client_secret_edit->setText(obs_data_get_string(data, "client_secret"));
	app_key_edit->setText(obs_data_get_string(data, "app_key"));
	start_lead_spin->setValue((int)obs_data_get_int(data, "start_lead"));
	stop_lag_spin->setValue((int)obs_data_get_int(data, "stop_lag"));
	show_on_start_check->setChecked(obs_data_get_bool(data, "show_on_start"));

	obs_data_release(data);

	enable_btn->setChecked(g_scheduler_enabled && g_scheduler.running);
	enable_btn->setText(enable_btn->isChecked() ? T("Buttons.Disable") : T("Buttons.Enable"));
}

void SchedulerPanel::SaveSettings()
{
	const char *path = sched_get_config_path();
	obs_data_t *data = obs_data_create();

	obs_data_set_string(data, "client_id", client_id_edit->text().toUtf8().constData());
	obs_data_set_string(data, "client_secret", client_secret_edit->text().toUtf8().constData());
	obs_data_set_string(data, "app_key", app_key_edit->text().toUtf8().constData());
	obs_data_set_string(data, "base_url", DEFAULT_BASE_URL);
	obs_data_set_int(data, "start_lead", start_lead_spin->value());
	obs_data_set_int(data, "stop_lag", stop_lag_spin->value());
	obs_data_set_bool(data, "show_on_start", show_on_start_check->isChecked());
	obs_data_set_bool(data, "enabled", g_scheduler_enabled);

	obs_data_save_json_safe(data, path, "tmp", "bak");
	obs_data_release(data);

	obs_log(LOG_INFO, "Settings saved");
}

void SchedulerPanel::OnTestConnection()
{
	subsplash_client_t client;
	bool ok = subsplash_client_init(&client, DEFAULT_BASE_URL, client_id_edit->text().toUtf8().constData(),
					client_secret_edit->text().toUtf8().constData(),
					app_key_edit->text().toUtf8().constData());

	if (!ok) {
		conn_status_label->setText(T("Status.InitFailed"));
		return;
	}

	char status_buf[256];
	bool success = subsplash_client_test_connection(&client, status_buf, sizeof(status_buf));
	conn_status_label->setText(status_buf);
	(void)success;

	subsplash_client_destroy(&client);
}

void SchedulerPanel::OnSave()
{
	SaveSettings();

	if (g_scheduler.running) {
		scheduler_configure(&g_scheduler, DEFAULT_BASE_URL, client_id_edit->text().toUtf8().constData(),
				    client_secret_edit->text().toUtf8().constData(),
				    app_key_edit->text().toUtf8().constData(), start_lead_spin->value(),
				    stop_lag_spin->value());
	}

	save_btn->setText(T("Status.Saved"));
	QTimer::singleShot(2000, this, [this]() { save_btn->setText(T("Buttons.Save")); });
}

void SchedulerPanel::OnRefresh()
{
	scheduler_wake(&g_scheduler);
	refresh_btn->setText(T("Status.Refreshing"));
	QTimer::singleShot(2000, this, [this]() { refresh_btn->setText(T("Buttons.Refresh")); });
}

void SchedulerPanel::OnEnableToggled()
{
	if (enable_btn->isChecked()) {
		g_scheduler_enabled = true;

		scheduler_configure(&g_scheduler, DEFAULT_BASE_URL, client_id_edit->text().toUtf8().constData(),
				    client_secret_edit->text().toUtf8().constData(),
				    app_key_edit->text().toUtf8().constData(), start_lead_spin->value(),
				    stop_lag_spin->value());

		scheduler_start(&g_scheduler);
		SaveSettings();
		enable_btn->setText(T("Buttons.Disable"));
		refresh_btn->setEnabled(true);
		obs_log(LOG_INFO, "Scheduler enabled");
	} else {
		scheduler_stop(&g_scheduler);
		g_scheduler_enabled = false;
		SaveSettings();
		enable_btn->setText(T("Buttons.Enable"));
		refresh_btn->setEnabled(false);
		obs_log(LOG_INFO, "Scheduler disabled");
	}
}

void SchedulerPanel::OnStatusTick()
{
	bool running = g_scheduler.running;

	if (enable_btn->isChecked() != running) {
		enable_btn->blockSignals(true);
		enable_btn->setChecked(running);
		enable_btn->setText(running ? T("Buttons.Disable") : T("Buttons.Enable"));
		enable_btn->blockSignals(false);
		refresh_btn->setEnabled(running);
	}

	if (running) {
		sched_status_label->setText(T("Status.Running"));

		char status_buf[256];
		char next_buf[256];
		char activity_buf[256];
		scheduler_get_status(&g_scheduler, status_buf, sizeof(status_buf), next_buf, sizeof(next_buf),
				     activity_buf, sizeof(activity_buf));
		next_broadcast_label->setText(next_buf);
		last_activity_label->setText(activity_buf);

		if (obs_frontend_streaming_active()) {
			conn_status_label->setText(T("Status.Streaming"));
		} else {
			conn_status_label->setText(T("Status.Authenticated"));
		}
	} else {
		sched_status_label->setText(T("Status.Stopped"));
		next_broadcast_label->setText(T("Status.NA"));
		last_activity_label->setText("");
	}
}
