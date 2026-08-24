#include <obs-module.h>
#include <obs-frontend-api.h>
#include <graphics/vec3.h>

#include <QApplication>
#include <QColor>
#include <QDialog>
#include <QFileDialog>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QSlider>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <functional>
#include <string>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-prograde", "en-US")
MODULE_EXPORT const char *obs_module_description(void)
{
    return "ProGrade: native GPU color grading with four wheels and dual LUT";
}

static const char *FILTER_ID = "prograde_filter";

struct ProGradeFilter {
    obs_source_t *context = nullptr;
    obs_source_t *parent = nullptr;
    obs_source_t *lut1 = nullptr;
    obs_source_t *lut2 = nullptr;
    gs_effect_t *effect = nullptr;

    gs_eparam_t *pLift = nullptr;
    gs_eparam_t *pLiftLuma = nullptr;
    gs_eparam_t *pGamma = nullptr;
    gs_eparam_t *pGammaLuma = nullptr;
    gs_eparam_t *pGain = nullptr;
    gs_eparam_t *pGainLuma = nullptr;
    gs_eparam_t *pOffset = nullptr;
    gs_eparam_t *pOffsetLuma = nullptr;
    gs_eparam_t *pSaturation = nullptr;

    QColor lift = QColor(128, 128, 128);
    QColor gamma = QColor(128, 128, 128);
    QColor gain = QColor(128, 128, 128);
    QColor offset = QColor(128, 128, 128);
    double liftLuma = 0.0;
    double gammaLuma = 0.0;
    double gainLuma = 0.0;
    double offsetLuma = 0.0;
    double saturation = 1.0;
    std::string lut1Path;
    std::string lut2Path;
    double lut1Mix = 1.0;
    double lut2Mix = 1.0;
};

static const char *effect_text = R"(
uniform float4x4 ViewProj;
uniform texture2d image;
uniform float3 lift_color;
uniform float lift_luma;
uniform float3 gamma_color;
uniform float gamma_luma;
uniform float3 gain_color;
uniform float gain_luma;
uniform float3 offset_color;
uniform float offset_luma;
uniform float saturation;

sampler_state textureSampler {
    Filter = Linear;
    AddressU = Clamp;
    AddressV = Clamp;
};

struct VertData {
    float4 pos : POSITION;
    float2 uv : TEXCOORD0;
};

VertData VSDefault(VertData v_in)
{
    VertData vert_out;
    vert_out.pos = mul(float4(v_in.pos.xyz, 1.0), ViewProj);
    vert_out.uv = v_in.uv;
    return vert_out;
}

float3 sat_adjust(float3 c, float sat)
{
    float y = dot(c, float3(0.2126, 0.7152, 0.0722));
    return lerp(float3(y, y, y), c, sat);
}

float4 PSProGrade(VertData v_in) : TARGET
{
    float4 px = image.Sample(textureSampler, v_in.uv);
    float3 c = px.rgb;
    float lum = dot(c, float3(0.2126, 0.7152, 0.0722));

    float shadow = 1.0 - smoothstep(0.12, 0.58, lum);
    float high = smoothstep(0.42, 0.92, lum);
    float mid = saturate(1.0 - abs(lum - 0.5) * 2.15);
    mid = smoothstep(0.0, 1.0, mid);

    c += offset_luma * 0.32;
    c += offset_color * 0.32;

    c += shadow * lift_luma * 0.28;
    c += shadow * lift_color * 0.32;

    float g = exp(-gamma_luma * 1.25);
    float3 gc = pow(max(c, 0.00001), float3(g, g, g));
    c = lerp(c, gc, mid);
    c += mid * gamma_color * 0.26;

    c *= 1.0 + high * gain_luma * 0.65;
    c += high * gain_color * 0.30;

    c = sat_adjust(c, saturation);
    return float4(max(c, 0.0), px.a);
}

technique Draw
{
    pass
    {
        vertex_shader = VSDefault(v_in);
        pixel_shader = PSProGrade(v_in);
    }
}
)";

