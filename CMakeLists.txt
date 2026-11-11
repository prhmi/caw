cmake_minimum_required(VERSION 3.20)
project(Caw CXX RC)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- Dear ImGui is downloaded automatically (needs git installed) ---
include(FetchContent)
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.9
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(imgui)

FetchContent_Declare(
    imgui_color_text_edit
    GIT_REPOSITORY https://github.com/BalazsJako/ImGuiColorTextEdit.git
    GIT_TAG master
    GIT_SHALLOW TRUE
)
FetchContent_GetProperties(imgui_color_text_edit)
if(NOT imgui_color_text_edit_POPULATED)
    FetchContent_Populate(imgui_color_text_edit)
endif()

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui PUBLIC d3d11 dxgi d3dcompiler dwmapi)

add_library(imgui_color_text_edit STATIC ${imgui_color_text_edit_SOURCE_DIR}/TextEditor.cpp)
target_include_directories(imgui_color_text_edit PUBLIC ${imgui_color_text_edit_SOURCE_DIR})
target_link_libraries(imgui_color_text_edit PUBLIC imgui)

add_executable(caw WIN32
    src/main.cpp
    src/app.cpp
    src/csd_builder.cpp
    src/meters.cpp
    src/audio_devices.cpp
    src/process.cpp
    src/osc_sender.cpp
    src/project_io.cpp
    src/score_svg.cpp
    res/caw.rc
)
target_compile_definitions(caw PRIVATE NOMINMAX)
target_link_libraries(caw PRIVATE imgui imgui_color_text_edit winmm comdlg32 shell32 ws2_32)
