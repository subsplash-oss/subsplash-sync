#pragma once

#include <QWidget>
#include <QCheckBox>
#include <QLineEdit>
#include <QSpinBox>
#include <QLabel>
#include <QPushButton>
#include <QToolButton>
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
	void OnFieldChanged();
	void OnEnableToggled();
	void OnRefresh();
	void OnStatusTick();

private:
	void SetupUI();
	QToolButton *MakeCollapsibleHeader(const char *title_key, QWidget *container);
	void SetSectionCollapsed(QToolButton *btn, QWidget *container, bool collapsed);
	void CollapseConfiguredSections();
	void ScheduleAutosave();
	void FlushAutosave();
	void FitDockToContents();
	void FlashAutosaved();
	bool CredsComplete() const;
	void StartConnectionCheck(bool collapse_on_success);

	QToolButton *cred_toggle_btn;
	QWidget *cred_container;
	QToolButton *sched_toggle_btn;
	QWidget *sched_container;
	QLineEdit *client_id_edit;
	QLineEdit *client_secret_edit;
	QLineEdit *app_key_edit;
	QSpinBox *start_lead_spin;
	QSpinBox *stop_lag_spin;
	QCheckBox *show_on_start_check;
	QPushButton *test_btn;
	QPushButton *enable_btn;
	QPushButton *refresh_btn;
	QLabel *autosave_label;
	QLabel *conn_status_label;
	QLabel *sched_status_label;
	QLabel *next_broadcast_label;
	QLabel *last_activity_label;
	QTimer *status_timer;
	QTimer *autosave_timer;
	bool initial_collapse_done = false;
	bool is_loading = false;
	/* Bumped on every async connection check so stale results are ignored. */
	unsigned conn_check_gen = 0;
};
