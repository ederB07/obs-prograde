#include <obs-frontend-api.h>
#include <obs-module.h>
#include <graphics/vec2.h>
#include <graphics/vec3.h>

#include <QColor>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QSlider>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-prograde", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
    return "ProGrade: lightweight realtime GPU color grading with embedded scopes";
}

static const char *FILTER_ID = "prograde_filter";
static constexpr int SCOPE_W = 320;
static constexpr int SCOPE_H = 180;

struct ProGradeFilter {
    obs_source_t *context = nullptr;
    obs_source_t *parent = nullptr;
    obs_source_t *cst = nullptr;
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
    gs_eparam_t *pTexelSize = nullptr;
    gs_eparam_t *pBloomThreshold = nullptr;
    gs_eparam_t *pBloomRadius = nullptr;
    gs_eparam_t *pBloomStrength = nullptr;
    gs_eparam_t *pHalationThreshold = nullptr;
    gs_eparam_t *pHalationRadius = nullptr;
    gs_eparam_t *pHalationStrength = nullptr;

    std::mutex paramMutex;
    QColor lift = QColor(128, 128, 128);
    QColor gamma = QColor(128, 128, 128);
    QColor gain = QColor(128, 128, 128);
    QColor offset = QColor(128, 128, 128);
    double liftLuma = 0.0;
    double gammaLuma = 0.0;
    double gainLuma = 0.0;
    double offsetLuma = 0.0;
    double saturation = 1.0;

    double bloomThreshold = 0.72;
    double bloomRadius = 8.0;
    double bloomStrength = 0.0;
    double halationThreshold = 0.78;
    double halationRadius = 5.0;
    double halationStrength = 0.0;

    std::string cstPath;
    double cstMix = 1.0;
    std::string lut1Path;
    std::string lut2Path;
    double lut1Mix = 1.0;
    double lut2Mix = 1.0;

