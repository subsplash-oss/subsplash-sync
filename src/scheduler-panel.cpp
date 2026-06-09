#include "scheduler-panel.hpp"

#include "plugin-support.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
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

	/* ---- API Credentials ---- */
	auto *cred_group = new QGroupBox(T("Credentials.Title"), this);
	auto *cred_form = new QFormLayout;
	cred_form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

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
	auto *sched_form = new QFormLayout;
	sched_form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

	poll_interval_spin = new QSpinBox(this);
	poll_interval_spin->setRange(10, 300);
	poll_interval_spin->setValue(30);
	poll_interval_spin->setMinimumWidth(80);

	start_lead_spin = new QSpinBox(this);
	start_lead_spin->setRange(0, 2);
	start_lead_spin->setValue(SCHED_DEFAULT_START_LEAD_MINUTES);
	start_lead_spin->setMinimumWidth(80);

	stop_lag_spin = new QSpinBox(this);
	stop_lag_spin->setRange(0, 30);
	stop_lag_spin->setValue(SCHED_DEFAULT_STOP_LAG_MINUTES);
	stop_lag_spin->setMinimumWidth(80);

	sched_form->addRow(T("Schedule.PollInterval"), poll_interval_spin);
	sched_form->addRow(T("Schedule.StartLead"), start_lead_spin);
	sched_form->addRow(T("Schedule.StopLag"), stop_lag_spin);
	sched_group->setLayout(sched_form);
	main_layout->addWidget(sched_group);

	/* ---- Buttons ---- */
	auto *btn_layout = new QHBoxLayout;

	test_btn = new QPushButton(T("Buttons.TestConnection"), this);
	save_btn = new QPushButton(T("Buttons.Save"), this);

	btn_layout->addWidget(test_btn);
	btn_layout->addWidget(save_btn);
	main_layout->addLayout(btn_layout);

	enable_btn = new QPushButton(T("Buttons.Enable"), this);
	enable_btn->setCheckable(true);
	main_layout->addWidget(enable_btn);

	/* ---- Status ---- */
	auto *status_group = new QGroupBox(T("Status.Title"), this);
	auto *status_form = new QFormLayout;

	conn_status_label = new QLabel(T("Status.Unknown"), this);
	sched_status_label = new QLabel(T("Status.Stopped"), this);
	next_broadcast_label = new QLabel(T("Status.NA"), this);
	next_broadcast_label->setWordWrap(true);
	next_broadcast_label->setMinimumHeight(next_broadcast_label->fontMetrics().lineSpacing() * 2);
	last_activity_label = new QLabel("", this);
	last_activity_label->setWordWrap(true);
	last_activity_label->setMinimumHeight(last_activity_label->fontMetrics().lineSpacing() * 2);

	status_form->addRow(T("Status.Connection"), conn_status_label);
	status_form->addRow(T("Status.Scheduler"), sched_status_label);
	status_form->addRow(T("Status.NextBroadcast"), next_broadcast_label);
	status_form->addRow(T("Status.LastActivity"), last_activity_label);
	status_group->setLayout(status_form);
	main_layout->addWidget(status_group);

	main_layout->addStretch();

	setLayout(main_layout);

	connect(test_btn, &QPushButton::clicked, this, &SchedulerPanel::OnTestConnection);
	connect(save_btn, &QPushButton::clicked, this, &SchedulerPanel::OnSave);
	connect(enable_btn, &QPushButton::toggled, this, &SchedulerPanel::OnEnableToggled);
}

void SchedulerPanel::LoadSettings()
{
	const char *path = sched_get_config_path();
	obs_data_t *data = obs_data_create_from_json_file(path);
	if (!data)
		data = obs_data_create();

	obs_data_set_default_int(data, "poll_interval", 30);
	obs_data_set_default_int(data, "start_lead", SCHED_DEFAULT_START_LEAD_MINUTES);
	obs_data_set_default_int(data, "stop_lag", SCHED_DEFAULT_STOP_LAG_MINUTES);

	client_id_edit->setText(obs_data_get_string(data, "client_id"));
	client_secret_edit->setText(obs_data_get_string(data, "client_secret"));
	app_key_edit->setText(obs_data_get_string(data, "app_key"));
	poll_interval_spin->setValue((int)obs_data_get_int(data, "poll_interval"));
	start_lead_spin->setValue((int)obs_data_get_int(data, "start_lead"));
	stop_lag_spin->setValue((int)obs_data_get_int(data, "stop_lag"));

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
	obs_data_set_int(data, "poll_interval", poll_interval_spin->value());
	obs_data_set_int(data, "start_lead", start_lead_spin->value());
	obs_data_set_int(data, "stop_lag", stop_lag_spin->value());
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
				    app_key_edit->text().toUtf8().constData(), poll_interval_spin->value(),
				    start_lead_spin->value(), stop_lag_spin->value());
	}

	conn_status_label->setText(T("Status.Saved"));
}

void SchedulerPanel::OnEnableToggled()
{
	if (enable_btn->isChecked()) {
		SaveSettings();

		scheduler_configure(&g_scheduler, DEFAULT_BASE_URL, client_id_edit->text().toUtf8().constData(),
				    client_secret_edit->text().toUtf8().constData(),
				    app_key_edit->text().toUtf8().constData(), poll_interval_spin->value(),
				    start_lead_spin->value(), stop_lag_spin->value());

		scheduler_start(&g_scheduler);
		g_scheduler_enabled = true;
		enable_btn->setText(T("Buttons.Disable"));
		obs_log(LOG_INFO, "Scheduler enabled");
	} else {
		scheduler_stop(&g_scheduler);
		g_scheduler_enabled = false;
		enable_btn->setText(T("Buttons.Enable"));
		obs_log(LOG_INFO, "Scheduler disabled");
	}
}

void SchedulerPanel::OnStatusTick()
{
	if (g_scheduler.running) {
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