static QColor color_from_obs(obs_data_t *settings, const char *key)
{
    uint32_t c = (uint32_t)obs_data_get_int(settings, key);
    return QColor((int)(c & 0xff), (int)((c >> 8) & 0xff), (int)((c >> 16) & 0xff));
}

static uint32_t color_to_obs(const QColor &c)
{
    return (uint32_t)c.red() | ((uint32_t)c.green() << 8) | ((uint32_t)c.blue() << 16);
}

static void set_bias(gs_eparam_t *param, const QColor &c)
{
    float r = c.redF();
    float g = c.greenF();
    float b = c.blueF();
    float avg = (r + g + b) / 3.0f;
    vec3 v;
    vec3_set(&v, r - avg, g - avg, b - avg);
    gs_effect_set_vec3(param, &v);
}

static void update_lut_filter(obs_source_t *lut, const std::string &path, double mix)
{
    if (!lut)
        return;
    obs_data_t *s = obs_source_get_settings(lut);
    obs_data_set_string(s, "image_path", path.c_str());
    obs_data_set_double(s, "clut_amount", mix);
    obs_source_update(lut, s);
    obs_data_release(s);
}

static void ensure_luts(ProGradeFilter *f)
{
    if (!f || !f->parent)
        return;

    if (!f->lut1) {
        obs_data_t *s = obs_data_create();
        f->lut1 = obs_source_create_private("clut_filter", "[ProGrade] LUT 1", s);
        obs_data_release(s);
        if (f->lut1)
            obs_source_filter_add(f->parent, f->lut1);
    }
    if (!f->lut2) {
        obs_data_t *s = obs_data_create();
        f->lut2 = obs_source_create_private("clut_filter", "[ProGrade] LUT 2", s);
        obs_data_release(s);
        if (f->lut2)
            obs_source_filter_add(f->parent, f->lut2);
    }

    update_lut_filter(f->lut1, f->lut1Path, f->lut1Mix);
    update_lut_filter(f->lut2, f->lut2Path, f->lut2Mix);

    if (f->lut1)
        obs_source_filter_set_order(f->parent, f->lut1, OBS_ORDER_MOVE_BOTTOM);
    if (f->lut2)
        obs_source_filter_set_order(f->parent, f->lut2, OBS_ORDER_MOVE_BOTTOM);
}

class ColorWheel : public QWidget {
public:
    QColor value = QColor(128, 128, 128);
    std::function<void(const QColor &)> changed;

    explicit ColorWheel(QWidget *parent = nullptr) : QWidget(parent)
    {
        setMinimumSize(180, 180);
        setMaximumSize(220, 220);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const int side = std::min(width(), height()) - 12;
        QRectF rect((width() - side) / 2.0, (height() - side) / 2.0, side, side);
        const QPointF c = rect.center();
        const double radius = rect.width() / 2.0;

        QImage wheel((int)rect.width(), (int)rect.height(), QImage::Format_ARGB32_Premultiplied);
        wheel.fill(Qt::transparent);
        for (int y = 0; y < wheel.height(); ++y) {
            for (int x = 0; x < wheel.width(); ++x) {
                double dx = x - wheel.width() / 2.0;
                double dy = y - wheel.height() / 2.0;
                double rr = std::sqrt(dx * dx + dy * dy) / radius;
                if (rr <= 1.0) {
                    double ang = std::atan2(-dy, dx);
                    double hue = std::fmod(ang / (2.0 * M_PI) + 1.0, 1.0);
                    QColor col;
                    col.setHsvF(hue, std::min(1.0, rr), 0.94);
                    wheel.setPixelColor(x, y, col);
                }
            }
        }
        p.drawImage(rect.topLeft(), wheel);
        p.setPen(QPen(QColor(95, 95, 102), 2));
        p.drawEllipse(rect);

        QColor hsv = value.toHsv();
        double hue = hsv.hsvHueF();
        if (hue < 0.0)
            hue = 0.0;
        double sat = hsv.hsvSaturationF();
        double ang = hue * 2.0 * M_PI;
        QPointF marker(c.x() + std::cos(ang) * sat * radius,
                       c.y() - std::sin(ang) * sat * radius);
        p.setBrush(Qt::white);
        p.setPen(QPen(Qt::black, 2));
        p.drawEllipse(marker, 6, 6);
    }