    std::atomic<bool> scopesEnabled{false};
    uint32_t scopeFrameCounter = 0;
    std::mutex scopeMutex;
    std::vector<uint32_t> scopePixels;
    bool scopeValid = false;
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
uniform float2 texel_size;
uniform float bloom_threshold;
uniform float bloom_radius;
uniform float bloom_strength;
uniform float halation_threshold;
uniform float halation_radius;
uniform float halation_strength;

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

float luma709(float3 c)
{
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

float3 sat_adjust(float3 c, float sat)
{
    float y = luma709(c);
    return lerp(float3(y, y, y), c, sat);
}

float highlight_mask(float3 c, float threshold)
{
    float m = max(c.r, max(c.g, c.b));
    return smoothstep(threshold, min(1.0, threshold + 0.18), m);
}

float3 blur_fast(float2 uv, float radius)
{
    float2 s = texel_size * max(0.5, radius);
    float3 sum = image.Sample(textureSampler, uv).rgb * 0.40;
    sum += image.Sample(textureSampler, uv + s).rgb * 0.15;
    sum += image.Sample(textureSampler, uv - s).rgb * 0.15;
    sum += image.Sample(textureSampler, uv + float2(s.x, -s.y)).rgb * 0.15;
    sum += image.Sample(textureSampler, uv + float2(-s.x, s.y)).rgb * 0.15;
    return sum;
}

float4 PSProGrade(VertData v_in) : TARGET
{
    float4 px = image.Sample(textureSampler, v_in.uv);
    float3 c = px.rgb;
    float lum = luma709(c);

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

    if (bloom_strength > 0.0001) {
        float3 b = blur_fast(v_in.uv, bloom_radius);
        float mask = highlight_mask(b, bloom_threshold);
        float3 glow = b * mask * bloom_strength;
        c = 1.0 - (1.0 - saturate(c)) * (1.0 - saturate(glow));
    }

    if (halation_strength > 0.0001) {
        float3 h = blur_fast(v_in.uv, halation_radius);
        float mask = highlight_mask(h, halation_threshold);
        float redHalo = h.r * mask * halation_strength;
        c += float3(redHalo, redHalo * 0.10, redHalo * 0.025);
    }

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

static inline uint8_t clamp_byte(float v)
{
    return static_cast<uint8_t>(std::clamp(v, 0.0f, 255.0f));
}

static void yuv_to_rgb(uint8_t yv, uint8_t uv, uint8_t vv, bool full, uint8_t &r, uint8_t &g, uint8_t &b)
{
    float y;
    float u;
    float v;
    if (full) {
        y = static_cast<float>(yv);
        u = static_cast<float>(uv) - 128.0f;
        v = static_cast<float>(vv) - 128.0f;
        r = clamp_byte(y + 1.5748f * v);
        g = clamp_byte(y - 0.1873f * u - 0.4681f * v);
        b = clamp_byte(y + 1.8556f * u);
    } else {
        y = 1.16438f * (static_cast<float>(yv) - 16.0f);
        u = static_cast<float>(uv) - 128.0f;
        v = static_cast<float>(vv) - 128.0f;
        r = clamp_byte(y + 1.79274f * v);
        g = clamp_byte(y - 0.21325f * u - 0.53291f * v);
        b = clamp_byte(y + 2.11240f * u);
    }
}

static bool read_frame_rgb(const obs_source_frame *frame, uint32_t x, uint32_t y, uint8_t &r, uint8_t &g,
                           uint8_t &b)
{
    if (!frame || x >= frame->width || y >= frame->height)
        return false;

    switch (frame->format) {
    case VIDEO_FORMAT_BGRA:
    case VIDEO_FORMAT_BGRX: {
        const uint8_t *p = frame->data[0] + y * frame->linesize[0] + x * 4;
        b = p[0];
        g = p[1];
        r = p[2];
        return true;
    }
    case VIDEO_FORMAT_RGBA: {
        const uint8_t *p = frame->data[0] + y * frame->linesize[0] + x * 4;
        r = p[0];
        g = p[1];
        b = p[2];
        return true;
    }
    case VIDEO_FORMAT_NV12: {
        const uint8_t yy = frame->data[0][y * frame->linesize[0] + x];
        const uint8_t *uv = frame->data[1] + (y / 2) * frame->linesize[1] + (x / 2) * 2;
        yuv_to_rgb(yy, uv[0], uv[1], frame->full_range, r, g, b);
        return true;
    }
    case VIDEO_FORMAT_I420: {
        const uint8_t yy = frame->data[0][y * frame->linesize[0] + x];
        const uint8_t uu = frame->data[1][(y / 2) * frame->linesize[1] + (x / 2)];
        const uint8_t vv = frame->data[2][(y / 2) * frame->linesize[2] + (x / 2)];
        yuv_to_rgb(yy, uu, vv, frame->full_range, r, g, b);
        return true;
    }
    case VIDEO_FORMAT_YUY2: {
        const uint8_t *p = frame->data[0] + y * frame->linesize[0] + (x / 2) * 4;
        const uint8_t yy = (x & 1) ? p[2] : p[0];
        yuv_to_rgb(yy, p[1], p[3], frame->full_range, r, g, b);
        return true;
    }
    case VIDEO_FORMAT_UYVY: {
        const uint8_t *p = frame->data[0] + y * frame->linesize[0] + (x / 2) * 4;
        const uint8_t yy = (x & 1) ? p[3] : p[1];
        yuv_to_rgb(yy, p[0], p[2], frame->full_range, r, g, b);
        return true;
    }
    case VIDEO_FORMAT_YVYU: {
        const uint8_t *p = frame->data[0] + y * frame->linesize[0] + (x / 2) * 4;
        const uint8_t yy = (x & 1) ? p[2] : p[0];
        yuv_to_rgb(yy, p[3], p[1], frame->full_range, r, g, b);
        return true;
    }
    default:
        return false;
    }
}

static void capture_scope_frame(ProGradeFilter *f)
{
    if (!f || !f->parent || !f->scopesEnabled.load(std::memory_order_relaxed))
        return;

    const uint32_t flags = obs_source_get_output_flags(f->parent);
    if ((flags & OBS_SOURCE_ASYNC_VIDEO) == 0)
        return;

    obs_source_frame *frame = obs_source_get_frame(f->parent);
    if (!frame)
        return;

    std::vector<uint32_t> pixels(static_cast<size_t>(SCOPE_W) * SCOPE_H);
    bool valid = true;
    for (int sy = 0; sy < SCOPE_H && valid; ++sy) {
        const uint32_t srcY = std::min(frame->height - 1,
                                       static_cast<uint32_t>((static_cast<uint64_t>(sy) * frame->height) / SCOPE_H));
        for (int sx = 0; sx < SCOPE_W; ++sx) {
            const uint32_t srcX = std::min(frame->width - 1,
                                           static_cast<uint32_t>((static_cast<uint64_t>(sx) * frame->width) / SCOPE_W));
            uint8_t r = 0;
            uint8_t g = 0;
            uint8_t b = 0;
            if (!read_frame_rgb(frame, srcX, srcY, r, g, b)) {
                valid = false;
                break;
            }
            pixels[static_cast<size_t>(sy) * SCOPE_W + sx] =
                0xff000000u | (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) |
                static_cast<uint32_t>(b);
        }
    }
    obs_source_release_frame(f->parent, frame);

    if (valid) {
        std::lock_guard<std::mutex> lock(f->scopeMutex);
        f->scopePixels.swap(pixels);
        f->scopeValid = true;
    }
}

static QColor color_from_obs(obs_data_t *settings, const char *key)
{
    uint32_t c = static_cast<uint32_t>(obs_data_get_int(settings, key));
    return QColor(static_cast<int>(c & 0xff), static_cast<int>((c >> 8) & 0xff),
                  static_cast<int>((c >> 16) & 0xff));
}

static uint32_t color_to_obs(const QColor &c)
{
    return static_cast<uint32_t>(c.red()) | (static_cast<uint32_t>(c.green()) << 8) |
           (static_cast<uint32_t>(c.blue()) << 16);
}

static void set_bias(gs_eparam_t *param, const QColor &c)
{
    const float r = static_cast<float>(c.redF());
    const float g = static_cast<float>(c.greenF());
    const float b = static_cast<float>(c.blueF());
    const float avg = (r + g + b) / 3.0f;
    vec3 v;
    vec3_set(&v, r - avg, g - avg, b - avg);
    gs_effect_set_vec3(param, &v);
}

static void update_native_lut(obs_source_t *lut, const std::string &path, double mix)
{
    if (!lut)
        return;
    obs_data_t *settings = obs_source_get_settings(lut);
    obs_data_set_string(settings, "image_path", path.c_str());
    obs_data_set_double(settings, "clut_amount", mix);
    obs_source_update(lut, settings);
    obs_data_release(settings);
}

static obs_source_t *make_native_lut(const char *name)
{
    obs_data_t *settings = obs_data_create();
    obs_source_t *lut = obs_source_create_private("clut_filter", name, settings);
    obs_data_release(settings);
    return lut;
}

static void ensure_aux_filters(ProGradeFilter *f)
{
    if (!f || !f->parent)
        return;

    if (!f->cst) {
        f->cst = make_native_lut("[ProGrade] CST / Camera Transform");
        if (f->cst)
            obs_source_filter_add(f->parent, f->cst);
    }
    if (!f->lut1) {
        f->lut1 = make_native_lut("[ProGrade] Creative LUT 1");
        if (f->lut1)
            obs_source_filter_add(f->parent, f->lut1);
    }
    if (!f->lut2) {
        f->lut2 = make_native_lut("[ProGrade] Creative LUT 2");
        if (f->lut2)
            obs_source_filter_add(f->parent, f->lut2);
    }

    update_native_lut(f->cst, f->cstPath, f->cstMix);
    update_native_lut(f->lut1, f->lut1Path, f->lut1Mix);
    update_native_lut(f->lut2, f->lut2Path, f->lut2Mix);

    if (f->cst)
        obs_source_filter_set_order(f->parent, f->cst, OBS_ORDER_MOVE_TOP);
    if (f->lut1)
        obs_source_filter_set_order(f->parent, f->lut1, OBS_ORDER_MOVE_BOTTOM);
    if (f->lut2)
        obs_source_filter_set_order(f->parent, f->lut2, OBS_ORDER_MOVE_BOTTOM);
}

static void persist_double(ProGradeFilter *f, const char *key, double value)
{
    obs_data_t *settings = obs_source_get_settings(f->context);
    obs_data_set_double(settings, key, value);
    obs_source_update(f->context, settings);
    obs_data_release(settings);
}

static void persist_color(ProGradeFilter *f, const char *key, const QColor &color)
{
    obs_data_t *settings = obs_source_get_settings(f->context);
    obs_data_set_int(settings, key, static_cast<long long>(color_to_obs(color)));
    obs_source_update(f->context, settings);
    obs_data_release(settings);
}

class ColorWheel : public QWidget {
public:
    QColor value = QColor(128, 128, 128);
    std::function<void(const QColor &)> changed;
    std::function<void(const QColor &)> committed;

    explicit ColorWheel(QWidget *parent = nullptr) : QWidget(parent)
    {
        setMinimumSize(176, 176);
        setMaximumSize(210, 210);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const int side = std::min(width(), height()) - 16;
        ensureWheelCache(side);
        QRectF rect((width() - side) / 2.0, (height() - side) / 2.0, side, side);
        const QPointF center = rect.center();
        const double radius = rect.width() / 2.0;

        p.drawImage(rect.topLeft(), wheelCache);
        p.setPen(QPen(QColor(20, 20, 22), 5));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(rect.adjusted(1, 1, -1, -1));
        p.setPen(QPen(QColor(92, 92, 98), 1));
        p.drawLine(QPointF(center.x() - 8, center.y()), QPointF(center.x() + 8, center.y()));
        p.drawLine(QPointF(center.x(), center.y() - 8), QPointF(center.x(), center.y() + 8));

        const QColor hsv = value.toHsv();
        double hue = hsv.hsvHueF();
        if (hue < 0.0)
            hue = 0.0;
        const double sat = hsv.hsvSaturationF();
        const double ang = hue * 2.0 * M_PI;
        const QPointF marker(center.x() + std::cos(ang) * sat * radius,
                             center.y() - std::sin(ang) * sat * radius);
        p.setBrush(QColor(235, 235, 238));
        p.setPen(QPen(QColor(15, 15, 16), 2));
        p.drawEllipse(marker, 6.5, 6.5);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton)
            setFromPoint(event->position());
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (event->buttons() & Qt::LeftButton)
            setFromPoint(event->position());
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && committed)
            committed(value);
    }

private:
    QImage wheelCache;
    int cachedSide = 0;

    void ensureWheelCache(int side)
    {
        if (cachedSide == side && !wheelCache.isNull())
            return;

        cachedSide = side;
        wheelCache = QImage(side, side, QImage::Format_ARGB32_Premultiplied);
        wheelCache.fill(Qt::transparent);
        const double radius = side / 2.0;
        for (int y = 0; y < side; ++y) {
            QRgb *line = reinterpret_cast<QRgb *>(wheelCache.scanLine(y));
            for (int x = 0; x < side; ++x) {
                const double dx = x - side / 2.0;
                const double dy = y - side / 2.0;
                const double rr = std::sqrt(dx * dx + dy * dy) / radius;
                if (rr > 1.0)
                    continue;
                const double ang = std::atan2(-dy, dx);
                const double hue = std::fmod(ang / (2.0 * M_PI) + 1.0, 1.0);
                QColor col;
                col.setHsvF(static_cast<float>(hue), static_cast<float>(std::min(1.0, rr)), 0.90f);
                line[x] = col.rgba();
            }
        }
    }

    void setFromPoint(const QPointF &point)
    {
        const int side = std::min(width(), height()) - 16;
        const QPointF center(width() / 2.0, height() / 2.0);
        const double radius = side / 2.0;
        const double dx = point.x() - center.x();
        const double dy = center.y() - point.y();
        const double rr = std::min(1.0, std::sqrt(dx * dx + dy * dy) / radius);
        const double ang = std::atan2(dy, dx);
        const double hue = std::fmod(ang / (2.0 * M_PI) + 1.0, 1.0);
        QColor color;
        color.setHsvF(static_cast<float>(hue), static_cast<float>(rr), 0.5f);
        value = color;
        update();
        if (changed)
            changed(value);
    }
};

enum class ScopeMode { Preview, Waveform, Vectorscope };

class ScopeWidget : public QWidget {
public:
    ScopeWidget(ProGradeFilter *filter, ScopeMode m, QWidget *parent = nullptr) : QWidget(parent), f(filter), mode(m)
    {
        setMinimumSize(mode == ScopeMode::Waveform ? 340 : 220, 130);
        timer.setInterval(125);
        QObject::connect(&timer, &QTimer::timeout, this, QOverload<>::of(&ScopeWidget::update));
        timer.start();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), QColor(8, 9, 11));
        painter.setRenderHint(QPainter::Antialiasing, false);

