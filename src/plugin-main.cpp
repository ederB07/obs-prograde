#include <obs-module.h>
#include <graphics/matrix4.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-prograde", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
    return "ProGrade 0.8 native-core performance baseline";
}

struct ProGradeFilter {
    obs_source_t *context = nullptr;
    gs_effect_t *effect = nullptr;
    gs_eparam_t *gamma_param = nullptr;
    gs_eparam_t *matrix_param = nullptr;
    float gamma = 1.0f;
    matrix4 final_matrix{};
};

static const char *effect_text = R"(
uniform float4x4 ViewProj;
uniform texture2d image;
uniform float gamma;
uniform float4x4 color_matrix;

sampler_state textureSampler {
    Filter = Linear;
    AddressU = Clamp;
    AddressV = Clamp;
};

struct VertData {
    float4 pos : POSITION;
    float2 uv : TEXCOORD0;
};

VertData VSDefault(VertData v)
{
    VertData o;
    o.pos = mul(float4(v.pos.xyz, 1.0), ViewProj);
    o.uv = v.uv;
    return o;
}

float4 PSColor(VertData v) : TARGET
{
    float4 px = image.Sample(textureSampler, v.uv);
    px.rgb *= (px.a > 0.0) ? (1.0 / px.a) : 0.0;
    px.rgb = pow(px.rgb, float3(gamma, gamma, gamma));
    px = mul(color_matrix, px);
    px.rgb *= px.a;
    return px;
}

technique Draw
{
    pass
    {
        vertex_shader = VSDefault(v);
        pixel_shader = PSColor(v);
    }
}
)";

static const char *filter_name(void *)
{
    return "ProGrade 0.8 NATIVE CORE";
}

static void defaults(obs_data_t *s)
{
    obs_data_set_default_double(s, "gamma", 0.0);
    obs_data_set_default_double(s, "contrast", 0.0);
    obs_data_set_default_double(s, "brightness", 0.0);
    obs_data_set_default_double(s, "saturation", 0.0);
}

static void update(void *data, obs_data_t *s)
{
    auto *f = static_cast<ProGradeFilter *>(data);
    if (!f)
        return;

    double g = obs_data_get_double(s, "gamma");
    g = g < 0.0 ? (-g + 1.0) : (1.0 / (g + 1.0));
    f->gamma = static_cast<float>(g);

    float contrast = static_cast<float>(obs_data_get_double(s, "contrast"));
    contrast = contrast < 0.0f ? (1.0f / (-contrast + 1.0f)) : (contrast + 1.0f);
    const float brightness = static_cast<float>(obs_data_get_double(s, "brightness"));
    const float sat = static_cast<float>(obs_data_get_double(s, "saturation")) + 1.0f;

    constexpr float rw = 0.299f;
    constexpr float gw = 0.587f;
    constexpr float bw = 0.114f;
    const float rs = (1.0f - sat) * rw;
    const float gs = (1.0f - sat) * gw;
    const float bs = (1.0f - sat) * bw;

    matrix4 con;
    matrix4_identity(&con);
    con.x.x = contrast;
    con.y.y = contrast;
    con.z.z = contrast;

    matrix4 bright;
    matrix4_identity(&bright);
    bright.t.x = brightness;
    bright.t.y = brightness;
    bright.t.z = brightness;

    matrix4 sm;
    matrix4_identity(&sm);
    sm.x.x = rs + sat;
    sm.x.y = rs;
    sm.x.z = rs;
    sm.y.x = gs;
    sm.y.y = gs + sat;
    sm.y.z = gs;
    sm.z.x = bs;
    sm.z.y = bs;
    sm.z.z = bs + sat;

    matrix4 temp;
    matrix4_mul(&temp, &bright, &con);
    matrix4_mul(&f->final_matrix, &temp, &sm);
}

static obs_properties_t *properties(void *)
{
    obs_properties_t *p = obs_properties_create();
    obs_properties_add_text(p, "info", "Native-core baseline. Same render strategy as OBS Color Correction.", OBS_TEXT_INFO);
    obs_properties_add_float_slider(p, "gamma", "Gamma", -3.0, 3.0, 0.01);
    obs_properties_add_float_slider(p, "contrast", "Contrast", -4.0, 4.0, 0.01);
    obs_properties_add_float_slider(p, "brightness", "Brightness", -1.0, 1.0, 0.0001);
    obs_properties_add_float_slider(p, "saturation", "Saturation", -1.0, 5.0, 0.01);
    return p;
}

static void *create(obs_data_t *s, obs_source_t *context)
{
    auto *f = new ProGradeFilter;
    f->context = context;
    matrix4_identity(&f->final_matrix);

    obs_enter_graphics();
    f->effect = gs_effect_create(effect_text, "prograde-native-core.effect", nullptr);
    if (f->effect) {
        f->gamma_param = gs_effect_get_param_by_name(f->effect, "gamma");
        f->matrix_param = gs_effect_get_param_by_name(f->effect, "color_matrix");
    }
    obs_leave_graphics();

    if (!f->effect) {
        delete f;
        return nullptr;
    }

    update(f, s);
    return f;
}

static void destroy(void *data)
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

static gs_color_space get_color_space(void *data, size_t, const gs_color_space *)
{
    auto *f = static_cast<ProGradeFilter *>(data);
    const gs_color_space spaces[] = {GS_CS_SRGB, GS_CS_SRGB_16F, GS_CS_709_EXTENDED};
    return f ? obs_source_get_color_space(obs_filter_get_target(f->context), OBS_COUNTOF(spaces), spaces)
             : GS_CS_SRGB;
}

static void render(void *data, gs_effect_t *)
{
    auto *f = static_cast<ProGradeFilter *>(data);
    if (!f || !f->effect) {
        if (f)
            obs_source_skip_video_filter(f->context);
        return;
    }

    const gs_color_space spaces[] = {GS_CS_SRGB, GS_CS_SRGB_16F, GS_CS_709_EXTENDED};
    const gs_color_space space =
        obs_source_get_color_space(obs_filter_get_target(f->context), OBS_COUNTOF(spaces), spaces);

    if (space == GS_CS_709_EXTENDED) {
        obs_source_skip_video_filter(f->context);
        return;
    }

    const gs_color_format format = gs_get_format_from_space(space);
    if (!obs_source_process_filter_begin_with_color_space(f->context, format, space, OBS_ALLOW_DIRECT_RENDERING))
        return;

    gs_effect_set_float(f->gamma_param, f->gamma);
    gs_effect_set_matrix4(f->matrix_param, &f->final_matrix);

    gs_blend_state_push();
    gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
    obs_source_process_filter_end(f->context, f->effect, 0, 0);
    gs_blend_state_pop();
}

bool obs_module_load(void)
{
    obs_source_info info = {};
    info.id = "prograde_filter";
    info.version = 8;
    info.type = OBS_SOURCE_TYPE_FILTER;
    info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB;
    info.get_name = filter_name;
    info.create = create;
    info.destroy = destroy;
    info.update = update;
    info.get_defaults = defaults;
    info.get_properties = properties;
    info.video_render = render;
    info.video_get_color_space = get_color_space;
    obs_register_source(&info);
    return true;
}
