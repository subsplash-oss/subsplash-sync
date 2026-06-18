#include "scheduler-panel.hpp"

#include "compat-atomics.h"
#include "plugin-support.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QMessageBox>
#include <QDockWidget>
#include <QPointer>
#include <QCoreApplication>
#include <obs-module.h>
#include <obs-frontend-api.h>

#include <string>
#include <thread>
#include <ctime>

#define T(key) obs_module_text(key)

static const char *DEFAULT_BASE_URL = "https://core.subsplash.com";

/* Format the wait until the next automatic refresh as a short countdown,
 * e.g. "in 2m 34s". Units are kept in code (not localized) to match the
 * existing C-side time formatting for the Next Broadcast row. */
static QString FormatNextRefresh(time_t epoch)
{
	if (epoch <= 0)
		return QStringLiteral("soon");

	long secs = (long)(epoch - time(NULL));
	if (secs <= 0)
		return QStringLiteral("now");
	if (secs < 60)
		return QStringLiteral("in %1s").arg(secs);
	if (secs < 3600)
		return QStringLiteral("in %1m %2s").arg(secs / 60).arg(secs % 60, 2, 10, QLatin1Char('0'));
	return QStringLiteral("in %1h %2m").arg(secs / 3600).arg((secs % 3600) / 60, 2, 10, QLatin1Char('0'));
}

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

	/* Debounced auto-save: a field edit (re)starts this timer, so we save
	 * shortly after the user stops typing instead of relying solely on
	 * focus-out (which a NoFocus fold header can't trigger). */
	autosave_timer = new QTimer(this);
	autosave_timer->setSingleShot(true);
	/* Idle window before an autosave fires. Long enough that deliberately
	 * typing an unfamiliar credential char-by-char doesn't trigger a save
	 * (and a connection probe) mid-entry; blur/Enter still flushes
	 * immediately via editingFinished, so nothing is lost. */
	autosave_timer->setInterval(1500);
	connect(autosave_timer, &QTimer::timeout, this, &SchedulerPanel::OnFieldChanged);

	LoadSettings();
	status_timer->start();
}

SchedulerPanel::~SchedulerPanel()
{
	FlushAutosave();
	status_timer->stop();
}

