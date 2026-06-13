cmake_minimum_required(VERSION 3.25)

set(PROJECT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../..")
set(PLUGIN_ROOT "${PROJECT_ROOT}/NeuralAmpModeler")
set(IPLUG2_ROOT "${PROJECT_ROOT}/iPlug2")
set(VST3_SDK_ROOT "${IPLUG2_ROOT}/Dependencies/IPlug/VST3_SDK")
set(NAM_CORE_ROOT "${PROJECT_ROOT}/NeuralAmpModelerCore")
set(AUDIO_DSP_TOOLS_ROOT "${PROJECT_ROOT}/AudioDSPTools")

file(STRINGS "${PLUGIN_ROOT}/config.h" PLUG_VERSION_LINE REGEX "^#define PLUG_VERSION_STR ")
string(REGEX REPLACE "^#define PLUG_VERSION_STR \"([0-9]+\\.[0-9]+\\.[0-9]+)\".*$" "\\1" PLUG_VERSION "${PLUG_VERSION_LINE}")

project(NAM-Oversampler-Linux VERSION ${PLUG_VERSION})

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SMTG_ENABLE_VST3_HOSTING_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SMTG_ENABLE_VSTGUI_SUPPORT OFF CACHE BOOL "" FORCE)
set(SMTG_CREATE_PLUGIN_LINK OFF CACHE BOOL "" FORCE)
set(SMTG_CXX_STANDARD 20 CACHE STRING "" FORCE)

add_subdirectory("${VST3_SDK_ROOT}" "${CMAKE_BINARY_DIR}/vst3sdk")

file(GLOB NAM_CORE_SOURCES
  "${NAM_CORE_ROOT}/NAM/*.cpp"
  "${NAM_CORE_ROOT}/NAM/*.c"
  "${NAM_CORE_ROOT}/NAM/*/*.cpp"
)

set(PLUGIN_SOURCES
  "${PLUGIN_ROOT}/NeuralAmpModeler.cpp"
  "${IPLUG2_ROOT}/IPlug/IPlugAPIBase.cpp"
  "${IPLUG2_ROOT}/IPlug/IPlugParameter.cpp"
  "${IPLUG2_ROOT}/IPlug/IPlugPaths.cpp"
  "${IPLUG2_ROOT}/IPlug/IPlugPluginBase.cpp"
  "${IPLUG2_ROOT}/IPlug/IPlugProcessor.cpp"
  "${IPLUG2_ROOT}/IPlug/IPlugTimer.cpp"
  "${IPLUG2_ROOT}/IPlug/VST3/IPlugVST3.cpp"
  "${IPLUG2_ROOT}/IPlug/VST3/IPlugVST3_Controller.cpp"
  "${IPLUG2_ROOT}/IPlug/VST3/IPlugVST3_ProcessorBase.cpp"
  "${AUDIO_DSP_TOOLS_ROOT}/dsp/dsp.cpp"
  "${AUDIO_DSP_TOOLS_ROOT}/dsp/ImpulseResponse.cpp"
  "${AUDIO_DSP_TOOLS_ROOT}/dsp/NoiseGate.cpp"
  "${AUDIO_DSP_TOOLS_ROOT}/dsp/RecursiveLinearFilter.cpp"
  "${AUDIO_DSP_TOOLS_ROOT}/dsp/wav.cpp"
  ${NAM_CORE_SOURCES}
)

smtg_add_vst3plugin(NAM-Oversampler PACKAGE_NAME "NAM-Oversampler" SOURCES_LIST ${PLUGIN_SOURCES})

target_compile_definitions(NAM-Oversampler
  PRIVATE
    VST3_API
    NO_IGRAPHICS
    NAM_HEADLESS_LINUX
    NAM_ENABLE_A2_FAST
)

target_include_directories(NAM-Oversampler
  PRIVATE
    "${PLUGIN_ROOT}"
    "${IPLUG2_ROOT}"
    "${IPLUG2_ROOT}/IPlug"
    "${IPLUG2_ROOT}/IPlug/VST3"
    "${IPLUG2_ROOT}/WDL"
    "${VST3_SDK_ROOT}"
    "${VST3_SDK_ROOT}/base/source"
    "${VST3_SDK_ROOT}/pluginterfaces"
    "${VST3_SDK_ROOT}/public.sdk/source/vst"
    "${NAM_CORE_ROOT}"
    "${NAM_CORE_ROOT}/NAM"
    "${NAM_CORE_ROOT}/Dependencies/eigen"
    "${NAM_CORE_ROOT}/Dependencies/nlohmann"
    "${AUDIO_DSP_TOOLS_ROOT}"
    "${AUDIO_DSP_TOOLS_ROOT}/dsp"
    "${AUDIO_DSP_TOOLS_ROOT}/Dependencies/eigen"
)

target_link_libraries(NAM-Oversampler PRIVATE sdk)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_libraries(NAM-Oversampler PRIVATE stdc++fs)
endif()

target_compile_options(NAM-Oversampler
  PRIVATE
    -Wall
    -Wextra
    -Wno-unused-parameter
    -Wno-missing-field-initializers
    -Wno-deprecated-declarations
    $<$<CONFIG:Release>:-O3>
)

set_target_properties(NAM-Oversampler
  PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    INTERPROCEDURAL_OPTIMIZATION_RELEASE ON
)
