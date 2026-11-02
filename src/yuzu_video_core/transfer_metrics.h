// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <mutex>
namespace VideoCommon {
enum class BufferTransferKind : unsigned { Unknown, AsyncDownload, SyncDownload,
    MappedUpload, InlineUpload, IndexUpload, GuestDeviceCopy, MergeDeviceCopy,
    SparseAssemble, SparseScatter, DifferentialLogical, Count };
inline constexpr std::array<const char*,11> BufferTransferNames{
    "unknown","async_download","sync_download","mapped_upload","inline_upload",
    "index_upload","guest_device_copy","merge_device_copy","sparse_assemble",
    "sparse_scatter","differential_logical"};
inline thread_local BufferTransferKind current_buffer_transfer_kind{};
class BufferTransferScope {
public:
    explicit BufferTransferScope(BufferTransferKind kind) : previous(current_buffer_transfer_kind) {
        current_buffer_transfer_kind=kind;
    }
    ~BufferTransferScope() { current_buffer_transfer_kind=previous; }
    BufferTransferScope(const BufferTransferScope&)=delete;
private:
    BufferTransferKind previous;
};
struct BufferTransferTotals { std::uint64_t calls{},regions{},bytes{},max_call_bytes{}; };
enum class BufferFallbackReason : unsigned { Disabled, TooSmall, CopyTooLarge, Unaligned,
    StorageRange, BufferTooLarge, Budget, EligibleButFallback, Count };
inline constexpr std::array<const char*,8> BufferFallbackNames{
    "disabled","too_small","copy_over_8mib","unaligned","storage_range",
    "buffer_over_128mib","shadow_budget","eligible_but_fallback"};
struct BufferFallbackTotals { std::uint64_t calls{},bytes{},max_copy_bytes{},max_buffer_bytes{}; };
struct ReadbackRetirementTotals { std::uint64_t calls{},logical_bytes{},changed_bytes{},chunks{},changed_chunks{}; };
struct ReadbackSegmentTotals { std::uint64_t calls{},segments{},logical_bytes{}; };
struct BufferTransferSnapshot {
    std::array<BufferTransferTotals,11> kinds{};
    std::array<BufferFallbackTotals,8> fallbacks{};
    ReadbackRetirementTotals retirement;
    ReadbackSegmentTotals segmented_plans;
    std::uint64_t publication_count{},diagnostic_cpu_ns{};
};
class BufferTransferMetrics {
public:
    void Record(BufferTransferKind kind,std::uint64_t regions,std::uint64_t bytes) {
        std::scoped_lock lock(mutex);
        auto& t=data.kinds.at(static_cast<unsigned>(kind));
        ++t.calls;t.regions+=regions;t.bytes+=bytes;
        if(bytes>t.max_call_bytes)t.max_call_bytes=bytes;
    }
    void RecordFallback(BufferFallbackReason reason,std::uint64_t bytes,std::uint64_t buffer_bytes) {
        std::scoped_lock lock(mutex);
        auto& t=data.fallbacks.at(static_cast<unsigned>(reason));++t.calls;t.bytes+=bytes;
        if(bytes>t.max_copy_bytes)t.max_copy_bytes=bytes;
        if(buffer_bytes>t.max_buffer_bytes)t.max_buffer_bytes=buffer_bytes;
    }
    void RecordRetirement(std::uint64_t logical,std::uint64_t changed,
                          std::uint64_t chunks,std::uint64_t changed_chunks) {
        std::scoped_lock lock(mutex);auto& t=data.retirement;
        ++t.calls;t.logical_bytes+=logical;t.changed_bytes+=changed;
        t.chunks+=chunks;t.changed_chunks+=changed_chunks;
    }
    void RecordSegments(std::uint64_t segments,std::uint64_t bytes) {
        std::scoped_lock lock(mutex);auto& t=data.segmented_plans;
        ++t.calls;t.segments+=segments;t.logical_bytes+=bytes;
    }
    bool Due(std::uint64_t ms) {
        std::scoped_lock lock(mutex);
        if(published && ms-last_publish_ms<500)return false;
        published=true;last_publish_ms=ms;++data.publication_count;return true;
    }
    void AddDiagnosticTime(std::uint64_t ns) {
        std::scoped_lock lock(mutex);data.diagnostic_cpu_ns+=ns;
    }
    BufferTransferSnapshot Snapshot() const { std::scoped_lock lock(mutex);return data; }
private:
    mutable std::mutex mutex;
    BufferTransferSnapshot data;
    bool published{};
    std::uint64_t last_publish_ms{};
};
} // namespace VideoCommon
