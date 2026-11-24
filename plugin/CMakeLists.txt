# SPDX-License-Identifier: AGPL-3.0-only
# Atomic Lounge (formerly Vintage Chonk) VST3 instrument (Windows): the dsp/ engine + the tablet's web panel in a JUCE 8 WebView2.
#   powershell -ExecutionPolicy Bypass -File plugin\build.ps1
cmake_minimum_required(VERSION 3.22)
project(VintageChonk VERSION 1.2.1 LANGUAGES C CXX)   # the version (JucePlugin_VersionString); 1.2.0 = Atomic Lounge (formerly Vintage Chonk): REGOLITH + ORBIT, live input

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
# one runtime for everything (JUCE, the engine, the static WebView2 loader): the static CRT, so the
# .vst3 needs no Visual C++ redistributable
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

get_filename_component(REPO "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(JUCE_DIR "${REPO}/third_party/JUCE")
if(NOT EXISTS "${JUCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "JUCE not found in ${JUCE_DIR} (run plugin/fetch_deps.ps1; see third_party/JUCE.VERSION)")
endif()
# JUCE is used with one local change (plugin/patches/*.patch, applied by plugin/build.ps1 / fetch_deps.ps1):
# the VST3 MIDI-CC emulation parameters are flagged hidden. Refuse an unpatched copy.
file(READ "${JUCE_DIR}/modules/juce_audio_plugin_client/juce_audio_plugin_client_VST3.cpp" _juce_vst3)
string(FIND "${_juce_vst3}" "Vst::ParameterInfo::kIsHidden, Vst::kRootUnitId" _juce_patched)
if(_juce_patched EQUAL -1)
    message(FATAL_ERROR "JUCE in ${JUCE_DIR} is not patched (plugin/patches/juce-8.0.15-vst3-hidden-midi-cc-params.patch); run plugin/build.ps1")
endif()
add_subdirectory("${JUCE_DIR}" JUCE EXCLUDE_FROM_ALL)

# ------------------------------------------------------------------ engine (C, as on the tablet) ---
# every dsp/**/*.c (the chroma *.inc files are included by mfe_fx.c), 16 voices, desktop FX sizes.
# The engine uses GCC/Clang vector extensions (Chroma spring tank) that MSVC's C compiler rejects, and
# CMake will not mix clang-cl and cl in one project, so the C files are compiled by clang-cl through
# custom commands (MSVC ABI, static runtime, no default-lib records) into a static library.
set(MFE_CLANG_CL "" CACHE FILEPATH "clang-cl.exe that compiles the engine's C")
if(NOT EXISTS "${MFE_CLANG_CL}")
    message(FATAL_ERROR "set MFE_CLANG_CL to a clang-cl.exe (plugin/build.ps1 finds the Android NDK's)")
endif()
file(GLOB_RECURSE MFE_SOURCES CONFIGURE_DEPENDS "${REPO}/dsp/*.c")
file(GLOB_RECURSE MFE_HEADERS CONFIGURE_DEPENDS "${REPO}/dsp/*.h" "${REPO}/dsp/*.inc")
set(MFE_OBJS)
foreach(src ${MFE_SOURCES})
    file(RELATIVE_PATH rel "${REPO}/dsp" "${src}")
    string(REPLACE "/" "_" objname "${rel}")
    set(obj "${CMAKE_CURRENT_BINARY_DIR}/mfe_obj/${objname}.obj")
    add_custom_command(OUTPUT "${obj}"
        COMMAND "${MFE_CLANG_CL}" --target=x86_64-pc-windows-msvc /nologo /c /O2 /MT /Zl /W3
                -Wno-comment -Wno-unused-function -DMFE_MAX_VOICES=16 -D_CRT_SECURE_NO_WARNINGS
                "/I${REPO}/dsp/include" "/Fo${obj}" "${src}"
        DEPENDS "${src}" ${MFE_HEADERS}
        COMMENT "clang-cl dsp/${rel}"
        VERBATIM)
    list(APPEND MFE_OBJS "${obj}")
endforeach()
set_source_files_properties(${MFE_OBJS} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
add_library(mfe_dsp STATIC ${MFE_OBJS})
set_target_properties(mfe_dsp PROPERTIES LINKER_LANGUAGE CXX)
target_include_directories(mfe_dsp INTERFACE "${REPO}/dsp/include")
target_compile_definitions(mfe_dsp INTERFACE MFE_MAX_VOICES=16)

# ------------------------------------------------------------------------- panel resources ---
# the panel: panel in the development repository, panel/ in the public source tree
if(EXISTS "${REPO}/panel/index.html")
    set(PANEL_DIR "${REPO}/panel")
else()
    set(PANEL_DIR "${REPO}/panel")
endif()
file(GLOB_RECURSE WWW_FILES CONFIGURE_DEPENDS "${PANEL_DIR}/*.html" "${PANEL_DIR}/*.css" "${PANEL_DIR}/*.js")
# the ORBIT panel (plugin/web/orbit): its own page, served next to the tablet panel's files
file(GLOB ORBIT_WWW CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/web/orbit/*.html" "${CMAKE_CURRENT_SOURCE_DIR}/web/orbit/*.css"
     "${CMAKE_CURRENT_SOURCE_DIR}/web/orbit/*.js")
juce_add_binary_data(mfm1_www NAMESPACE MFM1Www HEADER_NAME MFM1Www.h
    SOURCES ${WWW_FILES} ${ORBIT_WWW} "${CMAKE_CURRENT_SOURCE_DIR}/web/desktop_shim.js")

# WebView2: Microsoft's official SDK (NuGet Microsoft.Web.WebView2, pinned in plugin/fetch_deps.ps1):
# its headers, and its static loader linked in
set(WEBVIEW2_DIR "${REPO}/third_party/webview2")
set(WEBVIEW2_LIB "${WEBVIEW2_DIR}/x64/WebView2LoaderStatic.lib")
if(NOT EXISTS "${WEBVIEW2_LIB}" OR NOT EXISTS "${WEBVIEW2_DIR}/include/WebView2.h")
    message(FATAL_ERROR "missing the WebView2 SDK in ${WEBVIEW2_DIR} (run plugin/fetch_deps.ps1)")
endif()

# -------------------------------------------------------------------------------- plugin ---
juce_add_plugin(VintageChonk
    PRODUCT_NAME "Atomic Lounge"
    COMPANY_NAME "Atomic Lounge"
    COMPANY_COPYRIGHT "Copyright (c) 2026 the Atomic Lounge contributors. AGPL-3.0"
    COMPANY_WEBSITE "https://github.com/quietorbit72/atomic-lounge"   # source code (AGPL-3.0)
    BUNDLE_ID "org.atomiclounge.synth"
    PLUGIN_MANUFACTURER_CODE Cly1
    PLUGIN_CODE Mfm1
    DESCRIPTION "Atomic Lounge: analog-style synth (REGOLITH) with the ORBIT poly-convolution engine and live input"
    FORMATS VST3 Standalone
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    NEEDS_WEB_BROWSER TRUE
    VST3_CATEGORIES Instrument Synth
    VST3_AUTO_MANIFEST FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE)

set(ORBIT_SOURCES src/FxChain.cpp src/OrbitModel.cpp src/OrbitRunner.cpp)
target_sources(VintageChonk PRIVATE
    src/EngineRunner.cpp
    ${ORBIT_SOURCES}
    src/PluginProcessor.cpp
    src/PluginEditor.cpp
    src/WebView2Probe.cpp
    src/PresetStore.cpp)
target_include_directories(VintageChonk PRIVATE src generated "${WEBVIEW2_DIR}/include")
target_compile_definitions(VintageChonk PUBLIC
    JUCE_WEB_BROWSER=1
    JUCE_USE_WIN_WEBVIEW2_WITH_STATIC_LINKING=1
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_REPORT_APP_USAGE=0)
target_link_libraries(VintageChonk
    PRIVATE
        mfe_dsp
        mfm1_www
        "${WEBVIEW2_LIB}"
        juce::juce_audio_utils
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# ---------------------------------------------------------------------------------- tests ---
# host test: loads the built .vst3 like a DAW, plus direct EngineRunner checks
juce_add_console_app(VintageChonkHostTest PRODUCT_NAME "VintageChonkHostTest")
target_sources(VintageChonkHostTest PRIVATE tests/HostTest.cpp src/EngineRunner.cpp src/PresetStore.cpp ${ORBIT_SOURCES})
target_include_directories(VintageChonkHostTest PRIVATE src generated)
target_compile_definitions(VintageChonkHostTest PRIVATE
    JUCE_PLUGINHOST_VST3=1
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_MODAL_LOOPS_PERMITTED=1
    MFM1_VST3_PATH="${CMAKE_CURRENT_SOURCE_DIR}/dist/Atomic Lounge.vst3")
target_link_libraries(VintageChonkHostTest
    PRIVATE
        mfe_dsp
        juce::juce_audio_processors
        juce::juce_audio_utils
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

# development tool: ORBIT factory preset levels and renders (not part of the plugin; built on request)
juce_add_console_app(VintageChonkOrbitTool PRODUCT_NAME "VintageChonkOrbitTool")
target_sources(VintageChonkOrbitTool PRIVATE tools/orbit_tool.cpp src/EngineRunner.cpp src/PresetStore.cpp ${ORBIT_SOURCES})
target_include_directories(VintageChonkOrbitTool PRIVATE src generated)
target_compile_definitions(VintageChonkOrbitTool PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(VintageChonkOrbitTool
    PRIVATE
        mfe_dsp
        juce::juce_audio_formats
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
