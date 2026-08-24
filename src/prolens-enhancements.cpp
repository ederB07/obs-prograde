#include "prolens-enhancements.hpp"

#include "ptz-controls.hpp"
#include "ptz-list-model.hpp"
#include "ptz.h"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <graphics/graphics.h>

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QSettings>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <cmath>

namespace {

class FocusWheel : public QWidget {
public:
	FocusWheel(QWidget *parent = nullptr) : QWidget(parent)
	{
		setMinimumSize(86, 86);
		setMaximumSize(110, 110);
		setCursor(Qt::OpenHandCursor);
	}

	std::function<void(double)> focusChanged;

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing);
		const int s = std::min(width(), height()) - 8;
		QRectF r((width() - s) / 2.0, (height() - s) / 2.0, s, s);
		QPointF c = r.center();
		p.setPen(QPen(QColor(86, 88, 96), 2));
		p.setBrush(QColor(34, 35, 40));
		p.drawEllipse(r);
		p.setPen(QPen(QColor(150, 152, 162), 2));
		for (int i = 0; i < 24; ++i) {
			double a = i * M_PI * 2.0 / 24.0 + angle;
			double r1 = s * 0.34;
			double r2 = s * 0.43;
			p.drawLine(QPointF(c.x() + std::cos(a) * r1, c.y() + std::sin(a) * r1),
				   QPointF(c.x() + std::cos(a) * r2, c.y() + std::sin(a) * r2));
		}
		p.setPen(QColor(228, 228, 232));
		QFont f = p.font();
		f.setBold(true);
		f.setPointSize(8);
		p.setFont(f);
		p.drawText(r, Qt::AlignCenter, "FOCUS");
	}

	void mousePressEvent(QMouseEvent *e) override
	{
		last = e->position();
		setCursor(Qt::ClosedHandCursor);
	}

	void mouseMoveEvent(QMouseEvent *e) override
	{
		if (!(e->buttons() & Qt::LeftButton))
			return;
		double dx = e->position().x() - last.x();
		last = e->position();
		angle += dx * 0.025;
		update();
		if (focusChanged)
			focusChanged(std::clamp(dx / 24.0, -1.0, 1.0));
	}

	void mouseReleaseEvent(QMouseEvent *) override
	{
		setCursor(Qt::OpenHandCursor);
		if (focusChanged)
			focusChanged(0.0);
	}

	void wheelEvent(QWheelEvent *e) override
	{
		double v = e->angleDelta().y() >= 0 ? 0.55 : -0.55;
		angle += v * 0.15;
		update();
		if (focusChanged)
			focusChanged(v);
		QTimer::singleShot(110, this, [this]() {
			if (focusChanged)
				focusChanged(0.0);
		});
		e->accept();
	}

private:
	QPointF last;
	double angle = 0.0;
};

