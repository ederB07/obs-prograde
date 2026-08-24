cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
find_package(obs-frontend-api REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Widgets Core Gui)

target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs OBS::obs-frontend-api Qt6::Core Qt6::Gui Qt6::Widgets)

# Generate the v0.6 source from the full v0.4 UI/features while replacing the
# expensive generic RGBA filter route with the same color-space-aware route
# used by OBS' native Color Correction filter.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/plugin-main.cpp" PROGRADE_SOURCE)

string(REPLACE
  "if (!f->cst) {"
  "if (!f->cst && !f->cstPath.empty()) {"
  PROGRADE_SOURCE "${PROGRADE_SOURCE}")
string(REPLACE
  "if (!f->lut1) {"
  "if (!f->lut1 && !f->lut1Path.empty()) {"
  PROGRADE_SOURCE "${PROGRADE_SOURCE}")
string(REPLACE
  "if (!f->lut2) {"
  "if (!f->lut2 && !f->lut2Path.empty()) {"
  PROGRADE_SOURCE "${PROGRADE_SOURCE}")

string(REPLACE
  "if (!obs_source_process_filter_begin(f->context, GS_RGBA, OBS_NO_DIRECT_RENDERING))\n        return;"
  [=[const enum gs_color_space preferred_spaces[] = {
        GS_CS_SRGB,
        GS_CS_SRGB_16F,
        GS_CS_709_EXTENDED,
    };
    const enum gs_color_space source_space = obs_source_get_color_space(
        obs_filter_get_target(f->context), OBS_COUNTOF(preferred_spaces), preferred_spaces);
    const enum gs_color_format format = gs_get_format_from_space(source_space);

    if (!obs_source_process_filter_begin_with_color_space(
            f->context, format, source_space, OBS_ALLOW_DIRECT_RENDERING))
        return;]=]
  PROGRADE_SOURCE "${PROGRADE_SOURCE}")

string(REPLACE
  "bool obs_module_load(void)"
  [=[static enum gs_color_space filter_get_color_space(void *data, size_t count,
                                                   const enum gs_color_space *preferred_spaces)
{
    UNUSED_PARAMETER(count);
    UNUSED_PARAMETER(preferred_spaces);

    auto *f = static_cast<ProGradeFilter *>(data);
    if (!f)
        return GS_CS_SRGB;

    const enum gs_color_space potential_spaces[] = {
        GS_CS_SRGB,
        GS_CS_SRGB_16F,
        GS_CS_709_EXTENDED,
    };

    return obs_source_get_color_space(obs_filter_get_target(f->context),
                                      OBS_COUNTOF(potential_spaces), potential_spaces);
}

bool obs_module_load(void)]=]
  PROGRADE_SOURCE "${PROGRADE_SOURCE}")

string(REPLACE
  "info.output_flags = OBS_SOURCE_VIDEO;"
  "info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB;"
  PROGRADE_SOURCE "${PROGRADE_SOURCE}")
string(REPLACE
  "info.filter_remove = filter_remove;"
  "info.filter_remove = filter_remove;\n    info.video_get_color_space = filter_get_color_space;"
  PROGRADE_SOURCE "${PROGRADE_SOURCE}")
string(REPLACE
  "ProGrade 0.4: lock-free realtime primaries, CST, film effects, dual LUT and ultra-light RGB waveform."
  "ProGrade 0.6: OBS-native color-space pipeline, realtime primaries, CST, film effects, dual LUT and RGB waveform."
  PROGRADE_SOURCE "${PROGRADE_SOURCE}")
string(REPLACE
  "[ProGrade] 0.4 loaded: lock-free render path + low-rate RGB waveform"
  "[ProGrade] 0.6 loaded: OBS-native color-space render path + full color suite"
  PROGRADE_SOURCE "${PROGRADE_SOURCE}")

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/generated/plugin-main.cpp" "${PROGRADE_SOURCE}")

target_sources(${CMAKE_PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated/plugin-main.cpp")

target_compile_features(${CMAKE_PROJECT_NAME} PRIVATE cxx_std_17)

if(MSVC)
  target_compile_options(${CMAKE_PROJECT_NAME} PRIVATE /wd4305)
endif()

set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
