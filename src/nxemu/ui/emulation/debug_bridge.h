// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include "debug_input.h"
#include "yuzu_video_core/native_debug_document.h"
#include <nxemu-core/settings/identifiers.h>
#include <nxemu-core/settings/settings.h>
#include <nxemu-module-spec/video.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

// Opt-in local API. All session controls run on the owning UI thread, not a worker.
// Status reads only atomic video counters and frontend-owned session state.
class DebugBridge {
public:
    explicit DebugBridge(const std::string& path): root(std::filesystem::u8path(path)) {
        if (path.empty()) return;
        std::error_code ec; std::filesystem::create_directories(root, ec);
        enabled = !ec;
        if (enabled) {
            const auto diagnosticPath = (root / "diagnostics").string();
#ifdef _WIN32
            _putenv_s("NXEMU_NATIVE_SHADER_DIAG_DIR", diagnosticPath.c_str());
#else
            setenv("NXEMU_NATIVE_SHADER_DIAG_DIR", diagnosticPath.c_str(), 1);
#endif
        }
        run = std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
        if (enabled) {
            // Keep legacy bridge launches detailed, but permit status/control-only runs.
            // Explicit zero is disabled; unlike the old nonempty test it is not truthy.
            const char* requestedDetail = std::getenv("NXEMU_DEBUG_DETAIL");
            const char* detailValue = !requestedDetail || std::string(requestedDetail) == "1" ? "1" : "0";
#ifdef _WIN32
            _putenv_s("NXEMU_DEBUG_DETAIL", detailValue);
            _putenv_s("NXEMU_DEBUG_RUN_ID", run.c_str());
#else
            setenv("NXEMU_DEBUG_DETAIL", detailValue, 1);
            setenv("NXEMU_DEBUG_RUN_ID", run.c_str(), 1);
#endif
        }
    }
    void Tick(EmulatorSession& session, MainWindow& window, bool exiting = false) {
        if (!enabled) return;
        const auto now = std::chrono::steady_clock::now();
        const auto inputNow = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        const auto inputGeneration = session.DebugModuleGeneration();
        // ReleasePending is also true during a healthy run (release-on-stop).
        // IsRunning is cleared before teardown, as for the video snapshot below.
        const bool inputSafe = session.IsValid() && session.IsRunning() && !session.IsLaunching() && !exiting;
        const bool inputUsable = inputSafe && !session.IsPaused();
        const auto inputSend = [&](uint32_t player, uint32_t button, bool pressed) {
            auto& os = session.Modules().OperatingSystem();
            os.SetPlayerButtonState(player, button, pressed);
            const auto npad = player < 8 ? static_cast<NpadIdType>(player) :
                player == 8 ? NpadIdType::Other : NpadIdType::Handheld;
            button_status_t values[static_cast<uint32_t>(NativeButtonValues::NumButtons)]{};
            os.GetEmulatedController(npad).GetButtonsStatus(values, static_cast<uint32_t>(NativeButtonValues::NumButtons));
            input.Observe(values[button].value);
        };
        if (!inputUsable) input.Cancel(inputSafe, inputGeneration, inputSend);
        input.Advance(inputNow, inputUsable, inputGeneration, inputSend);
        if (!exiting && now - last < std::chrono::milliseconds(250)) return;
        last = now;
        std::ifstream request(root / "command.txt");
        std::string requestedRun, verb, value; uint64_t id{};
        if (request >> requestedRun >> id >> verb; request && id > lastRequest && requestedRun == run) {
            std::string ignored; std::getline(request, ignored); std::getline(request, value);
            lastRequest = id; response = "rejected";
            if (verb == "stop" || verb == "pause" || verb == "quit")
                input.Cancel(inputSafe, inputGeneration, inputSend);
            if (verb == "input" && inputUsable && !input.active && input.Parse(value)) {
                input.Start(id, inputGeneration, inputNow, inputSend); response = "accepted";
            }
            else if (verb == "stop") { session.Stop(); response = "accepted"; }
            else if (verb == "pause" && session.IsRunning() && !session.IsLaunching()) {
                if (!session.IsPaused()) session.TogglePause(); response = "accepted";
            } else if (verb == "resume" && session.IsRunning() && !session.IsLaunching()) {
                if (session.IsPaused()) session.TogglePause(); response = "accepted";
            } else if (verb == "load" && !session.IsRunning() && !session.ReleasePending() && !value.empty()) {
                std::error_code ec;
                if (std::filesystem::is_regular_file(std::filesystem::u8path(value), ec)) {
                    window.OpenGame(value); response = "accepted";
                }
            } else if (verb == "quit") { session.Stop(); quitPending = true; response = "accepted"; }
        }
        if (quitPending && !session.IsRunning() && !session.ReleasePending()) window.RequestQuit();
        const auto generation = session.DebugModuleGeneration();
        if (generation != lastGeneration) {
            const auto text = std::to_string(generation);
#ifdef _WIN32
            _putenv_s("NXEMU_DEBUG_GENERATION", text.c_str());
#else
            setenv("NXEMU_DEBUG_GENERATION", text.c_str(), 1);
#endif
            lastGeneration = generation;
        }
        VideoDebugSnapshot video{};
        const bool modulesValid = session.IsValid() && !session.IsLaunching();
        // Stop() clears IsRunning before the asynchronous module/GPU release starts.
        // Never dereference the video backend during that teardown window.
        const bool valid = modulesValid && session.IsRunning() && !exiting;
        if (valid) video = session.Modules().Video().DebugSnapshot();
        const auto phase = session.CurrentPhase();
        const char* name = phase == EmulatorSession::Phase::Running ? "running" :
            phase == EmulatorSession::Phase::Loading ? "loading" :
            phase == EmulatorSession::Phase::Paused ? "paused" : "idle";
        const auto state = SettingsStore::GetInstance().GetInt(NXCoreSetting::EmulationState);
        const bool advancing = video.composites_completed > previousFrames;
        previousFrames = video.composites_completed;
        const char* milestone = exiting ? "exited" : !session.IsRunning() ?
            (session.ReleasePending() ? "backend_stopping" : "backend_ready") :
            phase == EmulatorSession::Phase::Paused ? "paused" : video.mapping_bytes ? "gpu_memory_mapping" :
            advancing ? "frames_advancing" : video.composites_completed ? "frames_submitted" :
            video.present_requests ? "present_pending" : video.command_submissions ? "gpu_commands_submitted" :
            session.IsRunning() ? "waiting_first_frame" : "backend_ready";
#ifdef _WIN32
        const auto pid = GetCurrentProcessId();
#else
        const auto pid = getpid();
#endif
        const auto epoch = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        const auto& perf = window.DebugPerformanceStats();
        const auto finite = [valid](double v) { return valid && std::isfinite(v) ? v : 0.0; };
        uint32_t inputMask = 0; bool inputConnected = false;
        if (inputUsable) {
            const auto npad = input.player < 8 ? static_cast<NpadIdType>(input.player) :
                input.player == 8 ? NpadIdType::Other : NpadIdType::Handheld;
            auto& controller = session.Modules().OperatingSystem().GetEmulatedController(npad);
            button_status_t values[static_cast<uint32_t>(NativeButtonValues::NumButtons)]{};
            controller.GetButtonsStatus(values, static_cast<uint32_t>(NativeButtonValues::NumButtons));
            inputConnected = controller.IsConnected();
            for (uint32_t i=0; i<static_cast<uint32_t>(NativeButtonValues::NumButtons); ++i)
                if (values[i].value) inputMask |= (1u<<i);
        }
        std::ostringstream out;
        out << "{\"schema\":1,\"pid\":" << pid << ",\"run_id\":\"" << run
            << "\",\"epoch_ms\":" << epoch << ",\"heartbeat\":" << ++heartbeat
            << ",\"phase\":\"" << name << "\",\"emulation_state\":" << state
            << ",\"milestone\":\"" << milestone << "\",\"modules_valid\":" << (modulesValid ? "true" : "false")
            << ",\"video_snapshot_available\":" << (valid ? "true" : "false")
            << ",\"exited\":" << (exiting ? "true" : "false") << ",\"map_calls\":" << video.map_calls
            << ",\"mapped_bytes_total\":" << video.mapped_bytes << ",\"mapping_bytes\":" << video.mapping_bytes
            << ",\"command_submissions\":" << video.command_submissions << ",\"present_requests\":" << video.present_requests
            << ",\"composites_completed\":" << video.composites_completed
            << ",\"game_fps\":" << finite(perf.average_game_fps)
            << ",\"frame_time_ms\":" << finite(perf.frametime * 1000.0)
            << ",\"system_fps\":" << finite(perf.system_fps)
            << ",\"emulation_speed\":" << finite(perf.emulation_speed)
            << ",\"perf_sample\":" << window.DebugPerformanceSample()
            << ",\"bridge_version\":5,\"module_generation\":" << generation
            << ",\"diagnostic_config\":{\"source\":\"startup_environment_not_coverage_proof\",\"detail\":"
            << (VideoCommon::DebugDocument::Enabled() ? "true" : "false")
            << ",\"handle_trace\":"
            << ((VideoCommon::DebugDocument::Environment("NXEMU_HANDLE_TRACE").empty() ?
                 VideoCommon::DebugDocument::Enabled() : VideoCommon::DebugDocument::Environment("NXEMU_HANDLE_TRACE") == "1") ? "true" : "false")
            << ",\"shader_rejection_trace\":"
            << ((VideoCommon::DebugDocument::Environment("NXEMU_SHADER_REJECTION_TRACE").empty() ?
                 VideoCommon::DebugDocument::Enabled() : VideoCommon::DebugDocument::Environment("NXEMU_SHADER_REJECTION_TRACE") == "1") ? "true" : "false")
            << ",\"shader_fixtures\":"
            << (VideoCommon::DebugDocument::Environment("NXEMU_NATIVE_SHADER_FIXTURES") == "1" ? "true" : "false")
            << ",\"frame_capture\":"
            << ((VideoCommon::DebugDocument::Environment("NXEMU_FRAME_CAPTURE").empty() ?
                 VideoCommon::DebugDocument::Enabled() : VideoCommon::DebugDocument::Environment("NXEMU_FRAME_CAPTURE") == "1") ? "true" : "false")
            << ",\"paired_pixels\":" << (VideoCommon::DebugDocument::Environment("NXEMU_PAIRED_PIXELS")=="1"?"true":"false")
            << ",\"image_writers\":" << (VideoCommon::DebugDocument::Environment("NXEMU_IMAGE_WRITERS")=="1"?"true":"false")
            << ",\"native_draw_inputs\":" << (VideoCommon::DebugDocument::Environment("NXEMU_NATIVE_DRAW_INPUTS")=="1"?"true":"false")
            << ",\"shader_progress\":"
            << ((VideoCommon::DebugDocument::Environment("NXEMU_SHADER_PROGRESS").empty() ?
                 VideoCommon::DebugDocument::Enabled() : VideoCommon::DebugDocument::Environment("NXEMU_SHADER_PROGRESS") == "1") ? "true" : "false")
            << ",\"pipeline_fixtures\":" << (VideoCommon::DebugDocument::Environment("NXEMU_PIPELINE_FIXTURES")=="1"?"true":"false")
            << ",\"pipeline_fixture_key\":" << VideoCommon::DebugDocument::Quote(VideoCommon::DebugDocument::Environment("NXEMU_PIPELINE_FIXTURE_KEY")) << "}"
            << ",\"game_path\":" << VideoCommon::DebugDocument::Quote(session.DebugGamePath())
            << ",\"guest_program_id\":" << (valid ? session.Modules().OperatingSystem().GetProgramId() : 0)
            << ",\"is_64bit_program\":" << (SettingsStore::GetInstance().GetBool(NXCoreSetting::Is64BitProgram) ? "true" : "false")
            << ",\"gpu_initialization_state\":" << SettingsStore::GetInstance().GetInt(NXCoreSetting::GpuInitializationState)
            << ",\"loader_result\":" << SettingsStore::GetInstance().GetInt(NXCoreSetting::GameLoadResult)
            << ",\"disk_cache_stage\":" << VideoCommon::DebugDocument::Quote(SettingsStore::GetInstance().GetString(NXCoreSetting::DiskCacheLoadStage))
            << ",\"disk_cache_current\":" << SettingsStore::GetInstance().GetInt(NXCoreSetting::DiskCacheLoadCurrent)
            << ",\"disk_cache_total\":" << SettingsStore::GetInstance().GetInt(NXCoreSetting::DiskCacheLoadTotal)
            << ",\"capabilities\":[\"status\",\"session_control\",\"ui_perf\",\"host_telemetry\",\"render_documents\",\"bounded_events\",\"inspection\",\"diagnostic_bundle\",\"local_stacks\"]"
            << ",\"display_accelerated\":" << video.display_accelerated
            << ",\"display_fallback\":" << video.display_fallback
            << ",\"display_fallback_us\":" << video.display_fallback_us
            << ",\"display_fallback_bytes\":" << video.display_fallback_bytes
            << ",\"render_debug_version\":" << video.version
            << ",\"unbound_methods\":" << video.unbound_methods << ",\"dma_batches\":" << video.dma_batches
            << ",\"draw_requests\":" << video.draw_requests << ",\"draw_pipeline_ready\":" << video.draw_pipeline_ready
            << ",\"compute_requests\":" << video.compute_requests << ",\"compute_pipeline_ready\":" << video.compute_pipeline_ready
            << ",\"clear_requests\":" << video.clear_requests << ",\"shaders_building\":" << video.shaders_building
            << ",\"input\":{\"available\":" << (inputUsable ? "true" : "false")
            << ",\"feedback_scope\":\"emulated_controller_not_guest_consumption\",\"player\":" << input.player
            << ",\"connected\":" << (inputConnected ? "true" : "false") << ",\"buttons_mask\":" << inputMask
            << ",\"request_id\":" << input.request_id << ",\"status\":\"" << input.status << "\""
            << ",\"active\":" << (input.active ? "true" : "false") << ",\"down\":" << (input.down ? "true" : "false")
            << ",\"completed_pulses\":" << input.completed << ",\"requested_pulses\":" << input.count
            << ",\"press_edges_total\":" << input.presses << ",\"release_edges_total\":" << input.releases
            << ",\"pressed_feedback_samples\":" << input.pressed_seen << ",\"released_feedback_samples\":" << input.released_seen << "}"
            << ",\"request_id\":" << lastRequest << ",\"response\":\"" << response << "\"}\n";
        const auto tmp = root / "status.tmp"; const auto dest = root / "status.json";
        { std::ofstream f(tmp, std::ios::trunc); f << out.str(); if (!f) return; }
#ifdef _WIN32
        MoveFileExW(tmp.c_str(), dest.c_str(), MOVEFILE_REPLACE_EXISTING);
#else
        std::error_code ec; std::filesystem::rename(tmp, dest, ec);
#endif
    }
private:
    DebugInput input;
    std::filesystem::path root;
    std::string run, response{"none"};
    uint64_t lastRequest{}, heartbeat{}, previousFrames{}, lastGeneration{~uint64_t{0}};
    bool enabled{}, quitPending{};
    std::chrono::steady_clock::time_point last{};
};