        std::vector<uint32_t> pixels;
        bool valid = false;
        {
            std::lock_guard<std::mutex> lock(f->scopeMutex);
            valid = f->scopeValid && f->scopePixels.size() == static_cast<size_t>(SCOPE_W) * SCOPE_H;
            if (valid)
                pixels = f->scopePixels;
        }

        if (!valid) {
            painter.setPen(QColor(125, 127, 135));
            painter.drawText(rect(), Qt::AlignCenter | Qt::TextWordWrap,
                             "Scope preview unavailable for this source format");
            return;
        }

        if (mode == ScopeMode::Preview)
            drawPreview(painter, pixels);
        else if (mode == ScopeMode::Waveform)
            drawWaveform(painter, pixels);
        else
            drawVectorscope(painter, pixels);
    }

private:
    ProGradeFilter *f;
    ScopeMode mode;
    QTimer timer;

    static inline uint8_t red(uint32_t p) { return static_cast<uint8_t>((p >> 16) & 0xff); }
    static inline uint8_t green(uint32_t p) { return static_cast<uint8_t>((p >> 8) & 0xff); }
    static inline uint8_t blue(uint32_t p) { return static_cast<uint8_t>(p & 0xff); }

    void drawPreview(QPainter &p, const std::vector<uint32_t> &pixels)
    {
        QImage image(reinterpret_cast<const uchar *>(pixels.data()), SCOPE_W, SCOPE_H, SCOPE_W * 4,
                     QImage::Format_ARGB32);
        const QRect target = QRect(QPoint(0, 0), size()).adjusted(4, 4, -4, -4);
        const QImage scaled = image.scaled(target.size(), Qt::KeepAspectRatio, Qt::FastTransformation);
        const QPoint pos((width() - scaled.width()) / 2, (height() - scaled.height()) / 2);
        p.drawImage(pos, scaled);
    }

    void drawGrid(QPainter &p)
    {
        p.setPen(QColor(38, 40, 45));
        for (int i = 1; i < 4; ++i) {
            const int y = i * height() / 4;
            p.drawLine(0, y, width(), y);
        }
        p.setPen(QColor(83, 85, 92));
        p.drawRect(rect().adjusted(0, 0, -1, -1));
    }

    void drawWaveform(QPainter &p, const std::vector<uint32_t> &pixels)
    {
        drawGrid(p);
        const int w = std::max(1, width() - 2);
        const int h = std::max(1, height() - 2);
        QImage scope(w, h, QImage::Format_ARGB32);
        scope.fill(Qt::transparent);

        auto addPoint = [&scope, w, h](int x, int y, int channel) {
            if (x < 0 || x >= w || y < 0 || y >= h)
                return;
            QRgb *line = reinterpret_cast<QRgb *>(scope.scanLine(y));
            QColor old = QColor::fromRgba(line[x]);
            int r = old.red();
            int g = old.green();
            int b = old.blue();
            if (channel == 0)
                r = std::min(255, r + 90);
            else if (channel == 1)
                g = std::min(255, g + 90);
            else
                b = std::min(255, b + 90);
            line[x] = qRgba(r, g, b, std::max(150, old.alpha()));
        };

        for (int sy = 0; sy < SCOPE_H; sy += 3) {
            for (int sx = 0; sx < SCOPE_W; sx += 2) {
                const uint32_t px = pixels[static_cast<size_t>(sy) * SCOPE_W + sx];
                const int x = sx * (w - 1) / (SCOPE_W - 1);
                addPoint(x, (255 - red(px)) * (h - 1) / 255, 0);
                addPoint(x, (255 - green(px)) * (h - 1) / 255, 1);
                addPoint(x, (255 - blue(px)) * (h - 1) / 255, 2);
            }
        }
        p.drawImage(1, 1, scope);
    }

    void drawVectorscope(QPainter &p, const std::vector<uint32_t> &pixels)
    {
        const int side = std::min(width(), height()) - 8;
        const QRect scopeRect((width() - side) / 2, (height() - side) / 2, side, side);
        p.setPen(QColor(52, 54, 60));
        p.drawEllipse(scopeRect);
        p.drawLine(scopeRect.center().x(), scopeRect.top(), scopeRect.center().x(), scopeRect.bottom());
        p.drawLine(scopeRect.left(), scopeRect.center().y(), scopeRect.right(), scopeRect.center().y());

        QImage scope(side, side, QImage::Format_ARGB32);
        scope.fill(Qt::transparent);
        for (int sy = 0; sy < SCOPE_H; sy += 3) {
            for (int sx = 0; sx < SCOPE_W; sx += 3) {
                const uint32_t px = pixels[static_cast<size_t>(sy) * SCOPE_W + sx];
                const float rf = red(px) / 255.0f;
                const float gf = green(px) / 255.0f;
                const float bf = blue(px) / 255.0f;
                const float y = 0.2126f * rf + 0.7152f * gf + 0.0722f * bf;
                const float u = (bf - y) * 0.5389f;
                const float v = (rf - y) * 0.6350f;
                const int x = std::clamp(static_cast<int>((0.5f + u) * (side - 1)), 0, side - 1);
                const int yy = std::clamp(static_cast<int>((0.5f - v) * (side - 1)), 0, side - 1);
                QRgb *line = reinterpret_cast<QRgb *>(scope.scanLine(yy));
                QColor old = QColor::fromRgba(line[x]);
                line[x] = qRgba(std::min(255, old.red() + 42), std::min(255, old.green() + 55),
                                std::min(255, old.blue() + 42), 200);
            }
        }
        p.drawImage(scopeRect.topLeft(), scope);
    }
};