void SchedulerPanel::SetupUI()
{
	setMinimumWidth(450);

	auto *main_layout = new QVBoxLayout(this);
	main_layout->setContentsMargins(6, 6, 6, 6);
	main_layout->setSpacing(4);

	/* ---- API Credentials (collapsible) ---- */
	cred_container = new QWidget(this);
	auto *cred_form = new QFormLayout(cred_container);
	cred_form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
	cred_form->setContentsMargins(12, 0, 6, 4);
	cred_form->setVerticalSpacing(4);

	client_id_edit = new QLineEdit(this);
	client_secret_edit = new QLineEdit(this);
	client_secret_edit->setEchoMode(QLineEdit::Password);
	app_key_edit = new QLineEdit(this);
	app_key_edit->setMaxLength(6);
	/* Valid app keys are always uppercase; force-uppercase keystrokes and
	 * pastes so a lowercase entry can't silently fail with "authenticated,
	 * but not authorized to read broadcasts". setText() emits textChanged
	 * (not textEdited), so this never re-triggers autosave; the guard stops
	 * the recursion. */
	connect(app_key_edit, &QLineEdit::textChanged, this, [this](const QString &text) {
		const QString upper = text.toUpper();
		if (upper != text) {
			const int pos = app_key_edit->cursorPosition();
			app_key_edit->blockSignals(true);
			app_key_edit->setText(upper);
			app_key_edit->setCursorPosition(pos);
			app_key_edit->blockSignals(false);
		}
		/* Flip Enable the instant the last required cred is filled/cleared,
		 * without waiting for the autosave debounce. */
		UpdateEnableButtonState();
	});

	cred_form->addRow(T("Credentials.ClientId"), client_id_edit);
	cred_form->addRow(T("Credentials.ClientSecret"), client_secret_edit);
	cred_form->addRow(T("Credentials.AppKey"), app_key_edit);

	/* Test Connection lives with the creds -- it's a setup/troubleshoot
	 * action, so it tucks away with the section during normal operation. */
	test_btn = new QPushButton(T("Buttons.TestConnection"), cred_container);
	test_btn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	cred_form->addRow(test_btn);

	cred_toggle_btn = MakeCollapsibleHeader("Credentials.Title", cred_container);
	main_layout->addWidget(cred_toggle_btn);
	main_layout->addWidget(cred_container);

	/* ---- Schedule Settings (collapsible) ---- */
	sched_container = new QWidget(this);
	auto *sched_grid = new QGridLayout(sched_container);
	sched_grid->setContentsMargins(12, 0, 6, 4);
	sched_grid->setVerticalSpacing(4);

	start_lead_spin = new QSpinBox(this);
	start_lead_spin->setRange(0, 2);
	start_lead_spin->setValue(SCHED_DEFAULT_START_LEAD_MINUTES);
	start_lead_spin->setMinimumWidth(80);

	stop_lag_spin = new QSpinBox(this);
	stop_lag_spin->setRange(0, 10);
	stop_lag_spin->setValue(SCHED_DEFAULT_STOP_LAG_MINUTES);
	stop_lag_spin->setMinimumWidth(80);

	sched_grid->addWidget(new QLabel(T("Schedule.StartLead"), this), 0, 0);
	sched_grid->addWidget(start_lead_spin, 0, 1);

	sched_grid->addWidget(new QLabel(T("Schedule.StopLag"), this), 1, 0);
	sched_grid->addWidget(stop_lag_spin, 1, 1);

	sched_toggle_btn = MakeCollapsibleHeader("Schedule.Title", sched_container);
	main_layout->addWidget(sched_toggle_btn);
	main_layout->addWidget(sched_container);

	/* ---- Primary actions (always visible, full width) ---- */
	enable_btn = new QPushButton(T("Buttons.Enable"), this);
	enable_btn->setCheckable(true);
	main_layout->addWidget(enable_btn);

	/* Manual "check now" -- only meaningful while the poll thread is alive. */
	refresh_btn = new QPushButton(T("Buttons.Refresh"), this);
	refresh_btn->setEnabled(false);
	main_layout->addWidget(refresh_btn);

	/* Autosave flash on its own thin line so it never steals button width. */
	autosave_label = new QLabel("", this);
	autosave_label->setStyleSheet("QLabel { color: #3fb950; font-weight: bold; }");
	autosave_label->setAlignment(Qt::AlignCenter);
	main_layout->addWidget(autosave_label);

	/* ---- Status ---- */
	auto *status_group = new QGroupBox(T("Status.Title"), this);
	auto *status_grid = new QGridLayout;
	status_grid->setContentsMargins(6, 0, 6, 4);
	status_grid->setVerticalSpacing(2);
	status_grid->setColumnStretch(1, 1);

	conn_status_label = new QLabel(T("Status.NotConfigured"), this);
	/* Error messages (e.g. "Signed in, but not authorized…") run long;
	 * wrap instead of truncating mid-sentence. Size to content so a
	 * single-line value doesn't leave dead space between rows. */
	conn_status_label->setWordWrap(true);
	sched_status_label = new QLabel(T("Status.Stopped"), this);
	next_broadcast_label = new QLabel(T("Status.NA"), this);
	/* Wrap long broadcast strings, but size to content so a single-line
	 * value doesn't leave a dead row above Next Refresh. */
	next_broadcast_label->setWordWrap(true);
	next_refresh_label = new QLabel(T("Status.NA"), this);
	last_activity_label = new QLabel("", this);
	last_activity_label->setWordWrap(true);

	/* Explain the adaptive cadence so a just-created near-term event isn't a
	 * surprise; both the caption and value carry it since either may be hovered.
	 * Wrap in a fixed-width rich-text block so the tooltip word-wraps into a
	 * readable column instead of rendering as one very wide line. */
	const QString cadence_tip =
		QStringLiteral("<table><tr><td width='280'>%1</td></tr></table>").arg(T("Status.NextRefresh.Tooltip"));
	/* Dimmed info glyph hints that hovering the row reveals an explanation. */
	auto *next_refresh_caption = new QLabel(
		QStringLiteral("%1 <span style='color:#8b949e'>\u24D8</span>").arg(T("Status.NextRefresh")), this);
	next_refresh_caption->setToolTip(cadence_tip);
	next_refresh_label->setToolTip(cadence_tip);

	status_grid->addWidget(new QLabel(T("Status.Connection"), this), 0, 0, Qt::AlignRight);
	status_grid->addWidget(conn_status_label, 0, 1);
	status_grid->addWidget(new QLabel(T("Status.Scheduler"), this), 1, 0, Qt::AlignRight);
	status_grid->addWidget(sched_status_label, 1, 1);
	status_grid->addWidget(new QLabel(T("Status.NextBroadcast"), this), 2, 0, Qt::AlignRight | Qt::AlignTop);
	next_broadcast_label->setAlignment(Qt::AlignTop | Qt::AlignLeft);
	status_grid->addWidget(next_broadcast_label, 2, 1, Qt::AlignTop);
	status_grid->addWidget(next_refresh_caption, 3, 0, Qt::AlignRight);
	status_grid->addWidget(next_refresh_label, 3, 1);
	status_grid->addWidget(new QLabel(T("Status.LastActivity"), this), 4, 0, Qt::AlignRight);
	status_grid->addWidget(last_activity_label, 4, 1);
	status_group->setLayout(status_grid);
	main_layout->addWidget(status_group);

	main_layout->addStretch();

	setLayout(main_layout);

	connect(test_btn, &QPushButton::clicked, this, &SchedulerPanel::OnTestConnection);
	connect(enable_btn, &QPushButton::toggled, this, &SchedulerPanel::OnEnableToggled);
	connect(refresh_btn, &QPushButton::clicked, this, &SchedulerPanel::OnRefresh);

	/* ---- Auto-save (no Save button) ----
	 * Edits schedule a debounced save; editingFinished flushes immediately
	 * on Enter/focus-out; discrete toggles save right away. */
	connect(client_id_edit, &QLineEdit::textEdited, this, &SchedulerPanel::ScheduleAutosave);
	connect(client_secret_edit, &QLineEdit::textEdited, this, &SchedulerPanel::ScheduleAutosave);
	connect(app_key_edit, &QLineEdit::textEdited, this, &SchedulerPanel::ScheduleAutosave);
	/* Keep Enable's availability in sync as credentials are typed/cleared
	 * (app_key is handled in its uppercase textChanged lambda above). */
	connect(client_id_edit, &QLineEdit::textChanged, this, &SchedulerPanel::UpdateEnableButtonState);
	connect(client_secret_edit, &QLineEdit::textChanged, this, &SchedulerPanel::UpdateEnableButtonState);
	connect(client_id_edit, &QLineEdit::editingFinished, this, &SchedulerPanel::OnFieldChanged);
	connect(client_secret_edit, &QLineEdit::editingFinished, this, &SchedulerPanel::OnFieldChanged);
	connect(app_key_edit, &QLineEdit::editingFinished, this, &SchedulerPanel::OnFieldChanged);
	connect(start_lead_spin, QOverload<int>::of(&QSpinBox::valueChanged), this, &SchedulerPanel::ScheduleAutosave);
	connect(stop_lag_spin, QOverload<int>::of(&QSpinBox::valueChanged), this, &SchedulerPanel::ScheduleAutosave);
	connect(start_lead_spin, &QSpinBox::editingFinished, this, &SchedulerPanel::OnFieldChanged);
	connect(stop_lag_spin, &QSpinBox::editingFinished, this, &SchedulerPanel::OnFieldChanged);
}

