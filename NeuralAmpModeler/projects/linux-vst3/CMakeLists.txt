cmake_minimum_required(VERSION 3.25)

set(PROJECT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../..")
set(PLUGIN_ROOT "${PROJECT_ROOT}/NeuralAmpModeler")
set(IPLUG2_ROOT "${PROJECT_ROOT}/iPlug2")
set(VST3_SDK_ROOT "${IPLUG2_ROOT}/Dependencies/IPlug/VST3_SDK")
set(NAM_CORE_ROOT "${PROJECT_ROOT}/NeuralAmpModelerCore")
set(AUDIO_DSP_TOOLS_ROOT "${PROJECT_ROOT}/AudioDSPTools")

file(STRINGS "${PLUGIN_ROOT}/config.h" PLUG_VERSION_LINE REGEX "^#define PLUG_VERSION_STR ")
string(REGEX REPLACE "^#define PLUG_VERSION_STR \"([0-9]+\\.[0-9]+\\.[0-9]+)\".*$" "\\1" PLUG_VERSION "${PLUG_VERSION_LINE}")

project(NAMOnSteroids-Linux VERSION ${PLUG_VERSION})

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SMTG_ENABLE_VST3_HOSTING_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SMTG_ENABLE_VSTGUI_SUPPORT OFF CACHE BOOL "" FORCE)
set(SMTG_CREATE_PLUGIN_LINK OFF CACHE BOOL "" FORCE)
set(SMTG_CREATE_MODULE_INFO OFF CACHE BOOL "" FORCE)
set(SMTG_CXX_STANDARD 20 CACHE STRING "" FORCE)

list(APPEND CMAKE_MODULE_PATH "${VST3_SDK_ROOT}/cmake/modules")
include(SMTG_VST3_SDK)

smtg_setup_platform_toolset()
smtg_setup_symbol_visibility()

set(ROOT "${VST3_SDK_ROOT}")
set(SDK_ROOT "${VST3_SDK_ROOT}")
set(public_sdk_SOURCE_DIR "${SDK_ROOT}/public.sdk")
set(pluginterfaces_SOURCE_DIR "${SDK_ROOT}/pluginterfaces")
set(SDK_IDE_LIBS_FOLDER FOLDER "Libraries")

include_directories("${ROOT}" "${SDK_ROOT}")

smtg_create_lib_base_target()
smtg_create_pluginterfaces_target()
smtg_create_public_sdk_common_target()
smtg_create_public_sdk_target()

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK REQUIRED IMPORTED_TARGET gtk+-3.0)
pkg_check_modules(ALSA REQUIRED IMPORTED_TARGET alsa)

file(GLOB NAM_CORE_SOURCES
  "${NAM_CORE_ROOT}/NAM/*.cpp"
  "${NAM_CORE_ROOT}/NAM/*.c"
  "${NAM_CORE_ROOT}/NAM/*/*.cpp"
)

file(GLOB PNAM_CORE_SOURCES
  "${PROJECT_ROOT}/ParametricCore/NAM/*.cpp"
  "${PROJECT_ROOT}/ParametricCore/NAM/*/*.cpp"
)
set_source_files_properties(${PNAM_CORE_SOURCES} PROPERTIES COMPILE_DEFINITIONS "nam=pnamcore;tanh_bak=pnam_tanh_bak;sigmoid_bak=pnam_sigmoid_bak;silu_bak=pnam_silu_bak")

set(PLUGIN_SOURCES
  "${PLUGIN_ROOT}/NeuralAmpModeler.cpp"
  "${PLUGIN_ROOT}/ToneStack.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/LinuxEditor.cpp"
  "${IPLUG2_ROOT}/IPlug/IPlugAPIBase.cpp"
  "${IPLUG2_ROOT}/IPlug/IPlugParameter.cpp"
  "${IPLUG2_ROOT}/IPlug/IPlugPaths.cpp"
  "${IPLUG2_ROOT}/IPlug/IPlugPluginBase.cpp"
  "${IPLUG2_ROOT}/IPlug/IPlugProcessor.cpp"
  "${IPLUG2_ROOT}/IPlug/IPlugTimer.cpp"
  "${IPLUG2_ROOT}/IPlug/VST3/IPlugVST3.cpp"
  "${IPLUG2_ROOT}/IPlug/VST3/IPlugVST3_Controller.cpp"
  "${IPLUG2_ROOT}/IPlug/VST3/IPlugVST3_ProcessorBase.cpp"
  # The SDK's `sdk` target does not include this implementation, although
  # IPlugVST3_ProcessorBase derives from SingleComponentEffect.
  "${VST3_SDK_ROOT}/public.sdk/source/vst/vstsinglecomponenteffect.cpp"
  "${AUDIO_DSP_TOOLS_ROOT}/dsp/dsp.cpp"
  "${AUDIO_DSP_TOOLS_ROOT}/dsp/ImpulseResponse.cpp"
  "${AUDIO_DSP_TOOLS_ROOT}/dsp/NoiseGate.cpp"
  "${AUDIO_DSP_TOOLS_ROOT}/dsp/RecursiveLinearFilter.cpp"
  "${AUDIO_DSP_TOOLS_ROOT}/dsp/wav.cpp"
  ${NAM_CORE_SOURCES}
  ${PNAM_CORE_SOURCES}
  "${PLUGIN_ROOT}/ParametricModel.cpp"
)