static QWidget *scope_card(ProGradeFilter *f, const QString &title, ScopeMode mode)
{
    QWidget *box = new QWidget;
    QVBoxLayout *layout = new QVBoxLayout(box);
    layout->setContentsMargins(4, 4, 4, 4);
    QLabel *label = new QLabel(title);
    label->setAlignment(Qt::AlignCenter);
    label->setStyleSheet("font-weight:700;font-size:10px;color:#aeb0b7;letter-spacing:0.5px;");
    layout->addWidget(label);
    layout->addWidget(new ScopeWidget(f, mode), 1);
    return box;
}

static QWidget *wheel_block(ProGradeFilter *f, const QString &title, const char *colorKey,
                            const char *lumaKey, QColor ProGradeFilter::*colorMember,
                            double ProGradeFilter::*lumaMember)
{
    QWidget *box = new QWidget;
    QVBoxLayout *layout = new QVBoxLayout(box);
    layout->setContentsMargins(5, 5, 5, 5);

    QLabel *label = new QLabel(title);
    label->setAlignment(Qt::AlignCenter);
    label->setStyleSheet("font-weight:700;font-size:12px;letter-spacing:0.5px;");

    ColorWheel *wheel = new ColorWheel;
    {
        std::lock_guard<std::mutex> lock(f->paramMutex);
        wheel->value = f->*colorMember;
    }
    wheel->changed = [f, colorMember](const QColor &color) {
        std::lock_guard<std::mutex> lock(f->paramMutex);
        f->*colorMember = color;
    };
    wheel->committed = [f, colorKey](const QColor &color) { persist_color(f, colorKey, color); };

    QSlider *slider = new QSlider(Qt::Horizontal);
    slider->setRange(-100, 100);
    {
        std::lock_guard<std::mutex> lock(f->paramMutex);
        slider->setValue(static_cast<int>(std::round((f->*lumaMember) * 100.0)));
    }

    QDoubleSpinBox *value = new QDoubleSpinBox;
    value->setDecimals(2);
    value->setRange(-1.0, 1.0);
    value->setSingleStep(0.01);
    value->setButtonSymbols(QAbstractSpinBox::NoButtons);
    value->setMaximumWidth(65);
    value->setValue(slider->value() / 100.0);

    QObject::connect(slider, &QSlider::valueChanged, [f, lumaMember, value](int raw) {
        const double v = raw / 100.0;
        {
            std::lock_guard<std::mutex> lock(f->paramMutex);
            f->*lumaMember = v;
        }
        value->blockSignals(true);
        value->setValue(v);
        value->blockSignals(false);
    });
    QObject::connect(slider, &QSlider::sliderReleased, [f, lumaKey, slider]() {
        persist_double(f, lumaKey, slider->value() / 100.0);
    });
    QObject::connect(value, &QDoubleSpinBox::valueChanged, [f, lumaMember, slider](double v) {
        {
            std::lock_guard<std::mutex> lock(f->paramMutex);
            f->*lumaMember = v;
        }
        slider->blockSignals(true);
        slider->setValue(static_cast<int>(std::round(v * 100.0)));
        slider->blockSignals(false);
    });
    QObject::connect(value, &QDoubleSpinBox::editingFinished, [f, lumaKey, value]() {
        persist_double(f, lumaKey, value->value());
    });

    QHBoxLayout *lumaRow = new QHBoxLayout;
    QLabel *lumaLabel = new QLabel("Y");
    lumaLabel->setStyleSheet("color:#9a9aa0;");
    lumaRow->addWidget(lumaLabel);
    lumaRow->addWidget(slider, 1);
    lumaRow->addWidget(value);

    layout->addWidget(label);
    layout->addWidget(wheel, 0, Qt::AlignCenter);
    layout->addLayout(lumaRow);
    return box;
}

