#include <obs-module.h>
#include <obs-frontend-api.h>

#include <QComboBox>
#include <QDial>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHostAddress>
#include <QIcon>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QPixmap>
#include <QPushButton>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
#include <QTimer>
#include <QToolButton>
#include <QUdpSocket>
#include <QVBoxLayout>
#include <QWidget>

#include <array>
#include <cstdint>
#include <functional>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-prolens-ptz", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
    return "Prolens PTZ Control: VISCA-over-IP dock with focus, exposure, tracking and visual presets";
}

static constexpr const char *DOCK_ID = "prolens_ptz_control";
static constexpr int PRESET_COUNT = 8;

static QString configDir()
{
    char *p = obs_module_config_path("");
    QString out = p ? QString::fromUtf8(p) : QString();
    bfree(p);
    QDir().mkpath(out);
    return out;
}

static QByteArray parseHex(QString text)
{
    text.remove(' ');
    text.remove(':');
    text.remove('-');
    text.replace("0x", "", Qt::CaseInsensitive);
    if (text.size() % 2)
        return {};
    return QByteArray::fromHex(text.toLatin1());
}

class ViscaClient : public QObject {
public:
    explicit ViscaClient(QObject *parent = nullptr) : QObject(parent) {}
    QString host = "192.168.1.100";
    quint16 port = 52381;
    quint32 sequence = 1;

    void sendPayload(const QByteArray &payload)
    {
        if (payload.isEmpty()) return;
        QByteArray packet;
        packet.reserve(payload.size() + 8);
        packet.append(char(0x01)); packet.append(char(0x00));
        packet.append(char((payload.size() >> 8) & 0xff)); packet.append(char(payload.size() & 0xff));
        packet.append(char((sequence >> 24) & 0xff)); packet.append(char((sequence >> 16) & 0xff));
        packet.append(char((sequence >> 8) & 0xff)); packet.append(char(sequence & 0xff));
        packet.append(payload);
        ++sequence;
        socket.writeDatagram(packet, QHostAddress(host), port);
    }
    void sendHex(const QString &hex) { sendPayload(parseHex(hex)); }

    void panTilt(int panDir, int tiltDir, int panSpeed = 8, int tiltSpeed = 8)
    {
        QByteArray p;
        p.append(char(0x81)); p.append(char(0x01)); p.append(char(0x06)); p.append(char(0x01));
        p.append(char(qBound(1, panSpeed, 24))); p.append(char(qBound(1, tiltSpeed, 20)));
        p.append(char(panDir)); p.append(char(tiltDir)); p.append(char(0xff));
        sendPayload(p);
    }
    void stopPT() { panTilt(0x03, 0x03); }
    void zoomStop() { sendPayload(QByteArray::fromHex("8101040700ff")); }
    void zoomTele(int speed = 4) { sendPayload(QByteArray::fromHex("81010407") + QByteArray(1, char(0x20 | qBound(0, speed, 7))) + QByteArray(1, char(0xff))); }
    void zoomWide(int speed = 4) { sendPayload(QByteArray::fromHex("81010407") + QByteArray(1, char(0x30 | qBound(0, speed, 7))) + QByteArray(1, char(0xff))); }
    void focusStop() { sendPayload(QByteArray::fromHex("8101040800ff")); }
    void focusFar() { sendPayload(QByteArray::fromHex("8101040802ff")); }
    void focusNear() { sendPayload(QByteArray::fromHex("8101040803ff")); }
    void focusAuto(bool on) { sendPayload(on ? QByteArray::fromHex("8101043802ff") : QByteArray::fromHex("8101043803ff")); }
    void exposureAuto(bool on) { sendPayload(on ? QByteArray::fromHex("8101043900ff") : QByteArray::fromHex("8101043903ff")); }
    void irisStep(bool up) { sendPayload(up ? QByteArray::fromHex("8101040b02ff") : QByteArray::fromHex("8101040b03ff")); }
    void shutterStep(bool up) { sendPayload(up ? QByteArray::fromHex("8101040a02ff") : QByteArray::fromHex("8101040a03ff")); }
    void gainStep(bool up) { sendPayload(up ? QByteArray::fromHex("8101040c02ff") : QByteArray::fromHex("8101040c03ff")); }
    void presetSet(int preset) { QByteArray p = QByteArray::fromHex("8101043f01"); p.append(char(qBound(0, preset, 255))); p.append(char(0xff)); sendPayload(p); }
    void presetRecall(int preset) { QByteArray p = QByteArray::fromHex("8101043f02"); p.append(char(qBound(0, preset, 255))); p.append(char(0xff)); sendPayload(p); }
private:
    QUdpSocket socket;
};

