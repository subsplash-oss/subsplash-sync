#pragma once

#include <QWidget>
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
	QPushButton *test_btn;
	QPushButton *save_btn;
	QPushButton *enable_btn;
	QLabel *conn_status_label;
	QLabel *sched_status_label;
	QLabel *next_bc_label;
	QTimer *status_timer;
};
