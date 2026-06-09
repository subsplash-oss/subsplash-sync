#pragma once

#include <QWidget>
#include <QCheckBox>
#include <QLineEdit>
#include <QSpinBox>
#include <QLabel>
#include <QPushButton>
#include <QTimer>

class SchedulerPanel : public QWidget {
	Q_OBJECT

public:
	explicit SchedulerPanel(QWidget *parent = nullptr);
	~SchedulerPanel() override;
	SchedulerPanel(const SchedulerPanel &) = delete;
	SchedulerPanel &operator=(const SchedulerPanel &) = delete;
	SchedulerPanel(SchedulerPanel &&) = delete;
	SchedulerPanel &operator=(SchedulerPanel &&) = delete;

	void LoadSettings();
	void SaveSettings();

private slots:
	void OnTestConnection();
	void OnSave();
	void OnEnableToggled();
	void OnStatusTick();

private:
	void SetupUI();

	QLineEdit *client_id_edit;
	QLineEdit *client_secret_edit;
	QLineEdit *app_key_edit;
	QSpinBox *poll_interval_spin;
	QSpinBox *start_lead_spin;
	QSpinBox *stop_lag_spin;
	QCheckBox *show_on_start_check;
	QPushButton *test_btn;
	QPushButton *save_btn;
	QPushButton *enable_btn;
	QLabel *conn_status_label;
	QLabel *sched_status_label;
	QLabel *next_broadcast_label;
	QLabel *last_activity_label;
	QTimer *status_timer;
};