class ProlensPanel : public QWidget {
public:
	explicit ProlensPanel(PTZControls *controls) : QWidget(controls), controls(controls)
	{
		setObjectName("prolensEnhancements");
		setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
		setStyleSheet(
			"#prolensEnhancements{background:rgba(26,27,31,180);border:1px solid rgba(120,120,130,90);border-radius:8px;}"
			"QToolButton,QPushButton{min-height:24px;}"
			"QToolButton:checked{background:#6430a8;}"
			"QGroupBox{border:0;margin-top:2px;}"
			"QLineEdit,QComboBox{min-height:24px;}"
		);

		auto *root = new QVBoxLayout(this);
		root->setContentsMargins(7, 7, 7, 7);
		root->setSpacing(6);

		auto *controlRow = new QHBoxLayout;
		controlRow->setSpacing(7);

		focusWheel = new FocusWheel(this);
		focusWheel->setToolTip("Arraste ou use a roda do mouse para ajustar o foco manual");
		focusWheel->focusChanged = [this](double speed) { sendFocus(speed); };
		controlRow->addWidget(focusWheel, 0, Qt::AlignCenter);

		auto *mini = new QVBoxLayout;
		mini->setSpacing(4);
		afButton = new QToolButton(this);
		afButton->setText("AF");
		afButton->setCheckable(true);
		afButton->setChecked(true);
		afButton->setToolTip("Auto Focus / Manual Focus");
		connect(afButton, &QToolButton::toggled, this, [this](bool on) { sendSetBool("focus_af_enabled", on); });

		trackingButton = new QToolButton(this);
		trackingButton->setText(QString::fromUtf8("◎"));
		trackingButton->setCheckable(true);
		trackingButton->setToolTip("Auto Tracking. Botão direito: modo/configuração");
		trackingButton->setContextMenuPolicy(Qt::CustomContextMenu);
		connect(trackingButton, &QToolButton::toggled, this, [this](bool on) { setTracking(on); });
		connect(trackingButton, &QWidget::customContextMenuRequested, this, [this](const QPoint &p) {
			showTrackingMenu(trackingButton->mapToGlobal(p));
		});
		mini->addWidget(afButton);
		mini->addWidget(trackingButton);
		mini->addStretch();
		controlRow->addLayout(mini);
		controlRow->addStretch();

		exposureToggle = new QToolButton(this);
		exposureToggle->setText("EXPOSIÇÃO  ▾");
		exposureToggle->setCheckable(true);
		exposureToggle->setToolButtonStyle(Qt::ToolButtonTextOnly);
		controlRow->addWidget(exposureToggle, 0, Qt::AlignTop);
		root->addLayout(controlRow);

		exposureBox = buildExposureBox();
		exposureBox->setVisible(false);
		root->addWidget(exposureBox);
		connect(exposureToggle, &QToolButton::toggled, exposureBox, &QWidget::setVisible);

		auto *presetHeader = new QHBoxLayout;
		auto *presetLabel = new QLabel("PRESETS", this);
		presetLabel->setStyleSheet("font-weight:600;font-size:11px;");
		presetHeader->addWidget(presetLabel);
		presetHeader->addStretch();
		QLabel *hint = new QLabel("clique: chamar  •  botão direito: salvar foto", this);
		hint->setStyleSheet("color:#92949d;font-size:9px;");
		presetHeader->addWidget(hint);
		root->addLayout(presetHeader);

		auto *presetGrid = new QGridLayout;
		presetGrid->setSpacing(5);
		for (int i = 0; i < 8; ++i) {
			auto *b = new QToolButton(this);
			b->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
			b->setIconSize(QSize(92, 52));
			b->setText(QString("P%1").arg(i + 1));
			b->setMinimumWidth(74);
			b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
			b->setContextMenuPolicy(Qt::CustomContextMenu);
			connect(b, &QToolButton::clicked, this, [this, i]() { recallPreset(i); });
			connect(b, &QWidget::customContextMenuRequested, this, [this, b, i](const QPoint &p) {
				QMenu m;
				auto *save = m.addAction("Salvar posição + miniatura");
				auto *clear = m.addAction("Remover miniatura");
				auto *chosen = m.exec(b->mapToGlobal(p));
				if (chosen == save)
					savePreset(i);
				else if (chosen == clear) {
					QFile::remove(thumbnailPath(currentDeviceId(), i));
					refreshPresets();
				}
			});
			presetButtons[i] = b;
			presetGrid->addWidget(b, i / 4, i % 4);
		}
		root->addLayout(presetGrid);

		cameraList = controls->findChild<QListView *>("cameraList");
		if (cameraList && cameraList->selectionModel()) {
			connect(cameraList->selectionModel(), &QItemSelectionModel::currentChanged, this,
				[this](const QModelIndex &, const QModelIndex &) { refreshPresets(); });
		}

		for (const char *name : {"focusButton_near", "focusButton_far", "focusButton_onetouch"}) {
			if (auto *w = controls->findChild<QWidget *>(name))
				w->hide();
		}
		if (auto *oldPresets = controls->findChild<QWidget *>("presetListView"))
			oldPresets->hide();

		refreshPresets();
	}

private:
	QWidget *buildExposureBox()
	{
		auto *box = new QGroupBox(this);
		auto *v = new QVBoxLayout(box);
		v->setContentsMargins(0, 0, 0, 0);
		v->setSpacing(4);

		auto *modeRow = new QHBoxLayout;
		modeRow->addWidget(new QLabel("Modo", box));
		auto *mode = new QComboBox(box);
		mode->addItem("Auto", 0);
		mode->addItem("Manual", 1);
		mode->addItem("Prioridade Shutter", 2);
		mode->addItem("Prioridade Iris", 3);
		mode->addItem("Bright", 4);
		modeRow->addWidget(mode, 1);
		v->addLayout(modeRow);
		connect(mode, &QComboBox::currentIndexChanged, this, [this, mode](int) {
			sendSetInt("exposure_mode", mode->currentData().toInt());
		});

		auto addStepper = [this, v, box](const QString &label, const char *property) {
			auto *row = new QHBoxLayout;
			auto *l = new QLabel(label, box);
			auto *minus = new QToolButton(box);
			auto *plus = new QToolButton(box);
			minus->setText("−");
			plus->setText("+");
			row->addWidget(l);
			row->addStretch();
			row->addWidget(minus);
			row->addWidget(plus);
			connect(minus, &QToolButton::clicked, this, [this, property]() { sendSetInt(property, -1); });
			connect(plus, &QToolButton::clicked, this, [this, property]() { sendSetInt(property, 1); });
			v->addLayout(row);
		};
		addStepper("Iris", "iris_delta");
		addStepper("Shutter", "shutter_delta");
		addStepper("Gain", "gain_delta");
		return box;
	}

