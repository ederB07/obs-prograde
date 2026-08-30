#include <obs-module.h>
#include <graphics/vec3.h>

#include <atomic>
#include <cstdint>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-prograde", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
    return "ProGrade 0.5 PERF: single-pass direct-render GPU primaries";
}

static const char *FILTER_ID = "prograde_filter";

struct ProGradeFilter {
    obs_source_t *context = nullptr;
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

    std::atomic<uint32_t> lift{0x808080};
    std::atomic<uint32_t> gamma{0x808080};
    std::atomic<uint32_t> gain{0x808080};
    std::atomic<uint32_t> offset{0x808080};
    std::atomic<float> liftLuma{0.0f};
    std::atomic<float> gammaLuma{0.0f};
    std::atomic<float> gainLuma{0.0f};
    std::atomic<float> offsetLuma{0.0f};
    std::atomic<float> saturation{1.0f};
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
    VertData v;
    v.pos = mul(float4(v_in.pos.xyz, 1.0), ViewProj);
    v.uv = v_in.uv;
    return v;
}

float luma709(float3 c)
{
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

float4 PSProGrade(VertData v_in) : TARGET
{
    float4 px = image.Sample(textureSampler, v_in.uv);
    float3 c = px.rgb;
    float lum = luma709(c);

    float shadow = 1.0 - smoothstep(0.12, 0.58, lum);
    float high = smoothstep(0.42, 0.92, lum);
    float mid = smoothstep(0.0, 1.0, saturate(1.0 - abs(lum - 0.5) * 2.15));

    c += offset_luma * 0.32 + offset_color * 0.32;
    c += shadow * (lift_luma * 0.28 + lift_color * 0.32);

    float gg = exp2(-gamma_luma * 1.80337);
    float3 gc = exp2(log2(max(c, 0.00001)) * gg);
    c = lerp(c, gc, mid);
    c += mid * gamma_color * 0.26;

    c *= 1.0 + high * gain_luma * 0.65;
    c += high * gain_color * 0.30;

    float y = luma709(c);
    c = lerp(float3(y, y, y), c, saturation);
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

static void set_bias(gs_eparam_t *param, uint32_t packed)
{
    const float r = static_cast<float>(packed & 0xff) / 255.0f;
    const float g = static_cast<float>((packed >> 8) & 0xff) / 255.0f;
    const float b = static_cast<float>((packed >> 16) & 0xff) / 255.0f;
    const float avg = (r + g + b) / 3.0f;
    vec3 v;
    vec3_set(&v, r - avg, g - avg, b - avg);
    gs_effect_set_vec3(param, &v);
}

static const char *filter_name(void *)
{
    return "ProGrade 0.5 PERF - Direct GPU";
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
}

static void filter_update(void *data, obs_data_t *settings)
{
    auto *f = static_cast<ProGradeFilter *>(data);
    if (!f)
        return;

    f->lift.store(static_cast<uint32_t>(obs_data_get_int(settings, "lift_color")), std::memory_order_relaxed);
    f->gamma.store(static_cast<uint32_t>(obs_data_get_int(settings, "gamma_color")), std::memory_order_relaxed);
    f->gain.store(static_cast<uint32_t>(obs_data_get_int(settings, "gain_color")), std::memory_order_relaxed);
    f->offset.store(static_cast<uint32_t>(obs_data_get_int(settings, "offset_color")), std::memory_order_relaxed);
    f->liftLuma.store(static_cast<float>(obs_data_get_double(settings, "lift_luma")), std::memory_order_relaxed);
    f->gammaLuma.store(static_cast<float>(obs_data_get_double(settings, "gamma_luma")), std::memory_order_relaxed);
    f->gainLuma.store(static_cast<float>(obs_data_get_double(settings, "gain_luma")), std::memory_order_relaxed);
    f->offsetLuma.store(static_cast<float>(obs_data_get_double(settings, "offset_luma")), std::memory_order_relaxed);
    f->saturation.store(static_cast<float>(obs_data_get_double(settings, "saturation")), std::memory_order_relaxed);
}

static obs_properties_t *filter_properties(void *)
{
    obs_properties_t *p = obs_properties_create();
    obs_properties_add_text(p, "perf_info",
                            "PERFORMANCE TEST: single shader pass, direct rendering, no scopes/LUT/Bloom/Halation.",
                            OBS_TEXT_INFO);
    obs_properties_add_color(p, "lift_color", "Lift / Pretos - Cor");
    obs_properties_add_float_slider(p, "lift_luma", "Lift / Pretos - Y", -1.0, 1.0, 0.01);
    obs_properties_add_color(p, "gamma_color", "Gamma / Medios - Cor");
    obs_properties_add_float_slider(p, "gamma_luma", "Gamma / Medios - Y", -1.0, 1.0, 0.01);
    obs_properties_add_color(p, "gain_color", "Gain / Brancos - Cor");
    obs_properties_add_float_slider(p, "gain_luma", "Gain / Brancos - Y", -1.0, 1.0, 0.01);
    obs_properties_add_color(p, "offset_color", "Offset / Geral - Cor");
    obs_properties_add_float_slider(p, "offset_luma", "Offset / Geral - Y", -1.0, 1.0, 0.01);
    obs_properties_add_float_slider(p, "saturation", "Saturacao", 0.0, 2.0, 0.01);
    return p;
}

static void *filter_create(obs_data_t *settings, obs_source_t *context)
{
    auto *f = new ProGradeFilter;
    f->context = context;

    obs_enter_graphics();
    f->effect = gs_effect_create(effect_text, "prograde-perf.effect", nullptr);
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
    obs_enter_graphics();
    if (f->effect)
        gs_effect_destroy(f->effect);
    obs_leave_graphics();
    delete f;
}

static void filter_render(void *data, gs_effect_t *)
{
    auto *f = static_cast<ProGradeFilter *>(data);
    if (!f || !f->effect) {
        if (f)
            obs_source_skip_video_filter(f->context);
        return;
    }

    if (!obs_source_process_filter_begin(f->context, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING))
        return;

    set_bias(f->pLift, f->lift.load(std::memory_order_relaxed));
    gs_effect_set_float(f->pLiftLuma, f->liftLuma.load(std::memory_order_relaxed));
    set_bias(f->pGamma, f->gamma.load(std::memory_order_relaxed));
    gs_effect_set_float(f->pGammaLuma, f->gammaLuma.load(std::memory_order_relaxed));
    set_bias(f->pGain, f->gain.load(std::memory_order_relaxed));
    gs_effect_set_float(f->pGainLuma, f->gainLuma.load(std::memory_order_relaxed));
    set_bias(f->pOffset, f->offset.load(std::memory_order_relaxed));
    gs_effect_set_float(f->pOffsetLuma, f->offsetLuma.load(std::memory_order_relaxed));
    gs_effect_set_float(f->pSaturation, f->saturation.load(std::memory_order_relaxed));

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
    obs_register_source(&info);
    blog(LOG_INFO, "[ProGrade] 0.5 PERF loaded: direct-render single-pass GPU path");
    return true;
}