    void mousePressEvent(QMouseEvent *e) override { setFromPoint(e->position()); }
    void mouseMoveEvent(QMouseEvent *e) override
    {
        if (e->buttons() & Qt::LeftButton)
            setFromPoint(e->position());
    }

private:
    void setFromPoint(const QPointF &pt)
    {
        const int side = std::min(width(), height()) - 12;
        QPointF c(width() / 2.0, height() / 2.0);
        double radius = side / 2.0;
        double dx = pt.x() - c.x();
        double dy = c.y() - pt.y();
        double rr = std::min(1.0, std::sqrt(dx * dx + dy * dy) / radius);
        double ang = std::atan2(dy, dx);
        double hue = std::fmod(ang / (2.0 * M_PI) + 1.0, 1.0);
        QColor col;
        col.setHsvF(hue, rr, 0.5);
        value = col;
        update();
        if (changed)
            changed(value);
    }
};

static void push_setting(ProGradeFilter *f, const char *key, double v)
{
    obs_data_t *s = obs_source_get_settings(f->context);
    obs_data_set_double(s, key, v);
    obs_source_update(f->context, s);
    obs_data_release(s);
}

static void push_color(ProGradeFilter *f, const char *key, const QColor &c)
{
    obs_data_t *s = obs_source_get_settings(f->context);
    obs_data_set_int(s, key, (long long)color_to_obs(c));
    obs_source_update(f->context, s);
    obs_data_release(s);
}

static QWidget *wheel_block(ProGradeFilter *f, const QString &title, const char *colorKey,
                            const char *lumaKey, const QColor &initial, double luma)
{
    QWidget *box = new QWidget;
    QVBoxLayout *v = new QVBoxLayout(box);
    v->setContentsMargins(8, 8, 8, 8);
    QLabel *label = new QLabel(title);
    label->setAlignment(Qt::AlignCenter);
    label->setStyleSheet("font-weight:600; font-size:13px;");
    ColorWheel *wheel = new ColorWheel;
    wheel->value = initial;
    wheel->changed = [f, colorKey](const QColor &c) { push_color(f, colorKey, c); };
    QSlider *slider = new QSlider(Qt::Horizontal);
    slider->setRange(-100, 100);
    slider->setValue((int)std::round(luma * 100.0));
    QLabel *l = new QLabel("Luminance");
    l->setAlignment(Qt::AlignCenter);
    QObject::connect(slider, &QSlider::valueChanged, [f, lumaKey](int value) {
        push_setting(f, lumaKey, value / 100.0);
    });
    v->addWidget(label);
    v->addWidget(wheel, 0, Qt::AlignCenter);
    v->addWidget(l);
    v->addWidget(slider);
    return box;
}

static QWidget *lut_block(ProGradeFilter *f, const QString &title, const char *pathKey,
                          const char *mixKey, const std::string &initialPath, double initialMix)
{
    QWidget *box = new QWidget;
    QVBoxLayout *v = new QVBoxLayout(box);
    QLabel *label = new QLabel(title);
    label->setStyleSheet("font-weight:600;");
    QHBoxLayout *row = new QHBoxLayout;
    QLineEdit *edit = new QLineEdit(QString::fromStdString(initialPath));
    QPushButton *browse = new QPushButton("Browse...");
    row->addWidget(edit, 1);
    row->addWidget(browse);
    QSlider *mix = new QSlider(Qt::Horizontal);
    mix->setRange(0, 100);
    mix->setValue((int)std::round(initialMix * 100.0));

    auto commitPath = [f, edit, pathKey]() {
        obs_data_t *s = obs_source_get_settings(f->context);
        obs_data_set_string(s, pathKey, edit->text().toUtf8().constData());
        obs_source_update(f->context, s);
        obs_data_release(s);
        ensure_luts(f);
    };

    QObject::connect(browse, &QPushButton::clicked, [edit, commitPath]() {
        QString path = QFileDialog::getOpenFileName(nullptr, "Choose LUT", QString(),
                                                    "LUT files (*.cube *.png)");
        if (!path.isEmpty()) {
            edit->setText(path);
            commitPath();
        }
    });
    QObject::connect(edit, &QLineEdit::editingFinished, commitPath);
    QObject::connect(mix, &QSlider::valueChanged, [f, mixKey](int value) {
        push_setting(f, mixKey, value / 100.0);
        ensure_luts(f);
    });

    v->addWidget(label);
    v->addLayout(row);
    v->addWidget(new QLabel("Mix"));
    v->addWidget(mix);
    return box;
}

