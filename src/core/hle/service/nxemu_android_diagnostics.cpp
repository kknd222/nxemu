#include "core/hle/service/nxemu_android_diagnostics.h"

#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <cstdint>
#include <cstring>
#ifdef __ANDROID__
#include <android/log.h>
#endif

namespace Service::NxemuAndroidDiagnostics {
namespace {

std::mutex g_mutex;
std::deque<std::string> g_recent_events;
std::deque<std::string> g_recent_svc_events;
std::unordered_map<std::string, uint64_t> g_counters;
std::string g_cached_snapshot{"osServiceDiagnostics=created"};
bool g_dirty = true;
constexpr std::size_t MaxRecentEvents = 192;
constexpr std::size_t MaxRecentSvcEvents = 256;
std::atomic_bool g_full_trace{false};
std::atomic_bool g_svc_ring_enabled{true};
constexpr std::size_t HotCounterBuckets = 512;
std::array<std::atomic<uint64_t>, HotCounterBuckets> g_hot_counters{};

bool StartsWith(const char* value, const char* prefix) {
    return std::strncmp(value, prefix, std::strlen(prefix)) == 0;
}

uint32_t HashCategory(const char* value) {
    uint32_t hash = 2166136261u;
    while (*value != '\0') {
        hash ^= static_cast<unsigned char>(*value++);
        hash *= 16777619u;
    }
    return hash;
}

bool IsHotPathCategory(const char* cat) {
    static constexpr std::array prefixes{
        "SVC.",
        "IPC.",
        "TIPC.",
        "SM.GetService",
        "SM.GetServiceResult",
        "Nvnflinger.HWC.",
        "Nvnflinger.BQP.",
        "Nvnflinger.Binder.",
        "Nvnflinger.SurfaceFlinger.",
        "VI.Conductor.",
        "VI.Manager.",
        "VI.App.OpenDisplay",
        "VI.App.OpenLayer",
        "VI.Container.ComposeOnDisplay",
        "VI.SharedBuffer.",
        "VI.System.AcquireSharedFrameBuffer",
        "VI.System.PresentSharedFrameBuffer",
        "VI.App.GetDisplayVsyncEvent",
        "VI.App.GetDisplayVsyncEventResult",
        "RendererVulkan.",
        "Video.DmaPusher.",
        "Video.Scheduler.",
        "Video.DrawManager.",
        "Video.Maxwell3D.",
        "NVDRV.Ioctl",
        "NVDRV.QueryEvent",
        "NVDRV.Syncpoint.",
        "NVDRV.nvhost_as_gpu.",
        "NVDRV.nvhost_gpu.",
        "NVDRV.nvhost_ctrl.",
        "NVDRV.nvmap.",
        "NVDRV.nvhost_ctrl.EventWait",
        "OS.CpuSample",
    };
    for (const char* prefix : prefixes) {
        if (StartsWith(cat, prefix)) {
            return true;
        }
    }
    return false;
}

bool ShouldKeepHotEvent(uint64_t count) {
#ifdef NXEMU_ANDROID_FULL_DIAG
    return true;
#else
    if (g_full_trace.load(std::memory_order_relaxed)) {
        return true;
    }
    // Keep enough early samples for diagnostics, then only sparse milestones.
    return count <= 8 || count == 16 || count == 32 || count == 64 || (count % 600) == 0;
#endif
}

std::string NowMs() {
    using namespace std::chrono;
    const auto ms = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
    return std::to_string(ms);
}

void RebuildSnapshotLocked() {
    std::ostringstream out;
    out << "osServiceDiagnostics=ok\n";
    out << "eventCount=" << g_recent_events.size() << "\n";
    out << "svcRingEnabled=" << (g_svc_ring_enabled.load(std::memory_order_relaxed) ? "true" : "false") << "\n";
    out << "svcEventCount=" << g_recent_svc_events.size() << "\n";
    out << "counters:\n";
    for (const auto& [key, value] : g_counters) {
        out << "  " << key << "=" << value << "\n";
    }
    out << "recent_svc:\n";
    for (const auto& event : g_recent_svc_events) {
        out << "  " << event << "\n";
    }
    out << "recent:\n";
    for (const auto& event : g_recent_events) {
        out << "  " << event << "\n";
    }
    g_cached_snapshot = out.str();
}

} // namespace

void RecordEventImpl(const char* category, std::string_view detail) {
    const char* raw_cat = category != nullptr ? category : "unknown";
    const bool hot = IsHotPathCategory(raw_cat);
    const bool svc_trace = g_svc_ring_enabled.load(std::memory_order_relaxed) &&
                           StartsWith(raw_cat, "SVC.");
    uint64_t count = 0;
    std::string detail_text;

    if (hot) {
        auto& bucket = g_hot_counters[HashCategory(raw_cat) % HotCounterBuckets];
        count = bucket.fetch_add(1, std::memory_order_relaxed) + 1;
        if (!svc_trace && !ShouldKeepHotEvent(count)) {
            return;
        }
    }

    const std::string cat{raw_cat};
    std::scoped_lock lk{g_mutex};
    if (svc_trace) {
        detail_text.assign(detail.begin(), detail.end());
        g_recent_svc_events.push_back(NowMs() + " " + cat + " " + detail_text);
        while (g_recent_svc_events.size() > MaxRecentSvcEvents) {
            g_recent_svc_events.pop_front();
        }
    }

    if (hot) {
        g_counters[cat] = count;
    } else {
        count = ++g_counters[cat];
    }

    if (detail_text.empty() && !detail.empty()) {
        detail_text.assign(detail.begin(), detail.end());
    }
    if (hot && !ShouldKeepHotEvent(count)) {
        g_dirty = true;
        return;
    }
    g_recent_events.push_back(NowMs() + " " + cat + " " + detail_text);
#ifdef __ANDROID__
#ifndef NXEMU_ANDROID_FULL_DIAG
    if (!hot || g_full_trace.load(std::memory_order_relaxed)) {
#endif
        __android_log_print(ANDROID_LOG_INFO, "NxEmuHleDiag", "%s %s", cat.c_str(),
                            detail_text.c_str());
#ifndef NXEMU_ANDROID_FULL_DIAG
    }
#endif
#endif
    while (g_recent_events.size() > MaxRecentEvents) {
        g_recent_events.pop_front();
    }
    g_dirty = true;
}

void RecordEvent(const char* category, const std::string& detail) {
    RecordEventImpl(category, detail);
}

void RecordEvent(const char* category, const char* detail) {
    RecordEventImpl(category, detail != nullptr ? std::string_view{detail} : std::string_view{});
}

std::string Snapshot() {
    std::scoped_lock lk{g_mutex};
    if (g_dirty) {
        RebuildSnapshotLocked();
        g_dirty = false;
    }
    return g_cached_snapshot;
}

void Clear() {
    std::scoped_lock lk{g_mutex};
    g_recent_events.clear();
    g_recent_svc_events.clear();
    g_counters.clear();
    for (auto& counter : g_hot_counters) {
        counter.store(0, std::memory_order_relaxed);
    }
    g_cached_snapshot = "osServiceDiagnostics=cleared";
    g_dirty = false;
}

void SetFullTrace(bool enabled) {
    g_full_trace.store(enabled, std::memory_order_relaxed);
}

bool IsFullTraceEnabled() {
    return g_full_trace.load(std::memory_order_relaxed);
}

void SetSvcRingEnabled(bool enabled) {
    g_svc_ring_enabled.store(enabled, std::memory_order_relaxed);
}

bool IsSvcRingEnabled() {
    return g_svc_ring_enabled.load(std::memory_order_relaxed);
}

} // namespace Service::NxemuAndroidDiagnostics

extern "C" const char* NxemuGetLastOsServiceDiagnostics() {
    static thread_local std::string snapshot;
    snapshot = Service::NxemuAndroidDiagnostics::Snapshot();
    return snapshot.c_str();
}