static QWidget *effect_slider(ProGradeFilter *f, const QString &labelText, const char *key,
                              double ProGradeFilter::*member, int minValue, int maxValue, double scale)
{
    QWidget *box = new QWidget;
    QHBoxLayout *row = new QHBoxLayout(box);
    row->setContentsMargins(0, 2, 0, 2);
    QLabel *label = new QLabel(labelText);
    label->setMinimumWidth(115);
    QSlider *slider = new QSlider(Qt::Horizontal);
    slider->setRange(minValue, maxValue);
    {
        std::lock_guard<std::mutex> lock(f->paramMutex);
        slider->setValue(static_cast<int>(std::round((f->*member) * scale)));
    }
    QLabel *number = new QLabel(QString::number(slider->value() / scale, 'f', 2));
    number->setMinimumWidth(48);
    number->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    QObject::connect(slider, &QSlider::valueChanged, [f, member, number, scale](int raw) {
        const double value = raw / scale;
        {
            std::lock_guard<std::mutex> lock(f->paramMutex);
            f->*member = value;
        }
        number->setText(QString::number(value, 'f', 2));
    });
    QObject::connect(slider, &QSlider::sliderReleased, [f, key, slider, scale]() {
        persist_double(f, key, slider->value() / scale);
    });

    row->addWidget(label);
    row->addWidget(slider, 1);
    row->addWidget(number);
    return box;
}

static QWidget *lut_block(ProGradeFilter *f, const QString &title, const char *pathKey,
                          const char *mixKey, std::string ProGradeFilter::*pathMember,
                          double ProGradeFilter::*mixMember)
{
    QWidget *box = new QWidget;
    QVBoxLayout *layout = new QVBoxLayout(box);
    layout->setContentsMargins(6, 6, 6, 6);

    QLabel *label = new QLabel(title);
    label->setStyleSheet("font-weight:700;");
    QHBoxLayout *fileRow = new QHBoxLayout;
    QLineEdit *edit = new QLineEdit;
    {
        std::lock_guard<std::mutex> lock(f->paramMutex);
        edit->setText(QString::fromStdString(f->*pathMember));
    }
    QPushButton *browse = new QPushButton("Browse");
    fileRow->addWidget(edit, 1);
    fileRow->addWidget(browse);

    QSlider *mix = new QSlider(Qt::Horizontal);
    mix->setRange(0, 100);
    {
        std::lock_guard<std::mutex> lock(f->paramMutex);
        mix->setValue(static_cast<int>(std::round((f->*mixMember) * 100.0)));
    }

    auto commitPath = [f, edit, pathKey, pathMember]() {
        const std::string path = edit->text().toUtf8().constData();
        {
            std::lock_guard<std::mutex> lock(f->paramMutex);
            f->*pathMember = path;
        }
        obs_data_t *settings = obs_source_get_settings(f->context);
        obs_data_set_string(settings, pathKey, path.c_str());
        obs_source_update(f->context, settings);
        obs_data_release(settings);
        ensure_aux_filters(f);
    };

    QObject::connect(browse, &QPushButton::clicked, [edit, commitPath]() {
        const QString path = QFileDialog::getOpenFileName(nullptr, "Choose LUT", QString(),
                                                          "LUT files (*.cube *.png);;All files (*.*)");
        if (!path.isEmpty()) {
            edit->setText(path);
            commitPath();
        }
    });
    QObject::connect(edit, &QLineEdit::editingFinished, commitPath);
    QObject::connect(mix, &QSlider::valueChanged, [f, mixMember](int raw) {
        std::lock_guard<std::mutex> lock(f->paramMutex);
        f->*mixMember = raw / 100.0;
    });
    QObject::connect(mix, &QSlider::sliderReleased, [f, mixKey, mix]() {
        persist_double(f, mixKey, mix->value() / 100.0);
        ensure_aux_filters(f);
    });

    QHBoxLayout *mixRow = new QHBoxLayout;
    mixRow->addWidget(new QLabel("Mix"));
    mixRow->addWidget(mix, 1);
    layout->addWidget(label);
    layout->addLayout(fileRow);
    layout->addLayout(mixRow);
    return box;
}