	QModelIndex currentIndex() const
	{
		return cameraList ? cameraList->currentIndex() : QModelIndex();
	}

	uint32_t currentDeviceId() const
	{
		auto idx = currentIndex();
		return idx.isValid() ? idx.data(PTZListModel::DeviceIdRole).toUInt() : 0;
	}

	void sendFocus(double speed)
	{
		auto idx = currentIndex();
		if (!idx.isValid())
			return;
		calldata_t cd;
		calldata_init(&cd);
		calldata_set_float(&cd, "focus", speed);
		ptzDeviceList.callDevice(idx, "ptz_move", &cd);
		calldata_free(&cd);
	}

	void sendSetBool(const char *name, bool value)
	{
		auto idx = currentIndex();
		if (!idx.isValid())
			return;
		calldata_t cd;
		calldata_init(&cd);
		calldata_set_bool(&cd, name, value);
		ptzDeviceList.callDevice(idx, "ptz_set", &cd);
		calldata_free(&cd);
	}

	void sendSetInt(const char *name, long long value)
	{
		auto idx = currentIndex();
		if (!idx.isValid())
			return;
		calldata_t cd;
		calldata_init(&cd);
		calldata_set_int(&cd, name, value);
		ptzDeviceList.callDevice(idx, "ptz_set", &cd);
		calldata_free(&cd);
	}

	void sendRaw(const QString &hex)
	{
		if (hex.trimmed().isEmpty())
			return;
		auto idx = currentIndex();
		if (!idx.isValid())
			return;
		calldata_t cd;
		calldata_init(&cd);
		QByteArray utf = hex.toUtf8();
		calldata_set_string(&cd, "visca_raw_hex", utf.constData());
		ptzDeviceList.callDevice(idx, "ptz_set", &cd);
		calldata_free(&cd);
	}

	QString cameraSettingsPrefix() const
	{
		return QString("camera/%1/").arg(currentDeviceId());
	}

	void setTracking(bool enabled)
	{
		QSettings s("Prolens", "PTZControl");
		QString prefix = cameraSettingsPrefix();
		QString cmd = s.value(prefix + (enabled ? "tracking_on" : "tracking_off")).toString();
		if (enabled) {
			QString mode = s.value(prefix + "tracking_mode", "full").toString();
			QString modeCmd = s.value(prefix + "tracking_" + mode).toString();
			if (!modeCmd.isEmpty())
				sendRaw(modeCmd);
		}
		sendRaw(cmd);
	}

	void showTrackingMenu(const QPoint &global)
	{
		QMenu menu;
		QSettings s("Prolens", "PTZControl");
		QString prefix = cameraSettingsPrefix();
		QString current = s.value(prefix + "tracking_mode", "full").toString();
		auto addMode = [&](const QString &label, const QString &key) {
			auto *a = menu.addAction(label);
			a->setCheckable(true);
			a->setChecked(current == key);
			connect(a, &QAction::triggered, this, [this, prefix, key]() {
				QSettings st("Prolens", "PTZControl");
				st.setValue(prefix + "tracking_mode", key);
				if (trackingButton->isChecked())
					sendRaw(st.value(prefix + "tracking_" + key).toString());
			});
		};
		addMode("Full Body", "full");
		addMode("Half Body", "half");
		addMode("Close Up", "close");
		addMode("Custom", "custom");
		menu.addSeparator();
		auto *cfg = menu.addAction("Configurar tracking…");
		connect(cfg, &QAction::triggered, this, [this]() { showTrackingConfig(); });
		menu.exec(global);
	}

