// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>

namespace VideoCommon {
enum class ReadbackReason : unsigned {
    Unspecified, GarbageCollection, CpuSynchronous, NativeCpuSynchronous,
    FlushOverlap, ObservedPartial, AsyncFull, AsyncSparse, AsyncFallback,
    ImageToGuestBuffer, UnmapPreserve, Count
};
inline constexpr std::array<const char*,11> ReadbackReasonNames{
    "unspecified","gc","cpu_sync","native_cpu_sync","flush_overlap",
    "observed_partial","async_full","async_sparse","async_fallback","image_to_guest_buffer","unmap_preserve"};
enum class ReadbackDestination { Unknown, Host, Device, HostLast };
struct ReadbackContext { ReadbackReason reason{}; ReadbackDestination destination{}; };
inline thread_local ReadbackContext current_readback_context{};
class ReadbackReasonScope {
public:
    explicit ReadbackReasonScope(ReadbackReason reason,
        ReadbackDestination destination=ReadbackDestination::Host)
        : previous(current_readback_context) { current_readback_context={reason,destination}; }
    ~ReadbackReasonScope() { current_readback_context=previous; }
    ReadbackReasonScope(const ReadbackReasonScope&)=delete;
private:
    ReadbackContext previous;
};
inline std::optional<std::uint64_t> ReadbackRegionBytes(std::uint64_t width,
    std::uint64_t height,std::uint64_t depth,std::uint64_t layers,
    std::uint64_t block_width,std::uint64_t block_height,std::uint64_t bytes_per_block) {
    if (!width || !height || !depth || !layers || !block_width || !block_height || !bytes_per_block)
        return std::nullopt;
    const std::array factors{width/block_width+(width%block_width!=0),
        height/block_height+(height%block_height!=0),depth,layers,bytes_per_block};
    std::uint64_t result=1;
    for(auto value:factors) {
        if(result>std::numeric_limits<std::uint64_t>::max()/value)return std::nullopt;
        result*=value;
    }
    return result;
}
struct ReadbackTransferSample {
    std::uint64_t image_address{},write_tick{},width{},height{},layers{},format{},regions{},buffers{};
    std::uint64_t host_bytes{},device_bytes{},unknown_destination_bytes{},buffer_span_bytes{};
    std::uint64_t host_unknown_regions{},device_unknown_regions{},unknown_destination_regions{};
    bool native{},rescaled{};
    ReadbackReason reason{};
};
struct ReadbackTotals {
    std::uint64_t calls{},regions{},host_bytes{},device_bytes{},unknown_destination_bytes{},buffer_span_bytes{};
    std::uint64_t host_unknown_regions{},device_unknown_regions{},unknown_destination_regions{},rescaled_calls{},native_calls{};
    void Add(const ReadbackTransferSample& s) {
        ++calls;regions+=s.regions;host_bytes+=s.host_bytes;device_bytes+=s.device_bytes;
        unknown_destination_bytes+=s.unknown_destination_bytes;buffer_span_bytes+=s.buffer_span_bytes;
        host_unknown_regions+=s.host_unknown_regions;device_unknown_regions+=s.device_unknown_regions;
        unknown_destination_regions+=s.unknown_destination_regions;
        rescaled_calls+=s.rescaled;native_calls+=s.native;
    }
};
struct ReadbackImageTotals {
    bool used{};
    std::uint64_t address{},format{},width{},height{},layers{},last_write_tick{},same_tick_calls{},same_tick_host_bytes{};
    ReadbackTotals totals;
};
struct ReadbackMetricsSnapshot {
    ReadbackTotals totals;
    std::array<ReadbackTotals,11> reasons{};
    std::array<ReadbackImageTotals,64> images{};
    std::uint64_t untracked_image_calls{};
};
class ReadbackMetrics {
public:
    // Fixed storage: no allocation or diagnostic file write per transfer.
    bool Record(const ReadbackTransferSample& s,std::uint64_t now_ms) {
        std::scoped_lock lock(mutex);
        data.totals.Add(s);data.reasons.at(static_cast<unsigned>(s.reason)).Add(s);
        ReadbackImageTotals* entry=nullptr;
        for(auto& image:data.images) {
            if(!image.used) { if(!entry)entry=&image; continue; }
            if(image.address==s.image_address && image.format==s.format && image.width==s.width &&
               image.height==s.height && image.layers==s.layers) {entry=&image;break;}
        }
        if(entry) {
            if(!entry->used) {
                entry->used=true;entry->address=s.image_address;entry->format=s.format;
                entry->width=s.width;entry->height=s.height;entry->layers=s.layers;
            } else if(entry->last_write_tick==s.write_tick) {
                ++entry->same_tick_calls;entry->same_tick_host_bytes+=s.host_bytes;
            }
            entry->last_write_tick=s.write_tick;entry->totals.Add(s);
        } else ++data.untracked_image_calls;
        if(!published || now_ms-last_publish_ms>=500) {
            published=true;last_publish_ms=now_ms;return true;
        }
        return false;
    }
    ReadbackMetricsSnapshot Snapshot() const {
        std::scoped_lock lock(mutex);return data;
    }
private:
    mutable std::mutex mutex;
    ReadbackMetricsSnapshot data;
    std::uint64_t last_publish_ms{};
    bool published{};
};
} // namespace VideoCommon