static void show_panel(ProGradeFilter *f)
{
    if (!f)
        return;
    ensure_luts(f);

    QWidget *parent = static_cast<QWidget *>(obs_frontend_get_main_window());
    QDialog dlg(parent);
    dlg.setWindowTitle("ProGrade Color");
    dlg.resize(980, 640);
    dlg.setStyleSheet(
        "QDialog{background:#1e1f22;color:#eeeeee;}"
        "QWidget{color:#eeeeee;}"
        "QLineEdit{background:#2a2b2f;border:1px solid #55565c;padding:6px;border-radius:4px;}"
        "QPushButton{background:#34353a;border:1px solid #5a5b62;padding:7px 12px;border-radius:5px;}"
        "QPushButton:hover{background:#404148;}"
    );

    QVBoxLayout *root = new QVBoxLayout(&dlg);
    QLabel *title = new QLabel("PROGRADE  •  PRIMARY WHEELS");
    title->setStyleSheet("font-weight:700;font-size:17px;");
    root->addWidget(title);

    QGridLayout *grid = new QGridLayout;
    grid->addWidget(wheel_block(f, "LIFT / BLACKS", "lift_color", "lift_luma", f->lift, f->liftLuma), 0, 0);
    grid->addWidget(wheel_block(f, "GAMMA / MIDS", "gamma_color", "gamma_luma", f->gamma, f->gammaLuma), 0, 1);
    grid->addWidget(wheel_block(f, "GAIN / WHITES", "gain_color", "gain_luma", f->gain, f->gainLuma), 0, 2);
    grid->addWidget(wheel_block(f, "OFFSET / GLOBAL", "offset_color", "offset_luma", f->offset, f->offsetLuma), 0, 3);
    root->addLayout(grid);

    QLabel *satLabel = new QLabel("Saturation");
    satLabel->setStyleSheet("font-weight:600;");
    QSlider *sat = new QSlider(Qt::Horizontal);
    sat->setRange(0, 200);
    sat->setValue((int)std::round(f->saturation * 100.0));
    QObject::connect(sat, &QSlider::valueChanged, [f](int value) {
        push_setting(f, "saturation", value / 100.0);
    });
    root->addWidget(satLabel);
    root->addWidget(sat);

    QHBoxLayout *luts = new QHBoxLayout;
    luts->addWidget(lut_block(f, "LUT 1", "lut1_path", "lut1_mix", f->lut1Path, f->lut1Mix));
    luts->addWidget(lut_block(f, "LUT 2", "lut2_path", "lut2_mix", f->lut2Path, f->lut2Mix));
    root->addLayout(luts);

    QHBoxLayout *buttons = new QHBoxLayout;
    QPushButton *reset = new QPushButton("Reset primaries");
    QPushButton *close = new QPushButton("Close");
    buttons->addWidget(reset);
    buttons->addStretch();
    buttons->addWidget(close);
    QObject::connect(reset, &QPushButton::clicked, [f, &dlg]() {
        obs_data_t *s = obs_source_get_settings(f->context);
        const uint32_t neutral = 0x808080;
        obs_data_set_int(s, "lift_color", neutral);
        obs_data_set_int(s, "gamma_color", neutral);
        obs_data_set_int(s, "gain_color", neutral);
        obs_data_set_int(s, "offset_color", neutral);
        obs_data_set_double(s, "lift_luma", 0.0);
        obs_data_set_double(s, "gamma_luma", 0.0);
        obs_data_set_double(s, "gain_luma", 0.0);
        obs_data_set_double(s, "offset_luma", 0.0);
        obs_data_set_double(s, "saturation", 1.0);
        obs_source_update(f->context, s);
        obs_data_release(s);
        dlg.accept();
        show_panel(f);
    });
    QObject::connect(close, &QPushButton::clicked, &dlg, &QDialog::accept);
    root->addLayout(buttons);
    dlg.exec();
}

