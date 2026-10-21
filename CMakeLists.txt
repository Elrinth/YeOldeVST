cmake_minimum_required(VERSION 3.21)
project(YeOldeVST VERSION 1.1.0 LANGUAGES C CXX)

# Windows hosts Windows VST2 DLLs (32-bit ones through a helper), Linux hosts Linux VST2 .so
# plugins. macOS support is prepared in the platform layer but not finished.
if(APPLE)
  message(WARNING "macOS support is incomplete: the GUI has no Cocoa backend yet.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(MSVC)
  # Ship without a VC runtime dependency.
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  add_compile_definitions(UNICODE _UNICODE NOMINMAX _CRT_SECURE_NO_WARNINGS)
  add_compile_options(/utf-8 /W3 /MP)
else()
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
  set(CMAKE_C_VISIBILITY_PRESET hidden)
  set(CMAKE_CXX_VISIBILITY_PRESET hidden)
  set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
  if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
  endif()
endif()

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(YOV_BITS 64)
else()
  set(YOV_BITS 32)
endif()

# The plugin itself is 64-bit only. A 32-bit configure builds just the 32-bit helper exe.
if(YOV_BITS EQUAL 64)
  option(YOV_BUILD_PLUGIN "Build the CLAP/VST3 plugin" ON)
else()
  set(YOV_BUILD_PLUGIN OFF)
endif()

# The Dear ImGui GUI: the only GUI on Linux / macOS; on Windows an alternative to the native one.
if(WIN32)
  option(YOV_IMGUI "Also build the Dear ImGui GUI (YEOLDEVST_GUI=imgui selects it)" OFF)
else()
  set(YOV_IMGUI ON)
endif()

if(NOT WIN32 AND NOT APPLE)
  find_package(X11 REQUIRED)
  find_package(Threads REQUIRED)
endif()

# Warnings for our own targets (not the SDKs).
function(yov_warnings target)
  if(NOT MSVC)
    target_compile_options(${target} PRIVATE -Wall -Wno-unused-parameter)
  endif()
endfunction()

# --- platform layer -----------------------------------------------------------------------------
if(WIN32)
  set(YOV_PLATFORM_SOURCES
    src/platform/win/DynLibWin.cpp
    src/platform/win/IpcWin.cpp
    src/platform/win/ProcessWin.cpp
    src/platform/win/SehWin.cpp
    src/platform/win/UtilWin.cpp
  )
  set(YOV_EMBEDDED_SOURCE src/platform/win/EmbeddedWin.cpp)
  set(YOV_HELPER_UI_SOURCE src/platform/win/HelperWin.cpp)
else()
  set(YOV_PLATFORM_SOURCES
    src/platform/posix/DynLibPosix.cpp
    src/platform/posix/IpcPosix.cpp
    src/platform/posix/ProcessPosix.cpp
    src/platform/posix/SehPosix.cpp
    src/platform/posix/UtilPosix.cpp
  )
  set(YOV_EMBEDDED_SOURCE src/platform/posix/EmbeddedPosix.cpp)
  if(APPLE)
    set(YOV_HELPER_UI_SOURCE src/platform/posix/HelperPosix.cpp src/platform/posix/HelperNoUi.cpp)
  else()
    set(YOV_HELPER_UI_SOURCE src/platform/posix/HelperPosix.cpp src/platform/x11/HelperX11.cpp)
  endif()
endif()

# --- core: shared by the plugin and the helper exes ---------------------------------------------
add_library(yov_core STATIC
  src/common/Json.cpp
  src/common/ModuleInfo.cpp
  src/common/PluginDb.cpp
  src/common/Util.cpp
  src/host/Vst2Plugin.cpp
  src/scan/Scanner.cpp
  ${YOV_PLATFORM_SOURCES}
)
target_include_directories(yov_core PUBLIC src)
yov_warnings(yov_core)
if(WIN32)
  target_link_libraries(yov_core PUBLIC shell32 ole32 user32 advapi32)
else()
  target_link_libraries(yov_core PUBLIC ${CMAKE_DL_LIBS} Threads::Threads)
  if(NOT APPLE)
    target_link_libraries(yov_core PUBLIC rt)
  endif()
endif()

# --- helper exe: scanner probe + out-of-process bridge ----------------------------------------
add_executable(yov_host
  src/hostexe/main.cpp
  src/hostexe/Probe.cpp
  src/hostexe/Bridge.cpp
  ${YOV_HELPER_UI_SOURCE}
  ${YOV_EMBEDDED_SOURCE}
)
target_link_libraries(yov_host PRIVATE yov_core)
yov_warnings(yov_host)
set_target_properties(yov_host PROPERTIES OUTPUT_NAME "YeOldeVST-Host${YOV_BITS}")
if(WIN32)
  target_link_libraries(yov_host PRIVATE gdi32 comctl32)
elseif(NOT APPLE)
  target_link_libraries(yov_host PRIVATE X11::X11)
endif()
if(MSVC)
  # Many old plugins assume a big main-thread stack.
  target_link_options(yov_host PRIVATE /STACK:8388608)
  if(YOV_BITS EQUAL 32)
    # Old plugins (ATL/WTL window thunks, self-modifying protection code) execute code from data
    # memory, which DEP forbids. 32-bit hosts of their era ran without DEP; so does this helper.
    target_link_options(yov_host PRIVATE /NXCOMPAT:NO)
  endif()
endif()

# Test VST2 plugin (MIDI echo + 6 outputs), built for both architectures.
add_library(yov_vst2_test MODULE tests/vst2_test_plugin.cpp)
target_include_directories(yov_vst2_test PRIVATE src)
set_target_properties(yov_vst2_test PROPERTIES OUTPUT_NAME "YovTest${YOV_BITS}" PREFIX "")
if(NOT WIN32 AND NOT APPLE)
  # Linux: the test plugin has a small X11 editor.
  target_link_libraries(yov_vst2_test PRIVATE X11::X11)
  set_target_properties(yov_vst2_test PROPERTIES C_VISIBILITY_PRESET default CXX_VISIBILITY_PRESET default)
endif()

if(NOT YOV_BUILD_PLUGIN)
  return()
endif()

# --- 32-bit helper (Windows), built as a sub-project for Win32 ---------------------------------
if(WIN32)
  include(ExternalProject)
  set(YOV_HOST32_DIR ${CMAKE_BINARY_DIR}/host32)
  ExternalProject_Add(yov_host32
    SOURCE_DIR ${CMAKE_SOURCE_DIR}
    BINARY_DIR ${YOV_HOST32_DIR}
    CMAKE_GENERATOR ${CMAKE_GENERATOR}
    CMAKE_GENERATOR_PLATFORM Win32
    CMAKE_ARGS -DYOV_BUILD_PLUGIN=OFF
    BUILD_COMMAND ${CMAKE_COMMAND} --build ${YOV_HOST32_DIR} --config $<CONFIG> --target yov_host yov_vst2_test
    INSTALL_COMMAND ""
    BUILD_ALWAYS TRUE
  )
  set(YOV_HOST32_EXE ${YOV_HOST32_DIR}/$<CONFIG>/YeOldeVST-Host32.exe)
endif()

# --- the plugin ---------------------------------------------------------------------------------
set(CLAP_SDK_ROOT ${CMAKE_SOURCE_DIR}/external/clap)
set(VST3_SDK_ROOT ${CMAKE_SOURCE_DIR}/external/vst3sdk)
set(CLAP_WRAPPER_OUTPUT_NAME "YeOldeVST")
set(CLAP_WRAPPER_DONT_ADD_TARGETS TRUE)
add_subdirectory(external/clap-wrapper EXCLUDE_FROM_ALL)

# The helper exes are embedded in the plugin binary so it can be installed as a single file.
if(WIN32)
  file(GENERATE OUTPUT ${CMAKE_BINARY_DIR}/embed_$<CONFIG>.rc CONTENT
"201 RCDATA \"$<TARGET_FILE:yov_host>\"
202 RCDATA \"${YOV_HOST32_EXE}\"
")
  set(YOV_EMBED_FILE ${CMAKE_BINARY_DIR}/embed_$<CONFIG>.rc)
  set(YOV_EMBED_DEPENDS "$<TARGET_FILE:yov_host>;${YOV_HOST32_EXE}")
else()
  enable_language(ASM)
  file(GENERATE OUTPUT ${CMAKE_BINARY_DIR}/embed_helper_$<CONFIG>.S CONTENT
"  .section .rodata
  .global yov_helper_start
  .global yov_helper_end
  .hidden yov_helper_start
  .hidden yov_helper_end
  .balign 16
yov_helper_start:
  .incbin \"$<TARGET_FILE:yov_host>\"
yov_helper_end:
  .section .note.GNU-stack,\"\",@progbits
")
  set(YOV_EMBED_FILE ${CMAKE_BINARY_DIR}/embed_helper_$<CONFIG>.S)
  set(YOV_EMBED_DEPENDS "$<TARGET_FILE:yov_host>")
endif()

add_library(yov_plugin_impl STATIC
  src/plugin/Plugin.cpp
  src/plugin/Gui.cpp
  src/plugin/HostedPlugin.cpp
  src/plugin/BridgeClient.cpp
)
target_link_libraries(yov_plugin_impl PUBLIC yov_core clap)
yov_warnings(yov_plugin_impl)
if(WIN32)
  target_sources(yov_plugin_impl PRIVATE src/plugin/win/Win32Gui.cpp)
  target_link_libraries(yov_plugin_impl PUBLIC comctl32 gdi32 uxtheme dwmapi shlwapi)
endif()

foreach(fmt clap vst3)
  add_library(yov_${fmt} MODULE
    src/plugin/entry.cpp
    ${YOV_EMBEDDED_SOURCE}
    ${YOV_EMBED_FILE}
  )
  if(WIN32)
    target_sources(yov_${fmt} PRIVATE src/plugin/plugin.manifest)
    add_dependencies(yov_${fmt} yov_host yov_host32)
  else()
    target_compile_definitions(yov_${fmt} PRIVATE YOV_EMBED_HELPER)
    add_dependencies(yov_${fmt} yov_host)
  endif()
  target_link_libraries(yov_${fmt} PRIVATE yov_plugin_impl)
  set_source_files_properties(${YOV_EMBED_FILE} PROPERTIES OBJECT_DEPENDS "${YOV_EMBED_DEPENDS}")
endforeach()

set_target_properties(yov_clap PROPERTIES OUTPUT_NAME "YeOldeVST" SUFFIX ".clap" PREFIX "")
if(WIN32)
  target_add_vst3_wrapper(TARGET yov_vst3 OUTPUT_NAME "YeOldeVST" WINDOWS_FOLDER_VST3 TRUE)
else()
  target_add_vst3_wrapper(TARGET yov_vst3 OUTPUT_NAME "YeOldeVST")
endif()
# clap-wrapper's sources are C++17 (u8string / fmt pointer formatting changed in C++20).
set_target_properties(yov_vst3-clap-wrapper-vst3-lib PROPERTIES CXX_STANDARD 17)
if(NOT MSVC AND TARGET clap-wrapper-compile-options)
  # It builds with -Werror; this VST3 SDK deprecates functions it still calls.
  get_target_property(_cw_opts clap-wrapper-compile-options INTERFACE_COMPILE_OPTIONS)
  list(REMOVE_ITEM _cw_opts -Werror)
  set_target_properties(clap-wrapper-compile-options PROPERTIES INTERFACE_COMPILE_OPTIONS "${_cw_opts}")
endif()
if(NOT WIN32 AND NOT APPLE AND TARGET base-sdk-vst3)
  # clap-wrapper leaves the SDK's timer out on Linux (it did not build in older SDKs), but this
  # SDK's plugin factory calls into it there.
  target_sources(base-sdk-vst3 PRIVATE ${VST3_SDK_ROOT}/base/source/timer.cpp)
endif()

# --- tests ------------------------------------------------------------------------------------
add_executable(yov_clap_test tests/clap_test.cpp)
target_link_libraries(yov_clap_test PRIVATE clap)
if(WIN32)
  target_link_libraries(yov_clap_test PRIVATE user32 gdi32 shell32)
else()
  target_link_libraries(yov_clap_test PRIVATE ${CMAKE_DL_LIBS} Threads::Threads)
  if(NOT APPLE)
    target_link_libraries(yov_clap_test PRIVATE X11::X11)
  endif()
endif()