QToolButton *SchedulerPanel::MakeCollapsibleHeader(const char *title_key, QWidget *container)
{
	auto *btn = new QToolButton(this);
	btn->setText(T(title_key));
	btn->setCheckable(true);
	btn->setChecked(true);
	btn->setAutoRaise(true);
	btn->setArrowType(Qt::DownArrow);
	btn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
	btn->setStyleSheet("QToolButton { border: none; font-weight: bold; }");
	btn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

	connect(btn, &QToolButton::toggled, this, [this, btn, container](bool expanded) {
		/* Commit any in-flight edit before the fold hides the field. */
		FlushAutosave();
		container->setVisible(expanded);
		btn->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
		/* User took manual control -- stop auto-collapsing on them. */
		initial_collapse_done = true;
		FitDockToContents();
	});
	return btn;
}

void SchedulerPanel::SetSectionCollapsed(QToolButton *btn, QWidget *container, bool collapsed)
{
	btn->blockSignals(true);
	btn->setChecked(!collapsed);
	btn->setArrowType(collapsed ? Qt::RightArrow : Qt::DownArrow);
	btn->blockSignals(false);
	container->setVisible(!collapsed);
}

void SchedulerPanel::CollapseConfiguredSections()
{
	if (initial_collapse_done)
		return;
	SetSectionCollapsed(cred_toggle_btn, cred_container, true);
	SetSectionCollapsed(sched_toggle_btn, sched_container, true);
	initial_collapse_done = true;
	FitDockToContents();
}