static void show_panel(ProGradeFilter *f)
{
    if (!f)
        return;
    ensure_aux_filters(f);
    f->scopesEnabled.store(true, std::memory_order_relaxed);

    QWidget *parent = static_cast<QWidget *>(obs_frontend_get_main_window());
    QDialog dialog(parent);
    dialog.setWindowTitle("ProGrade Color 0.3");
    dialog.resize(1160, 850);
    dialog.setStyleSheet(
        "QDialog{background:#18191c;color:#eeeeef;}"
        "QWidget{color:#eeeeef;}"
        "QTabWidget::pane{border:1px solid #303136;background:#1d1e22;}"
        "QTabBar::tab{background:#24252a;padding:8px 16px;border:1px solid #33343a;}"
        "QTabBar::tab:selected{background:#35363c;}"
        "QLineEdit,QDoubleSpinBox{background:#24252a;border:1px solid #44454c;padding:5px;border-radius:4px;}"
        "QPushButton{background:#2b2c31;border:1px solid #4a4b52;padding:7px 12px;border-radius:4px;}"
        "QPushButton:hover{background:#393a40;}"
        "QSlider::groove:horizontal{height:4px;background:#3d3e44;border-radius:2px;}"
        "QSlider::handle:horizontal{width:12px;margin:-5px 0;background:#d7d7da;border-radius:6px;}"
    );

    QVBoxLayout *root = new QVBoxLayout(&dialog);
    QHBoxLayout *header = new QHBoxLayout;
    QLabel *title = new QLabel("PROGRADE");
    title->setStyleSheet("font-weight:800;font-size:18px;letter-spacing:1px;");
    QLabel *live = new QLabel("● LIGHTWEIGHT REALTIME");
    live->setStyleSheet("color:#9da0a8;font-size:11px;");
    header->addWidget(title);
    header->addStretch();
    header->addWidget(live);
    root->addLayout(header);

    QTabWidget *tabs = new QTabWidget;
    QWidget *primaries = new QWidget;
    QVBoxLayout *primLayout = new QVBoxLayout(primaries);

    QHBoxLayout *scopeRow = new QHBoxLayout;
    scopeRow->setSpacing(6);
    scopeRow->addWidget(scope_card(f, "SOURCE PREVIEW", ScopeMode::Preview), 3);
    scopeRow->addWidget(scope_card(f, "RGB WAVEFORM", ScopeMode::Waveform), 5);
    scopeRow->addWidget(scope_card(f, "VECTORSCOPE", ScopeMode::Vectorscope), 3);
    primLayout->addLayout(scopeRow, 1);

    QGridLayout *grid = new QGridLayout;
    grid->setHorizontalSpacing(3);
    grid->addWidget(wheel_block(f, "LIFT", "lift_color", "lift_luma", &ProGradeFilter::lift,
                                &ProGradeFilter::liftLuma),
                    0, 0);
    grid->addWidget(wheel_block(f, "GAMMA", "gamma_color", "gamma_luma", &ProGradeFilter::gamma,
                                &ProGradeFilter::gammaLuma),
                    0, 1);
    grid->addWidget(wheel_block(f, "GAIN", "gain_color", "gain_luma", &ProGradeFilter::gain,
                                &ProGradeFilter::gainLuma),
                    0, 2);
    grid->addWidget(wheel_block(f, "OFFSET", "offset_color", "offset_luma", &ProGradeFilter::offset,
                                &ProGradeFilter::offsetLuma),
                    0, 3);
    primLayout->addLayout(grid, 2);
    primLayout->addWidget(
        effect_slider(f, "Saturation", "saturation", &ProGradeFilter::saturation, 0, 200, 100.0));
    tabs->addTab(primaries, "Primaries + Scopes");

    QWidget *film = new QWidget;
    QVBoxLayout *filmLayout = new QVBoxLayout(film);
    QLabel *bloomTitle = new QLabel("BLOOM");
    bloomTitle->setStyleSheet("font-weight:800;font-size:13px;");
    filmLayout->addWidget(bloomTitle);
    filmLayout->addWidget(
        effect_slider(f, "Strength", "bloom_strength", &ProGradeFilter::bloomStrength, 0, 100, 100.0));
    filmLayout->addWidget(effect_slider(f, "Threshold", "bloom_threshold", &ProGradeFilter::bloomThreshold,
                                        0, 100, 100.0));
    filmLayout->addWidget(
        effect_slider(f, "Radius", "bloom_radius", &ProGradeFilter::bloomRadius, 0, 300, 10.0));
    QLabel *halTitle = new QLabel("HALATION");
    halTitle->setStyleSheet("font-weight:800;font-size:13px;margin-top:12px;");
    filmLayout->addWidget(halTitle);
    filmLayout->addWidget(effect_slider(f, "Strength", "halation_strength", &ProGradeFilter::halationStrength,
                                        0, 100, 100.0));
    filmLayout->addWidget(effect_slider(f, "Threshold", "halation_threshold", &ProGradeFilter::halationThreshold,
                                        0, 100, 100.0));
    filmLayout->addWidget(
        effect_slider(f, "Radius", "halation_radius", &ProGradeFilter::halationRadius, 0, 200, 10.0));
    QLabel *filmInfo = new QLabel(
        "Fast 5-tap GPU blur. At zero strength both effects are bypassed entirely by the shader.");
    filmInfo->setWordWrap(true);
    filmInfo->setStyleSheet("color:#9b9ca2;margin-top:12px;");
    filmLayout->addWidget(filmInfo);
    filmLayout->addStretch();
    tabs->addTab(film, "Film Effects");

    QWidget *pipeline = new QWidget;
    QVBoxLayout *pipeLayout = new QVBoxLayout(pipeline);
    QLabel *chain = new QLabel("CST / TECHNICAL TRANSFORM  →  PRIMARIES  →  BLOOM / HALATION  →  LUT 1  →  LUT 2");
    chain->setStyleSheet("font-weight:700;color:#b8b9bf;padding:6px;");
    pipeLayout->addWidget(chain);
    pipeLayout->addWidget(lut_block(f, "CST / CAMERA → REC.709", "cst_path", "cst_mix",
                                    &ProGradeFilter::cstPath, &ProGradeFilter::cstMix));
    QLabel *cstInfo = new QLabel(
        "Technical camera transform only. Creative looks remain in the two slots below.");
    cstInfo->setWordWrap(true);
    cstInfo->setStyleSheet("color:#9b9ca2;padding:0 8px 8px 8px;");
    pipeLayout->addWidget(cstInfo);
    pipeLayout->addWidget(lut_block(f, "CREATIVE LUT 1", "lut1_path", "lut1_mix",
                                    &ProGradeFilter::lut1Path, &ProGradeFilter::lut1Mix));
    pipeLayout->addWidget(lut_block(f, "CREATIVE LUT 2", "lut2_path", "lut2_mix",
                                    &ProGradeFilter::lut2Path, &ProGradeFilter::lut2Mix));
    pipeLayout->addStretch();
    tabs->addTab(pipeline, "CST & LUTs");
    root->addWidget(tabs, 1);

    QHBoxLayout *buttons = new QHBoxLayout;
    QPushButton *reset = new QPushButton("Reset Primaries");
    QPushButton *close = new QPushButton("Close");
    buttons->addWidget(reset);
    buttons->addStretch();
    buttons->addWidget(close);

    QObject::connect(reset, &QPushButton::clicked, [f, &dialog]() {
        obs_data_t *settings = obs_source_get_settings(f->context);
        const uint32_t neutral = 0x808080;
        obs_data_set_int(settings, "lift_color", neutral);
        obs_data_set_int(settings, "gamma_color", neutral);
        obs_data_set_int(settings, "gain_color", neutral);
        obs_data_set_int(settings, "offset_color", neutral);
        obs_data_set_double(settings, "lift_luma", 0.0);
        obs_data_set_double(settings, "gamma_luma", 0.0);
        obs_data_set_double(settings, "gain_luma", 0.0);
        obs_data_set_double(settings, "offset_luma", 0.0);
        obs_data_set_double(settings, "saturation", 1.0);
        obs_source_update(f->context, settings);
        obs_data_release(settings);
        dialog.accept();
        f->scopesEnabled.store(false, std::memory_order_relaxed);
        show_panel(f);
    });
    QObject::connect(close, &QPushButton::clicked, &dialog, &QDialog::accept);
    root->addLayout(buttons);

    dialog.exec();
    f->scopesEnabled.store(false, std::memory_order_relaxed);
}

