#include <obs-module.h>
#include <util/config-file.h>
#include <util/platform.h>
#include "ptz.h"

#ifdef __cplusplus
extern "C" {
#endif
void prolens_ptz_enhancements_load(void);
void prolens_ptz_enhancements_unload(void);
#ifdef __cplusplus
}
#endif

OBS_DECLARE_MODULE();
OBS_MODULE_AUTHOR("Grant Likely / Prolens");
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-GB");

bool obs_module_load()
{
	blog(LOG_INFO, "Prolens PTZ Control loaded (based on PTZ Controls 0.19.0)");
	ptz_load_devices();
	ptz_load_action_source();
	ptz_load_controls();
	ptz_load_settings();
	prolens_ptz_enhancements_load();
	return true;
}

void obs_module_unload()
{
	prolens_ptz_enhancements_unload();
	ptz_unload_devices();
	blog(LOG_INFO, "Prolens PTZ Control unloaded");
}

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Prolens PTZ Control - PTZ Controls with focus wheel, exposure, tracking and visual presets";
}

MODULE_EXPORT const char *obs_module_name(void)
{
	return "Prolens PTZ Control";
}

struct source_active_cb_data {
	obs_source_t *source;
	bool active;
};

static void source_active_cb(obs_source_t *parent, obs_source_t *child, void *data)
{
	UNUSED_PARAMETER(parent);
	struct source_active_cb_data *cb_data = data;
	if (child == cb_data->source)
		cb_data->active = true;
}

bool ptz_scene_is_source_active(obs_source_t *scene, obs_source_t *source)
{
	struct source_active_cb_data cb_data = {.source = source, .active = false};
	if (scene == source)
		return true;
	obs_source_enum_active_sources(scene, source_active_cb, &cb_data);
	return cb_data.active;
}
