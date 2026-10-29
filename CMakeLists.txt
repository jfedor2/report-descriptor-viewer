cmake_minimum_required(VERSION 3.20)
project(report-descriptor-viewer CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# static CRT so the exe runs without the VC++ redistributable
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)
FetchContent_Declare(imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG v1.92.9b
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(imgui)

add_library(imgui_lib STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
)
target_include_directories(imgui_lib PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)

# Everything except the UI, so the command line tool can link it too.
add_library(rdv_core STATIC
  src/devices.cpp
  src/decoder.cpp
  src/usages.cpp
  src/winusb.cpp
)
target_compile_definitions(rdv_core PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(rdv_core PUBLIC /W3 /utf-8)
target_link_libraries(rdv_core PUBLIC setupapi hid cfgmgr32 newdev winusb ole32 shell32)

add_executable(report-descriptor-viewer WIN32 src/main.cpp)
target_link_libraries(report-descriptor-viewer PRIVATE rdv_core imgui_lib d3d11 dxgi d3dcompiler comdlg32)

# Command line tool that lists devices and dumps their descriptors.
add_executable(rdv-dump src/dump_main.cpp)
target_link_libraries(rdv-dump PRIVATE rdv_core)

set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT report-descriptor-viewer)
