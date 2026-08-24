cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)
project(${_name} VERSION ${_version})

include(FetchContent)
include(compilerconfig)
include(defaults)
include(helpers)

FetchContent_Declare(
  obsptz
  GIT_REPOSITORY https://github.com/glikely/obs-ptz.git
  GIT_TAG v0.19.0
  GIT_SHALLOW TRUE
  GIT_SUBMODULES_RECURSE TRUE
  SOURCE_SUBDIR __prolens_download_only__
)
FetchContent_MakeAvailable(obsptz)

file(COPY "${obsptz_SOURCE_DIR}/shared" DESTINATION "${CMAKE_CURRENT_SOURCE_DIR}")

file(READ "${obsptz_SOURCE_DIR}/src/ptz-visca.cpp" PTZ_VISCA_SOURCE)
set(_old_set [=[
void PTZVisca::set(calldata_t *cd)
{
	if (QThread::currentThread() != thread()) {
		ptz_log(LOG_ERROR, "PTZVisca::set(calldata) called from wrong thread; ignored");
		return;
	}
	bool power_on;
	if (calldata_get_bool(cd, "power_on", &power_on))
		send(VISCA_CAM_Power, {power_on});

	long long wb_mode;
	if (calldata_get_int(cd, "wb_mode", &wb_mode))
		send(VISCA_CAM_WB_Mode, {(int)wb_mode});

	bool trigger;
	if (calldata_get_bool(cd, "wb_onepush_trigger", &trigger) && trigger)
		send(VISCA_CAM_WB_OnePushTrigger);

	PTZDevice::set(cd);
}
]=])
set(_new_set [=[
void PTZVisca::set(calldata_t *cd)
{
	if (QThread::currentThread() != thread()) {
		ptz_log(LOG_ERROR, "PTZVisca::set(calldata) called from wrong thread; ignored");
		return;
	}
	bool power_on;
	if (calldata_get_bool(cd, "power_on", &power_on))
		send(VISCA_CAM_Power, {power_on});

	long long wb_mode;
	if (calldata_get_int(cd, "wb_mode", &wb_mode))
		send(VISCA_CAM_WB_Mode, {(int)wb_mode});

	bool trigger;
	if (calldata_get_bool(cd, "wb_onepush_trigger", &trigger) && trigger)
		send(VISCA_CAM_WB_OnePushTrigger);

	long long exposure_mode;
	if (calldata_get_int(cd, "exposure_mode", &exposure_mode)) {
		switch (exposure_mode) {
		case 0: send(VISCA_CAM_AutoExposure_Auto); break;
		case 1: send(VISCA_CAM_AutoExposure_Manual); break;
		case 2: send(VISCA_CAM_AutoExposure_ShutterPriority); break;
		case 3: send(VISCA_CAM_AutoExposure_IrisPriority); break;
		case 4: send(VISCA_CAM_AutoExposure_Bright); break;
		default: break;
		}
	}

	long long iris_delta;
	if (calldata_get_int(cd, "iris_delta", &iris_delta))
		send(iris_delta >= 0 ? VISCA_CAM_Iris_Up : VISCA_CAM_Iris_Down);
	long long shutter_delta;
	if (calldata_get_int(cd, "shutter_delta", &shutter_delta))
		send(shutter_delta >= 0 ? VISCA_CAM_Shutter_Up : VISCA_CAM_Shutter_Down);
	long long gain_delta;
	if (calldata_get_int(cd, "gain_delta", &gain_delta))
		send(gain_delta >= 0 ? VISCA_CAM_Gain_Up : VISCA_CAM_Gain_Down);

	QString raw_hex = calldata_string(cd, "visca_raw_hex");
	if (!raw_hex.isEmpty()) {
		QByteArray packet = QByteArray::fromHex(raw_hex.remove(' ').remove(':').toUtf8());
		if (!packet.isEmpty())
			send_packet(packet);
	}

	PTZDevice::set(cd);
}
]=])
string(FIND "${PTZ_VISCA_SOURCE}" "void PTZVisca::set(calldata_t *cd)" _set_found)
if(_set_found EQUAL -1)
  message(FATAL_ERROR "Unable to locate PTZVisca::set in upstream obs-ptz")
endif()
string(REPLACE "${_old_set}" "${_new_set}" PTZ_VISCA_SOURCE "${PTZ_VISCA_SOURCE}")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/patched")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/patched/ptz-visca.cpp" "${PTZ_VISCA_SOURCE}")

file(COPY "${obsptz_SOURCE_DIR}/data" DESTINATION "${CMAKE_CURRENT_SOURCE_DIR}")

add_library(${CMAKE_PROJECT_NAME} MODULE)
find_package(libobs REQUIRED)
find_package(obs-frontend-api REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Widgets Core Gui Svg Network Xml)

target_link_libraries(
  ${CMAKE_PROJECT_NAME}
  PRIVATE OBS::libobs OBS::obs-frontend-api Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg Qt6::Network Qt6::Xml
)

add_subdirectory("${obsptz_SOURCE_DIR}/shared/properties-view" "${CMAKE_BINARY_DIR}/shared/properties-view")
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::properties-view)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE "${obsptz_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/ptz-module.c
    src/prolens-enhancements.cpp
    src/prolens-enhancements.hpp
    "${obsptz_SOURCE_DIR}/src/ptz-controls.ui"
    "${obsptz_SOURCE_DIR}/src/settings.ui"
    "${obsptz_SOURCE_DIR}/src/ptz-controls.qrc"
    "${obsptz_SOURCE_DIR}/src/ptz-controls.cpp"
    "${obsptz_SOURCE_DIR}/src/ptz-device.cpp"
    "${obsptz_SOURCE_DIR}/src/ptz-list-model.cpp"
    "${obsptz_SOURCE_DIR}/src/ptz-preset-model.cpp"
    "${obsptz_SOURCE_DIR}/src/settings.cpp"
    "${CMAKE_CURRENT_BINARY_DIR}/patched/ptz-visca.cpp"
    "${obsptz_SOURCE_DIR}/src/ptz-visca.hpp"
    "${obsptz_SOURCE_DIR}/src/ptz-visca-udp.cpp"
    "${obsptz_SOURCE_DIR}/src/ptz-visca-tcp.cpp"
    "${obsptz_SOURCE_DIR}/src/protocol-helpers.cpp"
    "${obsptz_SOURCE_DIR}/src/ptz-action-source.c"
    "${obsptz_SOURCE_DIR}/src/circularlistview.cpp"
    "${obsptz_SOURCE_DIR}/src/touch-control.cpp"
    "${obsptz_SOURCE_DIR}/src/ptz-onvif.cpp"
    "${obsptz_SOURCE_DIR}/src/ptz-onvif.hpp"
    "${obsptz_SOURCE_DIR}/src/ptz-usb-cam.cpp"
    "${obsptz_SOURCE_DIR}/src/ptz-usb-cam.hpp"
)

target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE ENABLE_ONVIF ENABLE_USB_CAM PLUGIN_VERSION="0.2.0")
target_compile_features(${CMAKE_PROJECT_NAME} PRIVATE cxx_std_17)
set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON)

if(MSVC)
  target_compile_options(${CMAKE_PROJECT_NAME} PRIVATE /wd4305)
endif()

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