static const char *filter_name(void *)
{
    return "ProGrade - Realtime Color";
}

static void filter_update(void *data, obs_data_t *settings)
{
    auto *f = static_cast<ProGradeFilter *>(data);
    if (!f)
        return;

    {
        std::lock_guard<std::mutex> lock(f->paramMutex);
        f->lift = color_from_obs(settings, "lift_color");
        f->gamma = color_from_obs(settings, "gamma_color");
        f->gain = color_from_obs(settings, "gain_color");
        f->offset = color_from_obs(settings, "offset_color");
        f->liftLuma = obs_data_get_double(settings, "lift_luma");
        f->gammaLuma = obs_data_get_double(settings, "gamma_luma");
        f->gainLuma = obs_data_get_double(settings, "gain_luma");
        f->offsetLuma = obs_data_get_double(settings, "offset_luma");
        f->saturation = obs_data_get_double(settings, "saturation");
        f->bloomThreshold = obs_data_get_double(settings, "bloom_threshold");
        f->bloomRadius = obs_data_get_double(settings, "bloom_radius");
        f->bloomStrength = obs_data_get_double(settings, "bloom_strength");
        f->halationThreshold = obs_data_get_double(settings, "halation_threshold");
        f->halationRadius = obs_data_get_double(settings, "halation_radius");
        f->halationStrength = obs_data_get_double(settings, "halation_strength");
        f->cstPath = obs_data_get_string(settings, "cst_path");
        f->cstMix = obs_data_get_double(settings, "cst_mix");
        f->lut1Path = obs_data_get_string(settings, "lut1_path");
        f->lut2Path = obs_data_get_string(settings, "lut2_path");
        f->lut1Mix = obs_data_get_double(settings, "lut1_mix");
        f->lut2Mix = obs_data_get_double(settings, "lut2_mix");
    }

    if (f->cst || f->lut1 || f->lut2)
        ensure_aux_filters(f);
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
    obs_data_set_default_double(settings, "bloom_threshold", 0.72);
    obs_data_set_default_double(settings, "bloom_radius", 8.0);
    obs_data_set_default_double(settings, "bloom_strength", 0.0);
    obs_data_set_default_double(settings, "halation_threshold", 0.78);
    obs_data_set_default_double(settings, "halation_radius", 5.0);
    obs_data_set_default_double(settings, "halation_strength", 0.0);
    obs_data_set_default_string(settings, "cst_path", "");
    obs_data_set_default_double(settings, "cst_mix", 1.0);
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

static obs_properties_t *filter_properties(void *)
{
    obs_properties_t *props = obs_properties_create();
    obs_properties_add_text(props, "info",
                            "ProGrade 0.3: cached wheels, lightweight embedded scopes, GPU primaries, CST, bloom, halation and dual LUTs.",
                            OBS_TEXT_INFO);
    obs_properties_add_button(props, "open_panel", "Open ProGrade Color", open_panel_button);
    return props;
}

static void *filter_create(obs_data_t *settings, obs_source_t *context)
{
    auto *f = new ProGradeFilter;
    f->context = context;
    f->scopePixels.reserve(static_cast<size_t>(SCOPE_W) * SCOPE_H);

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
        f->pTexelSize = gs_effect_get_param_by_name(f->effect, "texel_size");
        f->pBloomThreshold = gs_effect_get_param_by_name(f->effect, "bloom_threshold");
        f->pBloomRadius = gs_effect_get_param_by_name(f->effect, "bloom_radius");
        f->pBloomStrength = gs_effect_get_param_by_name(f->effect, "bloom_strength");
        f->pHalationThreshold = gs_effect_get_param_by_name(f->effect, "halation_threshold");
        f->pHalationRadius = gs_effect_get_param_by_name(f->effect, "halation_radius");
        f->pHalationStrength = gs_effect_get_param_by_name(f->effect, "halation_strength");
    }
    obs_leave_graphics();

    if (!f->effect) {
        delete f;
        return nullptr;
    }

    filter_update(f, settings);
    return f;
}

