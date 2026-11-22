#pragma once

// app_shared.h - the canonical include block for every CAW app translation unit.
// (Phase 2: extracted from app.cpp; app.cpp and the ui_*.cpp / engine_dispatch.cpp
//  files all include ONLY this header, which carries the engine + UI stack, the
//  shared state (app_state.h) and the internal function declarations (app_internal.h).)

#include "app.h"
#include "caw_toggles.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
// the few Winsock names used below, mapped to the BSD socket API
using SOCKET = int;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define closesocket close
#endif
#include "platform.h"
#include "audio_devices.h"
#include "audio_device_match.h"
#include "audio_codec.h"
#include "branding.h"
#include "startup_logo.h"
#include "svg_raster.h"
#include "arrow_widgets.h"
#include "csd_builder.h"
#include "timeline_edit.h"   // clip_automation_move + clip_timing + timeline_loop + timeline_zoom + midi_clip_ops + audio_channels (phase-1 merge)
#include "code_export.h"
#include "imgui.h"
#include "process_runner.h"
#include "process_quote.h"
#include "csound_api_runner.h"
#include "pd_preview.h"
#include "sc_engine.h"
#include "sc_diagnostics.h"
#include "pd_diagnostics.h"
#include "pd_timeline.h"
#include "pd_daw_engine.h"
#include "pd_test_patch.h"
#include "pd_automation_patch.h"
#include "pd_patch_editor.h"
#include "pd_graph_edit.h"
#include "selected_test_sound.h"
#include "project_io.h"
#include "score_svg.h"
#include "TextEditor.h"
#include "csound_opcodes.h"
#include "marks_gfx.h"
#include "vst_ui.h"
#include "vst_lang.h"
#include "vst_solo.h"
#include "monitor_view.h"   // monitor_math + monitor_fft + monitor_capture + monitor_frame + monitor_parse + monitor_bands + monitor_peaks + monitor_osc + monitor_work (phase-1 merge)
#include "audio_track.h"
#include "master_fx.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cstdarg>
#include <chrono>
#include <cmath>
#include <cctype>
#include <deque>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <iomanip>
#include <locale>
#include <limits>
#include <functional>
#include <regex>
#include <sstream>
#include <cstring>
#include <utility>
#include <unordered_map>
#include <memory>

#include "app_state.h"
#include "app_internal.h"
