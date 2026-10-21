cmake_minimum_required(VERSION 3.21)
project(YeOldeVST VERSION 0.1.0 LANGUAGES C CXX)

if(NOT WIN32)
  message(FATAL_ERROR "YeOldeVST hosts Windows VST2 DLLs and is Windows-only.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Ship without a VC runtime dependency.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_compile_definitions(UNICODE _UNICODE NOMINMAX _CRT_SECURE_NO_WARNINGS)
add_compile_options(/utf-8 /W3 /MP)

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

# --- core: shared by the plugin and the helper exes ---------------------------------------------
add_library(yov_core STATIC
  src/common/Json.cpp
  src/common/PeInfo.cpp
  src/common/PluginDb.cpp
  src/common/Seh.cpp
  src/common/Util.cpp
  src/host/Vst2Plugin.cpp
  src/scan/Scanner.cpp
)
target_include_directories(yov_core PUBLIC src)
target_link_libraries(yov_core PUBLIC shell32 ole32 user32 advapi32)

# --- helper exe: scanner probe + out-of-process bridge ----------------------------------------
add_executable(yov_host
  src/hostexe/main.cpp
  src/hostexe/Probe.cpp
  src/hostexe/Bridge.cpp
)
target_link_libraries(yov_host PRIVATE yov_core gdi32 comctl32)
set_target_properties(yov_host PROPERTIES OUTPUT_NAME "YeOldeVST-Host${YOV_BITS}")
if(MSVC)
  # Many old plugins assume a big main-thread stack.
  target_link_options(yov_host PRIVATE /STACK:8388608)
endif()

if(NOT YOV_BUILD_PLUGIN)
  return()
endif()

# --- 32-bit helper, built as a sub-project for Win32 ------------------------------------------
include(ExternalProject)
set(YOV_HOST32_DIR ${CMAKE_BINARY_DIR}/host32)
ExternalProject_Add(yov_host32
  SOURCE_DIR ${CMAKE_SOURCE_DIR}
  BINARY_DIR ${YOV_HOST32_DIR}
  CMAKE_GENERATOR ${CMAKE_GENERATOR}
  CMAKE_GENERATOR_PLATFORM Win32
  CMAKE_ARGS -DYOV_BUILD_PLUGIN=OFF
  BUILD_COMMAND ${CMAKE_COMMAND} --build ${YOV_HOST32_DIR} --config $<CONFIG> --target yov_host
  INSTALL_COMMAND ""
  BUILD_ALWAYS TRUE
)
set(YOV_HOST32_EXE ${YOV_HOST32_DIR}/$<CONFIG>/YeOldeVST-Host32.exe)

# --- the plugin ---------------------------------------------------------------------------------
set(CLAP_SDK_ROOT ${CMAKE_SOURCE_DIR}/external/clap)
set(VST3_SDK_ROOT ${CMAKE_SOURCE_DIR}/external/vst3sdk)
set(CLAP_WRAPPER_OUTPUT_NAME "YeOldeVST")
set(CLAP_WRAPPER_DONT_ADD_TARGETS TRUE)
add_subdirectory(external/clap-wrapper EXCLUDE_FROM_ALL)

# Both helper exes are embedded in the plugin binary so it can be installed as a single file.
file(GENERATE OUTPUT ${CMAKE_BINARY_DIR}/embed_$<CONFIG>.rc CONTENT
"201 RCDATA \"$<TARGET_FILE:yov_host>\"
202 RCDATA \"${YOV_HOST32_EXE}\"
")

add_library(yov_plugin_impl STATIC
  src/plugin/Plugin.cpp
  src/plugin/Gui.cpp
  src/plugin/HostedPlugin.cpp
  src/plugin/BridgeClient.cpp
)
target_link_libraries(yov_plugin_impl PUBLIC yov_core clap comctl32 gdi32 uxtheme dwmapi shlwapi)

foreach(fmt clap vst3)
  add_library(yov_${fmt} MODULE
    src/plugin/entry.cpp
    src/plugin/plugin.manifest
    ${CMAKE_BINARY_DIR}/embed_$<CONFIG>.rc
  )
  target_link_libraries(yov_${fmt} PRIVATE yov_plugin_impl)
  add_dependencies(yov_${fmt} yov_host yov_host32)
  set_source_files_properties(${CMAKE_BINARY_DIR}/embed_$<CONFIG>.rc PROPERTIES OBJECT_DEPENDS
    "$<TARGET_FILE:yov_host>;${YOV_HOST32_EXE}")
endforeach()

set_target_properties(yov_clap PROPERTIES OUTPUT_NAME "YeOldeVST" SUFFIX ".clap")
target_add_vst3_wrapper(TARGET yov_vst3 OUTPUT_NAME "YeOldeVST" WINDOWS_FOLDER_VST3 TRUE)
# clap-wrapper's sources are C++17 (u8string / fmt pointer formatting changed in C++20).
set_target_properties(yov_vst3-clap-wrapper-vst3-lib PROPERTIES CXX_STANDARD 17)

# --- tests ------------------------------------------------------------------------------------
add_executable(yov_clap_test tests/clap_test.cpp)
target_link_libraries(yov_clap_test PRIVATE clap user32 gdi32)
