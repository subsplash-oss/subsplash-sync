#include "scheduler-panel.hpp"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QMessageBox>
#include <obs-module.h>
#include <obs-frontend-api.h>
#include "plugin-support.h"

static const char *SUBSPLASH_BASE_URL = "https://core.subsplash.com";

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
	connect(status_timer, &QTimer::timeout, this,
		&SchedulerPanel::OnStatusTick);

	LoadSettings();
	status_timer->start();
}

SchedulerPanel::~SchedulerPanel()
{
	status_timer->stop();
}

void SchedulerPanel::SetupUI()
{
	setMinimumWidth(350);

	auto *main_layout = new QVBoxLayout(this);

	/* ---- API Credentials ---- */
	auto *cred_group = new QGroupBox("Subsplash API Credentials", this);
	auto *cred_form = new QFormLayout;

	client_id_edit = new QLineEdit(this);
	client_secret_edit = new QLineEdit(this);
	client_secret_edit->setEchoMode(QLineEdit::Password);
	app_key_edit = new QLineEdit(this);
	app_key_edit->setMaxLength(6);

	cred_form->addRow("Client ID:", client_id_edit);
	cred_form->addRow("Client Secret:", client_secret_edit);
	cred_form->addRow("App Key:", app_key_edit);
	cred_group->setLayout(cred_form);
	main_layout->addWidget(cred_group);

	/* ---- Schedule Settings ---- */
	auto *sched_group = new QGroupBox("Schedule Settings", this);
	auto *sched_form = new QFormLayout;

	poll_interval_spin = new QSpinBox(this);
	poll_interval_spin->setRange(10, 300);
	poll_interval_spin->setValue(30);

	sched_form->addRow("Poll Interval (sec):", poll_interval_spin);
	sched_group->setLayout(sched_form);
	main_layout->addWidget(sched_group);

	/* ---- Buttons ---- */
	auto *btn_layout = new QHBoxLayout;

	test_btn = new QPushButton("Test Connection", this);
	save_btn = new QPushButton("Save", this);
	enable_btn = new QPushButton("Enable Scheduler", this);
	enable_btn->setCheckable(true);

	btn_layout->addWidget(test_btn);
	btn_layout->addWidget(save_btn);
	btn_layout->addWidget(enable_btn);
	main_layout->addLayout(btn_layout);

	/* ---- Status ---- */
	auto *status_group = new QGroupBox("Status", this);
	auto *status_form = new QFormLayout;

	conn_status_label = new QLabel("Unknown", this);
	sched_status_label = new QLabel("Stopped", this);
	next_bc_label = new QLabel("N/A", this);

	status_form->addRow("Connection:", conn_status_label);
	status_form->addRow("Scheduler:", sched_status_label);
	status_form->addRow("Next Broadcast:", next_bc_label);
	status_group->setLayout(status_form);
	main_layout->addWidget(status_group);

	main_layout->addStretch();

	setLayout(main_layout);

	connect(test_btn, &QPushButton::clicked, this,
		&SchedulerPanel::OnTestConnection);
	connect(save_btn, &QPushButton::clicked, this,
		&SchedulerPanel::OnSave);
	connect(enable_btn, &QPushButton::toggled, this,
		&SchedulerPanel::OnEnableToggled);
}

void SchedulerPanel::LoadSettings()
{
	const char *path = sched_get_config_path();
	obs_data_t *data = obs_data_create_from_json_file(path);
	if (!data)
		data = obs_data_create();

	obs_data_set_default_int(data, "poll_interval", 30);

	client_id_edit->setText(obs_data_get_string(data, "client_id"));
	client_secret_edit->setText(
		obs_data_get_string(data, "client_secret"));
	app_key_edit->setText(obs_data_get_string(data, "app_key"));
	poll_interval_spin->setValue(
		(int)obs_data_get_int(data, "poll_interval"));

	obs_data_release(data);

	enable_btn->setChecked(g_scheduler_enabled && g_scheduler.running);
	enable_btn->setText(enable_btn->isChecked() ? "Disable Scheduler"
						    : "Enable Scheduler");
}

void SchedulerPanel::SaveSettings()
{
	const char *path = sched_get_config_path();
	obs_data_t *data = obs_data_create();

	obs_data_set_string(data, "client_id",
			    client_id_edit->text().toUtf8().constData());
	obs_data_set_string(data, "client_secret",
			    client_secret_edit->text().toUtf8().constData());
	obs_data_set_string(data, "app_key",
			    app_key_edit->text().toUtf8().constData());
	obs_data_set_string(data, "base_url", SUBSPLASH_BASE_URL);
	obs_data_set_int(data, "poll_interval", poll_interval_spin->value());
	obs_data_set_int(data, "start_lead", 0);
	obs_data_set_int(data, "stop_lag", 0);
	obs_data_set_bool(data, "enabled", g_scheduler_enabled);

	obs_data_save_json_safe(data, path, "tmp", "bak");
	obs_data_release(data);

	obs_log(LOG_INFO, "Settings saved");
}

void SchedulerPanel::OnTestConnection()
{
	subsplash_client_t client;
	bool ok = subsplash_client_init(
		&client, SUBSPLASH_BASE_URL,
		client_id_edit->text().toUtf8().constData(),
		client_secret_edit->text().toUtf8().constData(),
		app_key_edit->text().toUtf8().constData());

	if (!ok) {
		conn_status_label->setText("Init failed");
		return;
	}

	char status_buf[256];
	bool success = subsplash_client_test_connection(&client, status_buf,
							sizeof(status_buf));
	conn_status_label->setText(status_buf);
	(void)success;

	subsplash_client_destroy(&client);
}

void SchedulerPanel::OnSave()
{
	SaveSettings();

	if (g_scheduler.running) {
		scheduler_configure(
			&g_scheduler, SUBSPLASH_BASE_URL,
			client_id_edit->text().toUtf8().constData(),
			client_secret_edit->text().toUtf8().constData(),
			app_key_edit->text().toUtf8().constData(),
			poll_interval_spin->value(), 0, 0);
	}

	conn_status_label->setText("Saved!");
}

void SchedulerPanel::OnEnableToggled()
{
	if (enable_btn->isChecked()) {
		SaveSettings();

		scheduler_configure(
			&g_scheduler, SUBSPLASH_BASE_URL,
			client_id_edit->text().toUtf8().constData(),
			client_secret_edit->text().toUtf8().constData(),
			app_key_edit->text().toUtf8().constData(),
			poll_interval_spin->value(), 0, 0);

		scheduler_start(&g_scheduler);
		g_scheduler_enabled = true;
		enable_btn->setText("Disable Scheduler");
		obs_log(LOG_INFO, "Scheduler enabled");
	} else {
		scheduler_stop(&g_scheduler);
		g_scheduler_enabled = false;
		enable_btn->setText("Enable Scheduler");
		obs_log(LOG_INFO, "Scheduler disabled");
	}
}

void SchedulerPanel::OnStatusTick()
{
	if (g_scheduler.running) {
		sched_status_label->setText("Running");

		char status_buf[256];
		char next_buf[256];
		scheduler_get_status(&g_scheduler, status_buf,
				     sizeof(status_buf), next_buf,
				     sizeof(next_buf));
		next_bc_label->setText(next_buf);

		if (obs_frontend_streaming_active())
			conn_status_label->setText("Streaming");
		else
			conn_status_label->setText("Authenticated");
	} else {
		sched_status_label->setText("Stopped");
		next_bc_label->setText("N/A");
	}
}