	void showTrackingConfig()
	{
		QDialog d(this);
		d.setWindowTitle("Tracking da câmera");
		auto *form = new QFormLayout(&d);
		QSettings s("Prolens", "PTZControl");
		QString prefix = cameraSettingsPrefix();
		std::array<std::pair<QString, QString>, 6> fields = {{{"Tracking ON", "tracking_on"},
			{"Tracking OFF", "tracking_off"}, {"Full Body", "tracking_full"}, {"Half Body", "tracking_half"},
			{"Close Up", "tracking_close"}, {"Custom", "tracking_custom"}}};
		std::array<QLineEdit *, 6> edits{};
		for (size_t i = 0; i < fields.size(); ++i) {
			edits[i] = new QLineEdit(s.value(prefix + fields[i].second).toString(), &d);
			edits[i]->setPlaceholderText("VISCA HEX do fabricante");
			form->addRow(fields[i].first, edits[i]);
		}
		auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &d);
		form->addRow(buttons);
		connect(buttons, &QDialogButtonBox::accepted, &d, &QDialog::accept);
		connect(buttons, &QDialogButtonBox::rejected, &d, &QDialog::reject);
		if (d.exec() == QDialog::Accepted) {
			for (size_t i = 0; i < fields.size(); ++i)
				s.setValue(prefix + fields[i].second, edits[i]->text().trimmed());
		}
	}

	QString thumbnailPath(uint32_t deviceId, int preset) const
	{
		char *base = obs_module_config_path("preset-thumbnails");
		QString dir = QString::fromUtf8(base ? base : "");
		bfree(base);
		QDir().mkpath(dir);
		return QDir(dir).filePath(QString("cam-%1-preset-%2.png").arg(deviceId).arg(preset));
	}

	QImage captureSource(obs_source_t *source)
	{
		if (!source)
			return {};
		const uint32_t sw = obs_source_get_width(source);
		const uint32_t sh = obs_source_get_height(source);
		if (!sw || !sh)
			return {};
		const uint32_t w = 240;
		const uint32_t h = std::max<uint32_t>(80, (uint64_t)w * sh / sw);
		QImage result;

		obs_enter_graphics();
		gs_texrender_t *render = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		gs_stagesurf_t *stage = gs_stagesurface_create(w, h, GS_RGBA);
		if (render && stage && gs_texrender_begin(render, w, h)) {
			gs_viewport_push();
			gs_projection_push();
			gs_matrix_push();
			gs_set_viewport(0, 0, w, h);
			gs_ortho(0.0f, (float)sw, 0.0f, (float)sh, -100.0f, 100.0f);
			gs_matrix_identity();
			obs_source_video_render(source);
			gs_matrix_pop();
			gs_projection_pop();
			gs_viewport_pop();
			gs_texrender_end(render);
			gs_texture_t *tex = gs_texrender_get_texture(render);
			if (tex) {
				gs_stage_texture(stage, tex);
				uint8_t *data = nullptr;
				uint32_t linesize = 0;
				if (gs_stagesurface_map(stage, &data, &linesize)) {
					QImage tmp(data, w, h, linesize, QImage::Format_RGBA8888);
					result = tmp.copy().mirrored(false, true);
					gs_stagesurface_unmap(stage);
				}
			}
		}
		if (stage)
			gs_stagesurface_destroy(stage);
		if (render)
			gs_texrender_destroy(render);
		obs_leave_graphics();
		return result;
	}

	void recallPreset(int preset)
	{
		uint32_t id = currentDeviceId();
		if (id)
			ptzDeviceList.preset_recall(id, preset);
	}

	void savePreset(int preset)
	{
		uint32_t id = currentDeviceId();
		if (!id)
			return;
		ptzDeviceList.preset_save(id, preset);
		QTimer::singleShot(180, this, [this, id, preset]() {
			obs_source_t *source = ptz_device_find_source_using_ptz_name(id);
			if (source) {
				QImage image = captureSource(source);
				if (!image.isNull())
					image.save(thumbnailPath(id, preset), "PNG");
				obs_source_release(source);
			}
			refreshPresets();
		});
	}

	void refreshPresets()
	{
		uint32_t id = currentDeviceId();
		for (int i = 0; i < 8; ++i) {
			QString path = id ? thumbnailPath(id, i) : QString();
			QImage img(path);
			if (!img.isNull())
				presetButtons[i]->setIcon(QPixmap::fromImage(img.scaled(184, 104, Qt::KeepAspectRatioByExpanding,
										Qt::SmoothTransformation)));
			else
				presetButtons[i]->setIcon(QIcon());
			presetButtons[i]->setEnabled(id != 0);
		}
	}

	PTZControls *controls = nullptr;
	QListView *cameraList = nullptr;
	FocusWheel *focusWheel = nullptr;
	QToolButton *afButton = nullptr;
	QToolButton *trackingButton = nullptr;
	QToolButton *exposureToggle = nullptr;
	QWidget *exposureBox = nullptr;
	std::array<QToolButton *, 8> presetButtons{};
};

ProlensPanel *gPanel = nullptr;

void attachEnhancements()
{
	PTZControls *controls = PTZControls::getInstance();
	if (!controls || gPanel)
		return;
	auto *layout = qobject_cast<QBoxLayout *>(controls->layout());
	if (!layout)
		return;
	gPanel = new ProlensPanel(controls);
	layout->addWidget(gPanel);
}

} // namespace

extern "C" void prolens_ptz_enhancements_load()
{
	QTimer::singleShot(0, []() { attachEnhancements(); });
}

extern "C" void prolens_ptz_enhancements_unload()
{
	if (gPanel) {
		delete gPanel;
		gPanel = nullptr;
	}
}