void SchedulerPanel::ScheduleAutosave()
{
	if (is_loading)
		return;
	autosave_timer->start();
}

void SchedulerPanel::FlushAutosave()
{
	if (autosave_timer->isActive())
		OnFieldChanged();
}

/*
 * Collapsing a section leaves dead space because the dock keeps its old
 * height (the panel's trailing stretch just absorbs it). Fit the dock to
 * its contents in both states:
 *   - Floating: the dock is its own top-level window, so resize() it. The
 *     OBS main window is never touched.
 *   - Docked: we must not resize the main window, but briefly clamping the
 *     dock's maxHeight makes QMainWindow's dock layout reclaim the freed
 *     space; we then restore the cap so the user can still grow it.
 * Three subtleties:
 *   1. The dock's cached sizeHint lags a child visibility change by an event
 *      cycle, so we force a recompute (adjustSize/updateGeometry) and measure
 *      on a later tick once the layout has settled.
 *   2. The dock won't shrink below its (stale) minimum via resize() alone, so
 *      the maxHeight clamp is what actually forces the shrink in both states.
 *   3. The status rows use word-wrap, whose sizeHint is width-independent and
 *      inflated, so dock->sizeHint() over-reserves height. We measure the
 *      panel's true content height via heightForWidth(width()) and add the
 *      dock chrome to get an accurate clamp target.
 */
void SchedulerPanel::FitDockToContents()
{
	QWidget *w = parentWidget();
	QDockWidget *dock = nullptr;
	while (w) {
		dock = qobject_cast<QDockWidget *>(w);
		if (dock)
			break;
		w = w->parentWidget();
	}
	if (!dock)
		return;

	QTimer::singleShot(0, dock, [dock, this]() {
		/* Invalidate (don't resize) so the sizeHint recomputes after the
		 * section toggle. adjustSize() would shrink the panel to its
		 * content width, leaving dead space on the right when docked. */
		updateGeometry();
		dock->updateGeometry();
		QTimer::singleShot(50, dock, [dock, this]() {
			/* Word-wrap labels report a width-independent (inflated)
			 * sizeHint, so dock->sizeHint() over-reserves height and the
			 * dock won't shrink tight on collapse. Measure the panel's real
			 * content height at its current width via heightForWidth and add
			 * the dock chrome (title bar/margins) so we clamp to what the
			 * collapsed layout actually needs. */
			int target = dock->sizeHint().height();
			if (hasHeightForWidth() && width() > 0) {
				const int content_h = heightForWidth(width());
				if (content_h > 0) {
					const int chrome = dock->sizeHint().height() - sizeHint().height();
					target = content_h + (chrome > 0 ? chrome : 0);
				}
			}
			if (dock->isFloating())
				dock->resize(dock->width(), target);
			dock->setMaximumHeight(target);
			QTimer::singleShot(60, dock, [dock]() { dock->setMaximumHeight(QWIDGETSIZE_MAX); });
		});
	});
}

void SchedulerPanel::FlashAutosaved()
{
	autosave_label->setText(QStringLiteral("\u2713 ") + T("Status.Saved"));
	QTimer::singleShot(2500, this, [this]() { autosave_label->setText(""); });
}