add_library(NAMOnSteroidsCommon OBJECT ${PLUGIN_SOURCES})
set_target_properties(NAMOnSteroidsCommon PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(NAMOnSteroidsCommon PUBLIC sdk Threads::Threads PkgConfig::GTK)

# A multilib -m32 build runs uname on the amd64 host. Name the bundle
# directory from the target ABI and keep the SDK's native strip command.
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
  function(smtg_get_linux_architecture_name)
    set(LINUX_ARCHITECTURE_NAME x86-linux PARENT_SCOPE)
  endfunction()
endif()

smtg_add_vst3plugin(NAMOnSteroids PACKAGE_NAME "NAM On Steroids"
  SOURCES_LIST $<TARGET_OBJECTS:NAMOnSteroidsCommon>)
set_target_properties(NAMOnSteroids PROPERTIES LINKER_LANGUAGE CXX)
target_link_libraries(NAMOnSteroids PRIVATE sdk Threads::Threads PkgConfig::GTK)

add_executable(NAMOnSteroidsStandalone LinuxStandalone.cpp)
set_target_properties(NAMOnSteroidsStandalone PROPERTIES OUTPUT_NAME nam-on-steroids)
target_link_libraries(NAMOnSteroidsStandalone PRIVATE NAMOnSteroidsCommon PkgConfig::ALSA)

target_compile_definitions(NAMOnSteroidsCommon
  PUBLIC
    VST3_API
    NO_IGRAPHICS
    NAM_HEADLESS_LINUX
    NAM_LINUX_GUI
    NAM_ENABLE_A2_FAST
)

target_include_directories(NAMOnSteroidsCommon
  PUBLIC
    "${PLUGIN_ROOT}"
    "${IPLUG2_ROOT}"
    "${IPLUG2_ROOT}/IPlug"
    "${IPLUG2_ROOT}/IPlug/Extras"
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

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS "9.0")
    target_link_libraries(NAMOnSteroids PRIVATE stdc++fs)
  endif()
  # Shared libraries otherwise allow unresolved symbols, which only surface
  # when a host attempts to dlopen the VST3 module.
  target_link_options(NAMOnSteroids PRIVATE "-Wl,-z,defs")
endif()

target_compile_options(NAMOnSteroidsCommon
  PRIVATE
    -Wall
    -Wextra
    -Wno-unused-parameter
    -Wno-missing-field-initializers
    -Wno-deprecated-declarations
    -include cstdlib
    $<$<CONFIG:Release>:-O3>
)

set_target_properties(NAMOnSteroids NAMOnSteroidsCommon NAMOnSteroidsStandalone
  PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    INTERPROCEDURAL_OPTIMIZATION_RELEASE ON
)


if(CMAKE_SIZEOF_VOID_P EQUAL 4)
  target_link_libraries(NAMOnSteroids PRIVATE atomic)
  target_link_libraries(NAMOnSteroidsStandalone PRIVATE atomic)
endif()

enable_testing()
add_test(NAME parametric_chain COMMAND NAMOnSteroidsStandalone --self-test
  "${PROJECT_ROOT}/ParametricCore/tools/test/fixtures/hyperwavenet.nam")
add_test(NAME concat_parametric_chain COMMAND NAMOnSteroidsStandalone --self-test
  "${PROJECT_ROOT}/ParametricCore/tools/test/fixtures/concat_wavenet.nam")


option(NAM_BUILD_LINUX_UI_TEST "Build the X11 VST3 editor smoke-test host" OFF)
if(NAM_BUILD_LINUX_UI_TEST)
  add_executable(NAMLinuxEditorSmoke tests/LinuxEditorSmoke.cpp)
  target_link_libraries(NAMLinuxEditorSmoke PRIVATE sdk PkgConfig::GTK ${CMAKE_DL_LIBS})
endif()
