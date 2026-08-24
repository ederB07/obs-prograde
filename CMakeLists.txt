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

# ProGrade 0.4.1 keeps the v0.4 UI/effects intact and patches only the
# waveform capture path. The old code refused to sample any source that did
# not advertise OBS_SOURCE_ASYNC_VIDEO, which left only the waveform grid.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/plugin-main.cpp" PROGRADE_SOURCE)

string(REPLACE
  "static constexpr uint32_t SCOPE_INTERVAL_FRAMES = 60;"
  "static constexpr uint32_t SCOPE_INTERVAL_FRAMES = 6;"
  PROGRADE_SOURCE "${PROGRADE_SOURCE}")

string(REPLACE
  [=[    if ((obs_source_get_output_flags(f->parent) & OBS_SOURCE_ASYNC_VIDEO) == 0)
        return;

    obs_source_frame *frame = obs_source_get_frame(f->parent);
    if (!frame)
        return;]=]
  [=[    obs_source_t *scopeSource = obs_filter_get_target(f->context);
    if (!scopeSource)
        scopeSource = f->parent;

    obs_source_frame *frame = obs_source_get_frame(scopeSource);
    if (!frame && scopeSource != f->parent) {
        scopeSource = f->parent;
        frame = obs_source_get_frame(scopeSource);
    }
    if (!frame)
        return;]=]
  PROGRADE_SOURCE "${PROGRADE_SOURCE}")

string(REPLACE
  "    obs_source_release_frame(f->parent, frame);"
  "    obs_source_release_frame(scopeSource, frame);"
  PROGRADE_SOURCE "${PROGRADE_SOURCE}")

string(REPLACE
  [=[    case VIDEO_FORMAT_UYVY: {
        const uint8_t *p = frame->data[0] + y * frame->linesize[0] + (x / 2) * 4;
        const uint8_t yy = (x & 1) ? p[3] : p[1];
        yuv_to_rgb(yy, p[0], p[2], frame->full_range, r, g, b);
        return true;
    }
    default:]=]
  [=[    case VIDEO_FORMAT_UYVY: {
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
    default:]=]
  PROGRADE_SOURCE "${PROGRADE_SOURCE}")

string(REPLACE
  "ProGrade: ultra-light realtime GPU color grading with RGB waveform"
  "ProGrade 0.4.1: realtime GPU color grading with fixed RGB waveform"
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