class HoldButton : public QPushButton {
public:
    std::function<void()> pressedAction;
    std::function<void()> releasedAction;
    explicit HoldButton(const QString &text, QWidget *parent = nullptr) : QPushButton(text, parent)
    {
        connect(this, &QPushButton::pressed, this, [this] { if (pressedAction) pressedAction(); });
        connect(this, &QPushButton::released, this, [this] { if (releasedAction) releasedAction(); });
    }
};

class FocusWheel : public QDial {
public:
    std::function<void(int)> jog;
    explicit FocusWheel(QWidget *parent = nullptr) : QDial(parent)
    {
        setRange(-100, 100); setValue(0); setNotchesVisible(true); setMinimumSize(94, 94);
        connect(this, &QDial::sliderMoved, this, [this](int v) { if (jog && qAbs(v) > 6) jog(v); });
        connect(this, &QDial::sliderReleased, this, [this] { setValue(0); });
    }
};

class ProlensPtzDock : public QWidget {
public:
    explicit ProlensPtzDock(QWidget *parent = nullptr) : QWidget(parent)
    {
        settings = new QSettings(configDir() + "/prolens-ptz.ini", QSettings::IniFormat, this);
        buildUi(); loadSettings(); refreshSources();
    }

    void screenshotReady()
    {
        if (pendingPreset < 0) return;
        char *p = obs_frontend_get_last_screenshot();
        if (!p) return;
        QString src = QString::fromUtf8(p); bfree(p);
        QImage img(src);
        if (img.isNull()) return;
        img = img.scaled(320, 180, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
        QString dst = configDir() + QString("/preset-%1.jpg").arg(pendingPreset);
        img.save(dst, "JPG", 88);
        presetImages[pendingPreset] = dst;
        updatePresetButton(pendingPreset);
        settings->setValue(QString("preset/%1/image").arg(pendingPreset), dst);
        pendingPreset = -1;
    }

private:
    ViscaClient client;
    QSettings *settings = nullptr;
    QLineEdit *ipEdit = nullptr;
    QSpinBox *portSpin = nullptr;
    QComboBox *sourceCombo = nullptr;
    QComboBox *exposureMode = nullptr;
    QToolButton *tracking = nullptr;
    QLineEdit *trackOnHex = nullptr;
    QLineEdit *trackOffHex = nullptr;
    QSlider *speedSlider = nullptr;
    QSlider *zoomSpeedSlider = nullptr;
    QLabel *status = nullptr;
    std::array<QToolButton *, PRESET_COUNT> presetButtons{};
    std::array<QString, PRESET_COUNT> presetImages{};
    int pendingPreset = -1;

    int ptSpeed() const { return speedSlider ? speedSlider->value() : 8; }
    int zoomSpeed() const { return zoomSpeedSlider ? zoomSpeedSlider->value() : 4; }

    QPushButton *button(const QString &text, std::function<void()> fn)
    {
        auto *b = new QPushButton(text);
        connect(b, &QPushButton::clicked, this, [fn] { fn(); });
        return b;
    }
    HoldButton *hold(const QString &text, std::function<void()> start, std::function<void()> stop)
    {
        auto *b = new HoldButton(text); b->pressedAction = std::move(start); b->releasedAction = std::move(stop); return b;
    }

    void buildUi()
    {
        setMinimumWidth(390);
        setStyleSheet("QWidget{font-size:12px;}QGroupBox{font-weight:600;border:1px solid rgba(128,31,221,90);border-radius:8px;margin-top:8px;padding-top:8px;}QGroupBox::title{subcontrol-origin:margin;left:10px;padding:0 5px;}QPushButton,QToolButton{min-height:30px;border-radius:6px;padding:4px 8px;}QToolButton:checked{background:#801fdd;color:white;}");
        auto *root = new QVBoxLayout(this); root->setContentsMargins(8,8,8,8); root->setSpacing(8);

        auto *titleRow = new QHBoxLayout;
        auto *title = new QLabel("PROLENS PTZ CONTROL"); title->setStyleSheet("font-size:15px;font-weight:800;");
        status = new QLabel("● VISCA IP"); status->setStyleSheet("color:#9b9da3;");
        titleRow->addWidget(title); titleRow->addStretch(); titleRow->addWidget(status); root->addLayout(titleRow);

        auto *connection = new QGroupBox("Câmera"); auto *cf = new QFormLayout(connection);
        ipEdit = new QLineEdit; portSpin = new QSpinBox; portSpin->setRange(1,65535); portSpin->setValue(52381); sourceCombo = new QComboBox;
        auto *sourceRow = new QHBoxLayout; sourceRow->addWidget(sourceCombo,1); sourceRow->addWidget(button("↻", [this]{ refreshSources(); }));
        cf->addRow("IP", ipEdit); cf->addRow("Porta", portSpin); cf->addRow("Fonte OBS", sourceRow); root->addWidget(connection);
        connect(ipEdit, &QLineEdit::editingFinished, this, [this]{ syncConnection(); });
        connect(portSpin, &QSpinBox::valueChanged, this, [this](int){ syncConnection(); });
        connect(sourceCombo, &QComboBox::currentTextChanged, this, [this](const QString &s){ settings->setValue("camera/source", s); });

        auto *movement = new QGroupBox("Movimento"); auto *mv = new QGridLayout(movement);
        mv->addWidget(hold("▲", [this]{ client.panTilt(0x03,0x01,ptSpeed(),ptSpeed()); }, [this]{ client.stopPT(); }),0,1);
        mv->addWidget(hold("◀", [this]{ client.panTilt(0x01,0x03,ptSpeed(),ptSpeed()); }, [this]{ client.stopPT(); }),1,0);
        mv->addWidget(button("●", [this]{ client.stopPT(); client.zoomStop(); client.focusStop(); }),1,1);
        mv->addWidget(hold("▶", [this]{ client.panTilt(0x02,0x03,ptSpeed(),ptSpeed()); }, [this]{ client.stopPT(); }),1,2);
        mv->addWidget(hold("▼", [this]{ client.panTilt(0x03,0x02,ptSpeed(),ptSpeed()); }, [this]{ client.stopPT(); }),2,1);
        speedSlider = new QSlider(Qt::Horizontal); speedSlider->setRange(1,18); speedSlider->setValue(8);
        mv->addWidget(new QLabel("Velocidade PT"),3,0); mv->addWidget(speedSlider,3,1,1,2);
        mv->addWidget(hold("ZOOM −", [this]{ client.zoomWide(zoomSpeed()); }, [this]{ client.zoomStop(); }),4,0);
        mv->addWidget(hold("ZOOM +", [this]{ client.zoomTele(zoomSpeed()); }, [this]{ client.zoomStop(); }),4,1,1,2);
        zoomSpeedSlider = new QSlider(Qt::Horizontal); zoomSpeedSlider->setRange(0,7); zoomSpeedSlider->setValue(4);
        mv->addWidget(new QLabel("Velocidade Z"),5,0); mv->addWidget(zoomSpeedSlider,5,1,1,2); root->addWidget(movement);

        auto *focusBox = new QGroupBox("Foco"); auto *fh = new QHBoxLayout(focusBox);
        auto *wheel = new FocusWheel;
        wheel->jog = [this](int value){ if (value > 0) client.focusFar(); else client.focusNear(); QTimer::singleShot(qBound(35,qAbs(value)*2,180),this,[this]{ client.focusStop(); }); };
        auto *fv = new QVBoxLayout; fv->addWidget(button("AUTO FOCUS", [this]{ client.focusAuto(true); })); fv->addWidget(button("MANUAL", [this]{ client.focusAuto(false); }));
        auto *fr = new QHBoxLayout; fr->addWidget(hold("NEAR", [this]{ client.focusNear(); }, [this]{ client.focusStop(); })); fr->addWidget(hold("FAR", [this]{ client.focusFar(); }, [this]{ client.focusStop(); })); fv->addLayout(fr);
        fh->addWidget(wheel); fh->addLayout(fv,1); root->addWidget(focusBox);

        auto *exp = new QGroupBox("Exposição"); auto *eg = new QGridLayout(exp); exposureMode = new QComboBox; exposureMode->addItems({"Auto","Manual"});
        connect(exposureMode, &QComboBox::currentIndexChanged, this, [this](int i){ client.exposureAuto(i==0); });
        eg->addWidget(new QLabel("Modo"),0,0); eg->addWidget(exposureMode,0,1,1,2);
        eg->addWidget(new QLabel("Iris"),1,0); eg->addWidget(button("−", [this]{ client.irisStep(false); }),1,1); eg->addWidget(button("+", [this]{ client.irisStep(true); }),1,2);
        eg->addWidget(new QLabel("Shutter"),2,0); eg->addWidget(button("−", [this]{ client.shutterStep(false); }),2,1); eg->addWidget(button("+", [this]{ client.shutterStep(true); }),2,2);
        eg->addWidget(new QLabel("Gain"),3,0); eg->addWidget(button("−", [this]{ client.gainStep(false); }),3,1); eg->addWidget(button("+", [this]{ client.gainStep(true); }),3,2); root->addWidget(exp);

        auto *trackBox = new QGroupBox("Auto Tracking"); auto *tv = new QVBoxLayout(trackBox);
        tracking = new QToolButton; tracking->setText("TRACKING OFF"); tracking->setCheckable(true); tracking->setMinimumHeight(38);
        connect(tracking, &QToolButton::toggled, this, [this](bool on){ tracking->setText(on?"TRACKING ON":"TRACKING OFF"); client.sendHex(on?trackOnHex->text():trackOffHex->text()); });
        tv->addWidget(tracking);
        auto *advanced = new QWidget; auto *afm = new QFormLayout(advanced); afm->setContentsMargins(0,0,0,0); trackOnHex = new QLineEdit; trackOffHex = new QLineEdit;
        trackOnHex->setPlaceholderText("HEX VISCA do Tracking ON"); trackOffHex->setPlaceholderText("HEX VISCA do Tracking OFF"); afm->addRow("ON HEX",trackOnHex); afm->addRow("OFF HEX",trackOffHex);
        auto *toggleAdv = new QToolButton; toggleAdv->setText("Comando de tracking ▾"); toggleAdv->setCheckable(true); advanced->setVisible(false);
        connect(toggleAdv, &QToolButton::toggled, advanced, &QWidget::setVisible); connect(trackOnHex,&QLineEdit::editingFinished,this,[this]{ settings->setValue("tracking/on",trackOnHex->text()); }); connect(trackOffHex,&QLineEdit::editingFinished,this,[this]{ settings->setValue("tracking/off",trackOffHex->text()); });
        tv->addWidget(toggleAdv); tv->addWidget(advanced); root->addWidget(trackBox);

        auto *presets = new QGroupBox("Presets visuais"); auto *pg = new QGridLayout(presets);
        for (int i=0;i<PRESET_COUNT;++i) {
            auto *card = new QToolButton; card->setToolButtonStyle(Qt::ToolButtonTextUnderIcon); card->setIconSize(QSize(150,84)); card->setMinimumSize(165,118); card->setText(QString("Preset %1").arg(i+1)); card->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(card,&QToolButton::clicked,this,[this,i]{ client.presetRecall(i); }); connect(card,&QToolButton::customContextMenuRequested,this,[this,i](const QPoint&){ savePreset(i); }); presetButtons[i]=card; pg->addWidget(card,i/2,i%2);
        }
        auto *hint = new QLabel("Clique: chamar preset   •   Botão direito: salvar posição + miniatura"); hint->setWordWrap(true); pg->addWidget(hint,PRESET_COUNT/2,0,1,2); root->addWidget(presets); root->addStretch();
    }

    void syncConnection()
    {
        client.host = ipEdit->text().trimmed(); client.port = quint16(portSpin->value()); settings->setValue("camera/ip",client.host); settings->setValue("camera/port",client.port);
        status->setText(QString("● %1:%2").arg(client.host).arg(client.port)); status->setStyleSheet("color:#801fdd;font-weight:700;");
    }
    void loadSettings()
    {
        ipEdit->setText(settings->value("camera/ip","192.168.1.100").toString()); portSpin->setValue(settings->value("camera/port",52381).toInt()); trackOnHex->setText(settings->value("tracking/on","").toString()); trackOffHex->setText(settings->value("tracking/off","").toString());
        for(int i=0;i<PRESET_COUNT;++i){ presetImages[i]=settings->value(QString("preset/%1/image").arg(i)).toString(); updatePresetButton(i);} syncConnection();
    }
    void refreshSources()
    {
        const QString previous = settings->value("camera/source").toString(); sourceCombo->blockSignals(true); sourceCombo->clear();
        obs_enum_sources([](void *data, obs_source_t *source){ auto *combo=static_cast<QComboBox*>(data); if(obs_source_get_output_flags(source)&OBS_SOURCE_VIDEO) combo->addItem(QString::fromUtf8(obs_source_get_name(source))); return true; },sourceCombo);
        int idx=sourceCombo->findText(previous); if(idx>=0) sourceCombo->setCurrentIndex(idx); sourceCombo->blockSignals(false);
    }
    void savePreset(int i)
    {
        client.presetSet(i); pendingPreset=i; QByteArray name=sourceCombo->currentText().toUtf8(); obs_source_t *source=obs_get_source_by_name(name.constData());
        if(source){ obs_frontend_take_source_screenshot(source); obs_source_release(source);} else pendingPreset=-1;
    }
    void updatePresetButton(int i)
    {
        if(!presetButtons[i]) return; const QString path=presetImages[i]; if(!path.isEmpty()&&QFileInfo::exists(path)){ QPixmap p(path); if(!p.isNull()) presetButtons[i]->setIcon(QIcon(p.scaled(150,84,Qt::KeepAspectRatioByExpanding,Qt::SmoothTransformation))); }
    }
};

static ProlensPtzDock *g_dock = nullptr;

static void frontendEvent(enum obs_frontend_event event, void *)
{
    if (event == OBS_FRONTEND_EVENT_SCREENSHOT_TAKEN && g_dock)
        QMetaObject::invokeMethod(g_dock, []{ if(g_dock) g_dock->screenshotReady(); }, Qt::QueuedConnection);
}

bool obs_module_load(void)
{
    g_dock = new ProlensPtzDock;
    if (!obs_frontend_add_dock_by_id(DOCK_ID, "Prolens PTZ Control", g_dock)) { delete g_dock; g_dock=nullptr; return false; }
    obs_frontend_add_event_callback(frontendEvent, nullptr);
    blog(LOG_INFO, "[Prolens PTZ] v0.1 loaded");
    return true;
}

void obs_module_unload(void)
{
    obs_frontend_remove_event_callback(frontendEvent, nullptr);
    obs_frontend_remove_dock(DOCK_ID);
    g_dock = nullptr;
}