static const char *filter_name(void *)
{
    return "ProGrade - Color Wheels + Dual LUT";
}

static void filter_update(void *data, obs_data_t *settings)
{
    auto *f = static_cast<ProGradeFilter *>(data);
    if (!f)
        return;
    f->lift = color_from_obs(settings, "lift_color");
    f->gamma = color_from_obs(settings, "gamma_color");
    f->gain = color_from_obs(settings, "gain_color");
    f->offset = color_from_obs(settings, "offset_color");
    f->liftLuma = obs_data_get_double(settings, "lift_luma");
    f->gammaLuma = obs_data_get_double(settings, "gamma_luma");
    f->gainLuma = obs_data_get_double(settings, "gain_luma");
    f->offsetLuma = obs_data_get_double(settings, "offset_luma");
    f->saturation = obs_data_get_double(settings, "saturation");
    f->lut1Path = obs_data_get_string(settings, "lut1_path");
    f->lut2Path = obs_data_get_string(settings, "lut2_path");
    f->lut1Mix = obs_data_get_double(settings, "lut1_mix");
    f->lut2Mix = obs_data_get_double(settings, "lut2_mix");
    if (f->lut1 || f->lut2)
        ensure_luts(f);
}

static void filter_defaults(obs_data_t *settings)
{
    const uint32_t neutral = 0x808080;
    obs_data_set_default_int(settings, "lift_color", neutral);
    obs_data_set_default_int(settings, "gamma_color", neutral);
    obs_data_set_default_int(settings, "gain_color", neutral);
    obs_data_set_default_int(settings, "offset_color", neutral);
    obs_data_set_default_double(settings, "lift_luma", 0.0);
    obs_data_set_default_double(settings, "gamma_luma", 0.0);
    obs_data_set_default_double(settings, "gain_luma", 0.0);
    obs_data_set_default_double(settings, "offset_luma", 0.0);
    obs_data_set_default_double(settings, "saturation", 1.0);
    obs_data_set_default_string(settings, "lut1_path", "");
    obs_data_set_default_string(settings, "lut2_path", "");
    obs_data_set_default_double(settings, "lut1_mix", 1.0);
    obs_data_set_default_double(settings, "lut2_mix", 1.0);
}

static bool open_panel_button(obs_properties_t *, obs_property_t *, void *data)
{
    show_panel(static_cast<ProGradeFilter *>(data));
    return true;
}

static obs_properties_t *filter_properties(void *data)
{
    obs_properties_t *props = obs_properties_create();
    obs_properties_add_text(props, "info",
                            "Open the native ProGrade panel for four real color wheels, saturation and two LUT slots.",
                            OBS_TEXT_INFO);
    obs_properties_add_button(props, "open_panel", "Open ProGrade Color Panel", open_panel_button);
    (void)data;
    return props;
}

