cmake_minimum_required(VERSION 3.20)
project(caw LANGUAGES C CXX)

# CMake 4 refuses old projects (PortAudio 19.7 declares CMake 3.1). This lets them configure anyway.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# ------------------------------------------------------------------ shared download cache
# Everything downloaded (ImGui, PortAudio, GLFW, the Steinberg ASIO SDK) goes to ONE folder outside the project,
# so copying / moving / deleting the project folder never triggers a new download.
# Change it with:  cmake -B build -DCAW_DEPS_DIR=D:/caw_deps
if(WIN32)
  set(_caw_home "$ENV{USERPROFILE}")
else()
  set(_caw_home "$ENV{HOME}")
endif()
set(CAW_DEPS_DIR "${_caw_home}/.caw_deps" CACHE PATH "Shared folder for downloaded dependencies")
file(MAKE_DIRECTORY "${CAW_DEPS_DIR}")
# Once downloaded, never contact GitHub again (the build works offline). To update: delete the folder in CAW_DEPS_DIR/fetch
set(FETCHCONTENT_UPDATES_DISCONNECTED ON CACHE BOOL "" FORCE)
set(FETCHCONTENT_BASE_DIR "${CAW_DEPS_DIR}/fetch" CACHE PATH "" FORCE)

# ------------------------------------------------------------------ offline use
# A dependency that is already on disk in CAW_DEPS_DIR/fetch/<name>-src is used as it is: no git, no internet.
# (To prepare another PC without internet: copy the whole .caw_deps folder there.)
function(caw_use_local name keyfile)
  string(TOUPPER ${name} _up)
  set(_dir "${CAW_DEPS_DIR}/fetch/${name}-src")
  if(EXISTS "${_dir}/${keyfile}")
    set(FETCHCONTENT_SOURCE_DIR_${_up} "${_dir}" CACHE PATH "" FORCE)
    message(STATUS "Using local copy of ${name}: ${_dir}")
  else()
    message(STATUS "${name}: not on disk yet, it will be downloaded (needs internet once)")
  endif()
endfunction()
caw_use_local(imgui imgui.cpp)
caw_use_local(texteditor TextEditor.cpp)
caw_use_local(portaudio CMakeLists.txt)
caw_use_local(glfw CMakeLists.txt)
caw_use_local(vst3sdk CMakeLists.txt)

# ------------------------------------------------------------------ Dear ImGui
FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG v1.90.9)
FetchContent_MakeAvailable(imgui)

# ------------------------------------------------------------------ text editor widget
# Put your existing TextEditor.h / TextEditor.cpp folder here (the one the Windows build used), or let CMake download one:
#   cmake -B build -DCAW_TEXTEDITOR_DIR=/path/to/ImGuiColorTextEdit
set(CAW_TEXTEDITOR_DIR "" CACHE PATH "Folder with TextEditor.h and TextEditor.cpp (ImGuiColorTextEdit)")
if(NOT CAW_TEXTEDITOR_DIR AND EXISTS "${CMAKE_SOURCE_DIR}/texteditor/TextEditor.h")
  set(CAW_TEXTEDITOR_DIR "${CMAKE_SOURCE_DIR}/texteditor")   # CAW ships a patched copy: uniform comment and string colors