void SchedulerPanel::LoadSettings()
{
	/* Suppress autosave/flash while we push stored values into the widgets;
	 * setValue()/setChecked() emit valueChanged/toggled that would otherwise
	 * trigger a spurious "Saved" flash on startup. */
	is_loading = true;

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

	obs_data_release(data);

	enable_btn->setChecked(g_scheduler_enabled && g_scheduler.running);
	enable_btn->setText(enable_btn->isChecked() ? T("Buttons.Disable") : T("Buttons.Enable"));
	refresh_btn->setEnabled(enable_btn->isChecked());

	/* If credentials are already configured, collapse both setup sections to
	 * reduce clutter (and mild screen-share exposure) and let Status take the
	 * dock. Empty creds stay expanded so first-time setup is obvious. */
	if (CredsComplete())
		CollapseConfiguredSections();

	is_loading = false;

	UpdateEnableButtonState();

	/* Populate the Connection field with a real result instead of leaving it
	 * "Unknown". When the scheduler is already running, OnStatusTick reflects
	 * live health, so only probe here while it's stopped. */
	if (CredsComplete() && !g_scheduler.running)
		StartConnectionCheck(false);
	else if (!CredsComplete())
		SetLabelState(conn_status_label, StatusColor::Grey);
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
	obs_data_set_bool(data, "enabled", g_scheduler_enabled);

	obs_data_save_json_safe(data, path, "tmp", "bak");
	obs_data_release(data);

	obs_log(LOG_INFO, "Settings saved");
}

bool SchedulerPanel::CredsComplete() const
{
	return !client_id_edit->text().isEmpty() && !client_secret_edit->text().isEmpty() &&
	       !app_key_edit->text().isEmpty();
}

void SchedulerPanel::UpdateEnableButtonState()
{
	/* Allow toggling when creds are complete, or while already running so
	 * Disable is never locked out. */
	const bool allow = CredsComplete() || enable_btn->isChecked();
	enable_btn->setEnabled(allow);
	/* Best-effort hint (disabled widgets may not deliver hover events on
	 * all platforms; the grey "Not configured" Connection row and the
	 * expanded Credentials section are the primary cue). */
	enable_btn->setToolTip(allow ? QString() : T("Buttons.EnableNeedsCreds"));
}

void SchedulerPanel::SetLabelState(QLabel *label, StatusColor color)
{
	/* OnStatusTick fires every second; only restyle when the color actually
	 * changes to avoid needless stylesheet churn/flicker. */
	const QVariant prev = label->property("statusColor");
	if (prev.isValid() && prev.toInt() == static_cast<int>(color))
		return;
	label->setProperty("statusColor", static_cast<int>(color));

	const char *css = "";
	switch (color) {
	case StatusColor::Green:
		css = "QLabel { color: #3fb950; font-weight: bold; }";
		break;
	case StatusColor::Amber:
		css = "QLabel { color: #d29922; font-weight: bold; }";
		break;
	case StatusColor::Red:
		css = "QLabel { color: #f85149; font-weight: bold; }";
		break;
	case StatusColor::Grey:
		css = "QLabel { color: #8b949e; font-weight: bold; }";
		break;
	}
	label->setStyleSheet(css);
}

/*
 * Authenticate against the API on a background thread and report the real
 * result in the Connection field, so it never sits at a meaningless
 * "Unknown". Network I/O must stay off the UI thread or OBS would hang on
 * startup. A generation counter discards results from superseded checks
 * (e.g. creds edited again before the previous check returned).
 */
void SchedulerPanel::StartConnectionCheck(bool collapse_on_success)
{
	/* Bump the generation first so an in-flight check is invalidated even
	 * when we short-circuit below -- otherwise clearing a credential while a
	 * previous check is running lets its late "Connected" result overwrite
	 * the "Not configured" state we set here. */
	const unsigned gen = ++conn_check_gen;

	if (!CredsComplete()) {
		conn_status_label->setText(T("Status.NotConfigured"));
		SetLabelState(conn_status_label, StatusColor::Grey);
		return;
	}

	conn_status_label->setText(T("Status.Checking"));
	SetLabelState(conn_status_label, StatusColor::Amber);

	QPointer<SchedulerPanel> self(this);
	const std::string id = client_id_edit->text().toStdString();
	const std::string secret = client_secret_edit->text().toStdString();
	const std::string app = app_key_edit->text().toStdString();
	const QString init_failed = T("Status.InitFailed");

	std::thread([self, gen, id, secret, app, init_failed, collapse_on_success]() {
		subsplash_client_t client;
		bool success = false;
		QString text;

		if (subsplash_client_init(&client, DEFAULT_BASE_URL, id.c_str(), secret.c_str(), app.c_str())) {
			char status_buf[256];
			success = subsplash_client_test_connection(&client, status_buf, sizeof(status_buf));
			text = QString::fromUtf8(status_buf);
			subsplash_client_destroy(&client);
		} else {
			text = init_failed;
		}

		/* Hop back to the UI thread via qApp. During OBS shutdown
		 * instance() can already be null, so guard it before posting;
		 * the QPointer separately guards the panel being destroyed. */
		auto *app = QCoreApplication::instance();
		if (!app)
			return;
		QMetaObject::invokeMethod(
			app,
			[self, gen, text, success, collapse_on_success]() {
				if (!self || self->conn_check_gen != gen)
					return;
				self->conn_status_label->setText(text);
				self->SetLabelState(self->conn_status_label,
						    success ? StatusColor::Green : StatusColor::Red);
				if (success && collapse_on_success)
					self->CollapseConfiguredSections();
			},
			Qt::QueuedConnection);
	}).detach();
}