static void release_aux_filter(obs_source_t *parent, obs_source_t *&filter)
{
    if (!filter)
        return;
    if (parent)
        obs_source_filter_remove(parent, filter);
    obs_source_release(filter);
    filter = nullptr;
}

static void filter_destroy(void *data)
{
    auto *f = static_cast<ProGradeFilter *>(data);
    if (!f)
        return;

    f->scopesEnabled.store(false, std::memory_order_relaxed);
    release_aux_filter(f->parent, f->cst);
    release_aux_filter(f->parent, f->lut1);
    release_aux_filter(f->parent, f->lut2);
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

    release_aux_filter(source, f->cst);
    release_aux_filter(source, f->lut1);
    release_aux_filter(source, f->lut2);
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

    if (f->scopesEnabled.load(std::memory_order_relaxed)) {
        ++f->scopeFrameCounter;
        if ((f->scopeFrameCounter & 7u) == 0u)
            capture_scope_frame(f);
    }

    if (!obs_source_process_filter_begin(f->context, GS_RGBA, OBS_NO_DIRECT_RENDERING))
        return;

    QColor lift;
    QColor gamma;
    QColor gain;
    QColor offset;
    double liftLuma;
    double gammaLuma;
    double gainLuma;
    double offsetLuma;
    double saturation;
    double bloomThreshold;
    double bloomRadius;
    double bloomStrength;
    double halationThreshold;
    double halationRadius;
    double halationStrength;

    {
        std::lock_guard<std::mutex> lock(f->paramMutex);
        lift = f->lift;
        gamma = f->gamma;
        gain = f->gain;
        offset = f->offset;
        liftLuma = f->liftLuma;
        gammaLuma = f->gammaLuma;
        gainLuma = f->gainLuma;
        offsetLuma = f->offsetLuma;
        saturation = f->saturation;
        bloomThreshold = f->bloomThreshold;
        bloomRadius = f->bloomRadius;
        bloomStrength = f->bloomStrength;
        halationThreshold = f->halationThreshold;
        halationRadius = f->halationRadius;
        halationStrength = f->halationStrength;
    }

    set_bias(f->pLift, lift);
    gs_effect_set_float(f->pLiftLuma, static_cast<float>(liftLuma));
    set_bias(f->pGamma, gamma);
    gs_effect_set_float(f->pGammaLuma, static_cast<float>(gammaLuma));
    set_bias(f->pGain, gain);
    gs_effect_set_float(f->pGainLuma, static_cast<float>(gainLuma));
    set_bias(f->pOffset, offset);
    gs_effect_set_float(f->pOffsetLuma, static_cast<float>(offsetLuma));
    gs_effect_set_float(f->pSaturation, static_cast<float>(saturation));

    const uint32_t width = f->parent ? obs_source_get_base_width(f->parent) : 1920;
    const uint32_t height = f->parent ? obs_source_get_base_height(f->parent) : 1080;
    vec2 texel;
    vec2_set(&texel, width > 0 ? 1.0f / static_cast<float>(width) : 0.0f,
             height > 0 ? 1.0f / static_cast<float>(height) : 0.0f);
    gs_effect_set_vec2(f->pTexelSize, &texel);
    gs_effect_set_float(f->pBloomThreshold, static_cast<float>(bloomThreshold));
    gs_effect_set_float(f->pBloomRadius, static_cast<float>(bloomRadius));
    gs_effect_set_float(f->pBloomStrength, static_cast<float>(bloomStrength));
    gs_effect_set_float(f->pHalationThreshold, static_cast<float>(halationThreshold));
    gs_effect_set_float(f->pHalationRadius, static_cast<float>(halationRadius));
    gs_effect_set_float(f->pHalationStrength, static_cast<float>(halationStrength));

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
    blog(LOG_INFO, "[ProGrade] 0.3 loaded: cached wheels, embedded scopes, optimized GPU effects");
    return true;
}
