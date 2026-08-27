#include <obs-module.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-prograde", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
    return "ProGrade 0.7 DIAG: zero-render passthrough filter";
}

static const char *FILTER_ID = "prograde_filter";

static const char *filter_name(void *)
{
    return "ProGrade 0.7 DIAG - Zero Render";
}

static void *filter_create(obs_data_t *, obs_source_t *context)
{
    return context;
}

static void filter_destroy(void *)
{
}

static obs_properties_t *filter_properties(void *)
{
    obs_properties_t *props = obs_properties_create();
    obs_properties_add_text(props, "info",
                            "DIAGNOSTIC BUILD: no shader, no Qt, no scopes, no LUTs, no effects. video_render only skips the filter.",
                            OBS_TEXT_INFO);
    return props;
}

static void filter_render(void *data, gs_effect_t *)
{
    obs_source_t *context = static_cast<obs_source_t *>(data);
    obs_source_skip_video_filter(context);
}

bool obs_module_load(void)
{
    obs_source_info info = {};
    info.id = FILTER_ID;
    info.type = OBS_SOURCE_TYPE_FILTER;
    info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB;
    info.get_name = filter_name;
    info.create = filter_create;
    info.destroy = filter_destroy;
    info.get_properties = filter_properties;
    info.video_render = filter_render;
    obs_register_source(&info);
    blog(LOG_INFO, "[ProGrade] 0.7 DIAG loaded: zero-render passthrough");
    return true;
}