endif()
if(NOT CAW_TEXTEDITOR_DIR)
  FetchContent_Declare(texteditor GIT_REPOSITORY https://github.com/BalazsJako/ImGuiColorTextEdit.git GIT_TAG master)
  FetchContent_MakeAvailable(texteditor)
  set(CAW_TEXTEDITOR_DIR ${texteditor_SOURCE_DIR})
endif()

# ------------------------------------------------------------------ PortAudio (with every host API of the system)
set(PA_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)   # PortAudio 19.7 reads this name: no portaudio_x64.dll next to caw.exe
set(PA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(PA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
if(WIN32)
  # Windows: ASIO (downloads the Steinberg SDK), WASAPI, DirectSound, MME and WDM-KS
  # The ASIO SDK zip is downloaded once into CAW_DEPS_DIR and unpacked there once (PortAudio wants the unpacked folder).
  # You can also put asiosdk.zip there by hand, or unpack the SDK yourself and pass -DASIOSDK_ROOT_DIR=C:/path/asiosdk
  if(NOT ASIOSDK_ROOT_DIR)
    set(_asio_zip "${CAW_DEPS_DIR}/asiosdk.zip")
    set(_asio_dir "${CAW_DEPS_DIR}/asiosdk")
    file(GLOB _asio_found "${_asio_dir}/*/common/asio.h" "${_asio_dir}/common/asio.h")
    if(NOT _asio_found)
      if(NOT EXISTS "${_asio_zip}")
        message(STATUS "Downloading the Steinberg ASIO SDK once to ${_asio_zip}")
        file(DOWNLOAD "https://www.steinberg.net/asiosdk" "${_asio_zip}" SHOW_PROGRESS STATUS _asio_status)
        list(GET _asio_status 0 _asio_code)
        if(NOT _asio_code EQUAL 0)
          file(REMOVE "${_asio_zip}")
          message(FATAL_ERROR "ASIO SDK download failed. Download it from steinberg.net/developers and save it as ${_asio_zip}")
        endif()
      endif()
      file(MAKE_DIRECTORY "${_asio_dir}")
      file(ARCHIVE_EXTRACT INPUT "${_asio_zip}" DESTINATION "${_asio_dir}")
      file(GLOB _asio_found "${_asio_dir}/*/common/asio.h" "${_asio_dir}/common/asio.h")
      if(NOT _asio_found)
        file(REMOVE "${_asio_zip}")
        message(FATAL_ERROR "${_asio_zip} is not a valid ASIO SDK zip (deleted). Download it from steinberg.net/developers, save it as that file and run again.")
      endif()
    endif()
    list(GET _asio_found 0 _asio_header)
    get_filename_component(_asio_common "${_asio_header}" DIRECTORY)
    get_filename_component(_asio_root "${_asio_common}" DIRECTORY)
    set(ASIOSDK_ROOT_DIR "${_asio_root}" CACHE PATH "" FORCE)
    message(STATUS "Using ASIO SDK: ${ASIOSDK_ROOT_DIR}")
  endif()
  set(PA_USE_ASIO  ON CACHE BOOL "" FORCE)
  set(PA_USE_WASAPI ON CACHE BOOL "" FORCE)
  set(PA_USE_DS    ON CACHE BOOL "" FORCE)
  set(PA_USE_WMME  ON CACHE BOOL "" FORCE)
  set(PA_USE_WDMKS ON CACHE BOOL "" FORCE)
elseif(APPLE)
  set(PA_USE_COREAUDIO ON CACHE BOOL "" FORCE)
else()
  set(PA_USE_ALSA ON CACHE BOOL "" FORCE)
  set(PA_USE_JACK ON CACHE BOOL "" FORCE)
  set(PA_USE_OSS  ON CACHE BOOL "" FORCE)
endif()
FetchContent_Declare(portaudio GIT_REPOSITORY https://github.com/PortAudio/portaudio.git GIT_TAG v19.7.0)
FetchContent_MakeAvailable(portaudio)

# ------------------------------------------------------------------ GLFW (macOS and Linux window; Windows uses Direct3D 11)
if(NOT WIN32)
  set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(glfw GIT_REPOSITORY https://github.com/glfw/glfw.git GIT_TAG 3.4)
  FetchContent_MakeAvailable(glfw)
  find_package(OpenGL REQUIRED)
endif()

# ------------------------------------------------------------------ VST3 host (Steinberg VST3 SDK, MIT/GPLv3 dual licence since 3.8)
# cmake -B build -DCAW_VST3=OFF   builds CAW without the VST3 code. -DCAW_VST3_SDK_DIR=/path/vst3sdk uses a local copy.
option(CAW_VST3 "Build the VST3 host (VSTi / VSTe tracks)" ON)
set(CAW_VST3_SDK_DIR "" CACHE PATH "Local VST3 SDK folder (with its submodules); empty = download")
if(CAW_VST3)
 set(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES OFF CACHE BOOL "" FORCE)
 set(SMTG_ENABLE_VST3_HOSTING_EXAMPLES OFF CACHE BOOL "" FORCE)
 set(SMTG_ENABLE_VSTGUI_SUPPORT OFF CACHE BOOL "" FORCE)
 set(SMTG_ADD_VSTGUI OFF CACHE BOOL "" FORCE)
 set(SMTG_RUN_VST_VALIDATOR OFF CACHE BOOL "" FORCE)
 set(SMTG_CREATE_PLUGIN_LINK OFF CACHE BOOL "" FORCE)
 set(SMTG_CREATE_BUNDLE_FOR_WINDOWS OFF CACHE BOOL "" FORCE)
 if(CAW_VST3_SDK_DIR)
  set(FETCHCONTENT_SOURCE_DIR_VST3SDK "${CAW_VST3_SDK_DIR}" CACHE PATH "" FORCE)
 endif()
 FetchContent_Declare(vst3sdk GIT_REPOSITORY https://github.com/steinbergmedia/vst3sdk.git GIT_TAG v3.7.12_build_20 GIT_SUBMODULES "base;pluginterfaces;public.sdk;cmake" GIT_SHALLOW ON)
 FetchContent_MakeAvailable(vst3sdk)
 # The Steinberg SDK ships a RELATIVE archive output dir (lib/<config>). On the Visual Studio
 # generator that resolves relative to each SDK .vcxproj's own folder, so when CAW is built
 # out-of-tree or its build folder is reused after a deps-cache move, the linker looks for
 # sdk_hosting.lib next to caw.vcxproj and fails with LNK1181. Pin every SDK static lib to an
 # ABSOLUTE path inside THIS build tree; caw links them by target name, so the link line then
 # always carries the real, resolved path.
 foreach(_t sdk_hosting sdk_common base pluginterfaces)
  if(TARGET ${_t})
   set_target_properties(${_t} PROPERTIES
     ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/$<CONFIG>"
     LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/$<CONFIG>"
     RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
  endif()
 endforeach()
endif()
# ------------------------------------------------------------------ CAW
set(CAW_SOURCES
  src/app.cpp src/engine_dispatch.cpp src/ui_panels.cpp src/ui_timeline.cpp src/ui_sound_editor.cpp src/audio_devices.cpp src/csd_builder.cpp src/csound_api_runner.cpp
  src/platform.cpp src/process_runner.cpp src/project_io.cpp src/score_svg.cpp src/pd_backend.cpp src/svg_raster.cpp
  ${CAW_TEXTEDITOR_DIR}/TextEditor.cpp
  ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp ${imgui_SOURCE_DIR}/imgui_widgets.cpp)

if(WIN32)
  list(APPEND CAW_SOURCES src/main.cpp res/caw.rc
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp)
  add_executable(caw WIN32 ${CAW_SOURCES})
  target_link_libraries(caw PRIVATE d3d11 dxgi d3dcompiler dwmapi ws2_32 ole32 shell32 comdlg32 propsys dbghelp)
  target_compile_definitions(caw PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
  if(MSVC)
    target_compile_options(caw PRIVATE /utf-8)
  endif()
  # Ship a known MP3-capable FFmpeg beside caw.exe; no end-user install or
  # PATH change. The first build downloads ~105 MB and checks the pinned hash.
  option(CAW_BUNDLE_FFMPEG "Bundle a Windows FFmpeg executable" ON)
  set(CAW_FFMPEG_EXECUTABLE "" CACHE FILEPATH "Optional existing ffmpeg.exe to bundle instead of downloading")
  if(CAW_BUNDLE_FFMPEG)
    if(CAW_FFMPEG_EXECUTABLE)
      if(NOT EXISTS "${CAW_FFMPEG_EXECUTABLE}")
        message(FATAL_ERROR "CAW_FFMPEG_EXECUTABLE does not exist: ${CAW_FFMPEG_EXECUTABLE}")
      endif()
      set(_caw_ffmpeg_exe "${CAW_FFMPEG_EXECUTABLE}")
      message(WARNING "Using a custom FFmpeg: verify MP3 support and supply its license and corresponding source when distributing.")
    else()
      FetchContent_Declare(caw_ffmpeg
        URL "https://github.com/GyanD/codexffmpeg/releases/download/8.1.2/ffmpeg-8.1.2-essentials_build.zip"
        URL_HASH SHA256=db580001caa24ac104c8cb856cd113a87b0a443f7bdf47d8c12b1d740584a2ec)
      FetchContent_MakeAvailable(caw_ffmpeg)
      set(_caw_ffmpeg_exe "${caw_ffmpeg_SOURCE_DIR}/bin/ffmpeg.exe")
      if(NOT EXISTS "${_caw_ffmpeg_exe}")
        message(FATAL_ERROR "FFmpeg archive did not contain bin/ffmpeg.exe.")
      endif()
      foreach(_doc LICENSE LICENSE.txt README.txt)
        if(EXISTS "${caw_ffmpeg_SOURCE_DIR}/${_doc}")
          add_custom_command(TARGET caw POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:caw>/ffmpeg-licenses"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${caw_ffmpeg_SOURCE_DIR}/${_doc}" "$<TARGET_FILE_DIR:caw>/ffmpeg-licenses/${_doc}"
            VERBATIM)
        endif()
      endforeach()
      if(EXISTS "${caw_ffmpeg_SOURCE_DIR}/doc")
        add_custom_command(TARGET caw POST_BUILD
          COMMAND ${CMAKE_COMMAND} -E copy_directory "${caw_ffmpeg_SOURCE_DIR}/doc" "$<TARGET_FILE_DIR:caw>/ffmpeg-licenses/doc"
          VERBATIM)
      endif()
      message(STATUS "Bundling FFmpeg 8.1.2; review GPL redistribution requirements in README.md.")
    endif()
    add_custom_command(TARGET caw POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_caw_ffmpeg_exe}" "$<TARGET_FILE_DIR:caw>/ffmpeg.exe"
      VERBATIM)
  endif()
else()
  list(APPEND CAW_SOURCES src/main_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
  if(APPLE)
    set(CAW_ICON ${CMAKE_SOURCE_DIR}/res/caw.icns)
    if(EXISTS ${CAW_ICON})
      list(APPEND CAW_SOURCES ${CAW_ICON})
      set_source_files_properties(${CAW_ICON} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    endif()
    add_executable(caw MACOSX_BUNDLE ${CAW_SOURCES})
    set_target_properties(caw PROPERTIES
      MACOSX_BUNDLE_BUNDLE_NAME "CAW"
      MACOSX_BUNDLE_GUI_IDENTIFIER "org.caw.audioworkstation"
      MACOSX_BUNDLE_ICON_FILE "caw.icns"
      MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0")
    target_link_libraries(caw PRIVATE "-framework Cocoa" "-framework IOKit" "-framework CoreFoundation")
  else()
    add_executable(caw ${CAW_SOURCES})
    find_package(Threads REQUIRED)
    target_link_libraries(caw PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
  endif()
  target_link_libraries(caw PRIVATE glfw OpenGL::GL)
endif()

if(CAW_VST3)
 target_sources(caw PRIVATE src/vst3_host.cpp
  ${vst3sdk_SOURCE_DIR}/public.sdk/source/common/memorystream.cpp
  ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/hosting/module.cpp
  ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/hosting/plugprovider.cpp
  ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/hosting/connectionproxy.cpp)
 if(WIN32)
  target_sources(caw PRIVATE ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/hosting/module_win32.cpp)
 elseif(APPLE)
  target_sources(caw PRIVATE ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/hosting/module_mac.mm)
 else()
  target_sources(caw PRIVATE ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/hosting/module_linux.cpp)
 endif()
 target_link_libraries(caw PRIVATE sdk_hosting sdk_common)
 target_include_directories(caw PRIVATE ${vst3sdk_SOURCE_DIR})
 target_compile_definitions(caw PRIVATE CAW_VST3=1)
endif()
target_include_directories(caw PRIVATE src ${CAW_TEXTEDITOR_DIR} ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
if(TARGET portaudio_static)
  target_link_libraries(caw PRIVATE portaudio_static)   # linked into caw.exe: no extra DLL needed
else()
  target_link_libraries(caw PRIVATE portaudio)
endif()

# The text editor widget uses ImGui::PushItemFlag / ImGuiItemFlags_NoTabStop, which newer ImGui keeps in imgui_internal.h
if(MSVC)
  set_source_files_properties(${CAW_TEXTEDITOR_DIR}/TextEditor.cpp PROPERTIES COMPILE_OPTIONS "/FIimgui_internal.h")
else()
  set_source_files_properties(${CAW_TEXTEDITOR_DIR}/TextEditor.cpp PROPERTIES COMPILE_OPTIONS "-include;imgui_internal.h")
endif()