void SchedulerPanel::OnTestConnection()
{
	StartConnectionCheck(true);
}

void SchedulerPanel::OnFieldChanged()
{
	if (is_loading)
		return;

	/* Cancel any pending debounce so a flush + timeout don't double-save. */
	autosave_timer->stop();

	SaveSettings();

	if (g_scheduler.running) {
		scheduler_configure(&g_scheduler, DEFAULT_BASE_URL, client_id_edit->text().toUtf8().constData(),
				    client_secret_edit->text().toUtf8().constData(),
				    app_key_edit->text().toUtf8().constData(), start_lead_spin->value(),
				    stop_lag_spin->value());
	} else {
		/* Re-probe so the Connection field tracks the edited creds. While
		 * running, the poll thread already drives OnStatusTick. */
		StartConnectionCheck(false);
	}

	UpdateEnableButtonState();
	FlashAutosaved();
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

		/* Refresh Connection now that the poll thread won't; avoids a
		 * stale "Streaming"/"Connected" lingering after stop. */
		StartConnectionCheck(false);
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

	UpdateEnableButtonState();

	if (running) {
		sched_status_label->setText(T("Status.Running"));

		char status_buf[256];
		char next_buf[256];
		char activity_buf[256];
		scheduler_get_status(&g_scheduler, status_buf, sizeof(status_buf), next_buf, sizeof(next_buf),
				     activity_buf, sizeof(activity_buf));
		next_broadcast_label->setText(next_buf);
		last_activity_label->setText(activity_buf);
		next_refresh_label->setText(FormatNextRefresh(scheduler_get_next_poll_epoch(&g_scheduler)));

		/* Reflect the poll thread's real health rather than assuming
		 * success: streaming when live, the engine's cause-specific error
		 * text while the API is failing (e.g. "Not authorized…" vs
		 * "Connection problem…"), otherwise connected. Color the rows so
		 * state reads at a glance. */
		const bool streaming = obs_frontend_streaming_active();
		const bool failing = sched_atomic_load(&g_scheduler.consecutive_failures) > 0;
		if (streaming) {
			conn_status_label->setText(T("Status.Streaming"));
			SetLabelState(conn_status_label, StatusColor::Green);
		} else if (failing) {
			QString cause = QString::fromUtf8(status_buf);
			if (cause.isEmpty())
				cause = T("Status.ApiError");
			conn_status_label->setText(cause);
			SetLabelState(conn_status_label, StatusColor::Red);
		} else {
			conn_status_label->setText(T("Status.Connected"));
			SetLabelState(conn_status_label, StatusColor::Green);
		}

		/* Sync row: green when live, red when the API is failing, amber
		 * while running but waiting (not yet live). */
		SetLabelState(sched_status_label,
			      streaming ? StatusColor::Green : (failing ? StatusColor::Red : StatusColor::Amber));

		/* Once we're authenticated, collapse the setup sections once. */
		CollapseConfiguredSections();
	} else {
		sched_status_label->setText(T("Status.Stopped"));
		SetLabelState(sched_status_label, StatusColor::Grey);
		next_broadcast_label->setText(T("Status.NA"));
		next_refresh_label->setText(T("Status.NA"));
		last_activity_label->setText("");
	}
}