static void *filter_create(obs_data_t *settings, obs_source_t *context)
{
    auto *f = new ProGradeFilter;
    f->context = context;
    obs_enter_graphics();
    f->effect = gs_effect_create(effect_text, "prograde.effect", nullptr);
    if (f->effect) {
        f->pLift = gs_effect_get_param_by_name(f->effect, "lift_color");
        f->pLiftLuma = gs_effect_get_param_by_name(f->effect, "lift_luma");
        f->pGamma = gs_effect_get_param_by_name(f->effect, "gamma_color");
        f->pGammaLuma = gs_effect_get_param_by_name(f->effect, "gamma_luma");
        f->pGain = gs_effect_get_param_by_name(f->effect, "gain_color");
        f->pGainLuma = gs_effect_get_param_by_name(f->effect, "gain_luma");
        f->pOffset = gs_effect_get_param_by_name(f->effect, "offset_color");
        f->pOffsetLuma = gs_effect_get_param_by_name(f->effect, "offset_luma");
        f->pSaturation = gs_effect_get_param_by_name(f->effect, "saturation");
    }
    obs_leave_graphics();
    if (!f->effect) {
        delete f;
        return nullptr;
    }
    filter_update(f, settings);
    return f;
}

static void filter_destroy(void *data)
{
    auto *f = static_cast<ProGradeFilter *>(data);
    if (!f)
        return;
    if (f->parent) {
        if (f->lut1)
            obs_source_filter_remove(f->parent, f->lut1);
        if (f->lut2)
            obs_source_filter_remove(f->parent, f->lut2);
    }
    if (f->lut1)
        obs_source_release(f->lut1);
    if (f->lut2)
        obs_source_release(f->lut2);
    if (f->parent)
        obs_source_release(f->parent);
    obs_enter_graphics();
    gs_effect_destroy(f->effect);
    obs_leave_graphics();
    delete f;
}

static void filter_add(void *data, obs_source_t *source)
{
    auto *f = static_cast<ProGradeFilter *>(data);
    if (!f)
        return;
    if (f->parent)
        obs_source_release(f->parent);
    f->parent = obs_source_get_ref(source);
}

static void filter_remove(void *data, obs_source_t *source)
{
    auto *f = static_cast<ProGradeFilter *>(data);
    if (!f)
        return;
    if (f->lut1) {
        obs_source_filter_remove(source, f->lut1);
        obs_source_release(f->lut1);
        f->lut1 = nullptr;
    }
    if (f->lut2) {
        obs_source_filter_remove(source, f->lut2);
        obs_source_release(f->lut2);
        f->lut2 = nullptr;
    }
    if (f->parent) {
        obs_source_release(f->parent);
        f->parent = nullptr;
    }
}

static void filter_render(void *data, gs_effect_t *)
{
    auto *f = static_cast<ProGradeFilter *>(data);
    if (!f || !f->effect) {
        if (f)
            obs_source_skip_video_filter(f->context);
        return;
    }

    if (!obs_source_process_filter_begin(f->context, GS_RGBA, OBS_NO_DIRECT_RENDERING))
        return;

    set_bias(f->pLift, f->lift);
    gs_effect_set_float(f->pLiftLuma, (float)f->liftLuma);
    set_bias(f->pGamma, f->gamma);
    gs_effect_set_float(f->pGammaLuma, (float)f->gammaLuma);
    set_bias(f->pGain, f->gain);
    gs_effect_set_float(f->pGainLuma, (float)f->gainLuma);
    set_bias(f->pOffset, f->offset);
    gs_effect_set_float(f->pOffsetLuma, (float)f->offsetLuma);
    gs_effect_set_float(f->pSaturation, (float)f->saturation);

    obs_source_process_filter_end(f->context, f->effect, 0, 0);
}

bool obs_module_load(void)
{
    obs_source_info info = {};
    info.id = FILTER_ID;
    info.type = OBS_SOURCE_TYPE_FILTER;
    info.output_flags = OBS_SOURCE_VIDEO;
    info.get_name = filter_name;
    info.create = filter_create;
    info.destroy = filter_destroy;
    info.update = filter_update;
    info.get_defaults = filter_defaults;
    info.get_properties = filter_properties;
    info.video_render = filter_render;
    info.filter_add = filter_add;
    info.filter_remove = filter_remove;
    obs_register_source(&info);
    blog(LOG_INFO, "[ProGrade] loaded");
    return true;
}
