// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once
#include "yuzu_video_core/native_alias_snapshot.h"
#include "yuzu_video_core/backing_identity.h"
#include "yuzu_video_core/native_debug_document.h"
#include <sstream>

#include <set>
#include <array>
#include <atomic>
#include <bit>
#include <deque>
#include <memory>
#include <mutex>

#include "yuzu_common/nvdata.h"
#include "yuzu_common/common_types.h"
#include "yuzu_common/range_mutex.h"
#include "yuzu_common/scratch_buffer.h"
#include "yuzu_common/virtual_buffer.h"
#include "yuzu_video_core/flush_region_cache_filter.h"

#include <nxemu-module-spec/cpu.h>
#include <nxemu-module-spec/operating_system.h>

namespace Core {

constexpr size_t DEVICE_PAGEBITS = 12ULL;
constexpr size_t DEVICE_PAGESIZE = 1ULL << DEVICE_PAGEBITS;
constexpr size_t DEVICE_PAGEMASK = DEVICE_PAGESIZE - 1ULL;

class DeviceMemory;

template <typename DTraits>
struct DeviceMemoryManagerAllocator;

template <typename Traits>
class DeviceMemoryManager {
    using DeviceInterface = typename Traits::DeviceInterface;
    using DeviceMethods = typename Traits::DeviceMethods;

public:
    DeviceMemoryManager(const IDeviceMemory & device_memory);
    ~DeviceMemoryManager();

    void BindInterface(DeviceInterface* device_inter);

    VideoCommon::BackingIdentity CapturedBacking() const {
        return {physical_base, captured_backing_size};
    }
    VideoCommon::BackingIdentity CurrentBacking() const {
        return {reinterpret_cast<uintptr_t>(source_backing.BackingBasePointer()),
                source_backing.BackingSize()};
    }
    bool BackingMatches() const { return CapturedBacking().Matches(CurrentBacking()); }
    void PublishBackingState(const char* stage) const {
        if (!VideoCommon::DebugDocument::Enabled()) return;
        const auto captured = CapturedBacking();
        const auto current = CurrentBacking();
        std::ostringstream doc;
        doc << "{\"stage\":" << VideoCommon::DebugDocument::Quote(stage)
            << ",\"captured_base\":" << captured.base << ",\"captured_size\":" << captured.size
            << ",\"current_base\":" << current.base << ",\"current_size\":" << current.size
            << ",\"physical_page_capacity\":" << compressed_device_addr.size()
            << ",\"matches\":" << (captured.Matches(current) ? "true" : "false") << '}';
        VideoCommon::DebugDocument::Publish("device-backing", doc.str());
    }

    DAddr Allocate(size_t size);
    void AllocateFixed(DAddr start, size_t size);
    void Free(DAddr start, size_t size);

    void Map(DAddr address, VAddr virtual_address, size_t size, Asid asid, bool track = false);

    void Unmap(DAddr address, size_t size);

    void TrackContinuityImpl(DAddr address, VAddr virtual_address, size_t size, Asid asid);
    void TrackContinuity(DAddr address, VAddr virtual_address, size_t size, Asid asid) {
        std::scoped_lock lk(mapping_guard);
        TrackContinuityImpl(address, virtual_address, size, asid);
    }

    // Write / Read
    template <typename T>
    T* GetPointer(DAddr address);

    template <typename T>
    const T* GetPointer(DAddr address) const;

    template <typename Func>
    void ApplyOpOnPAddr(PAddr address, Common::ScratchBuffer<u32>& buffer, Func&& operation) {
        DAddr subbits = static_cast<DAddr>(address & page_mask);
        if ((address >> page_bits) >= compressed_device_addr.size()) {
            ReportInvalidPhysicalAlias(address, address >> page_bits, compressed_device_addr.size());
            return;
        }
        const u32 base = compressed_device_addr[(address >> page_bits)];
        if ((base >> MULTI_FLAG_BITS) == 0) [[likely]] {
            if (base == 0) return;
            if (base >= (device_as_size >> page_bits)) {
                ReportInvalidPhysicalAlias(address, base, device_as_size >> page_bits);
                return;
            }
            const DAddr d_address = (static_cast<DAddr>(base) << page_bits) + subbits;
            operation(d_address);
            return;
        }
        InnerGatherDeviceAddresses(buffer, address);
        VideoCommon::ForEachStablePhysicalAlias(buffer, device_as_size >> page_bits,
            [&](u32 value) { operation((static_cast<DAddr>(value) << page_bits) + subbits); },
            [&](u32 value) { ReportInvalidPhysicalAlias(address, value, device_as_size >> page_bits); });
    }

    template <typename Func>
    void ApplyOpOnPointer(const u8* p, Common::ScratchBuffer<u32>& buffer, Func&& operation) {
        PAddr address = GetRawPhysicalAddr<u8>(p);
        ApplyOpOnPAddr(address, buffer, operation);
    }

    PAddr GetPhysicalRawAddressFromDAddr(DAddr address) const {
        PAddr subbits = static_cast<PAddr>(address & page_mask);
        auto paddr = compressed_physical_ptr[(address >> page_bits)];
        if (paddr == 0) {
            return 0;
        }
        return (static_cast<PAddr>(paddr - 1) << page_bits) + subbits;
    }

    template <typename T>
    void Write(DAddr address, T value);

    template <typename T>
    T Read(DAddr address) const;

    u8* GetSpan(const DAddr src_addr, const std::size_t size);
    const u8* GetSpan(const DAddr src_addr, const std::size_t size) const;

    void ReadBlock(DAddr address, void* dest_pointer, size_t size);
    void ReadBlockUnsafe(DAddr address, void* dest_pointer, size_t size);
    void WriteBlock(DAddr address, const void* src_pointer, size_t size);
    void WriteBlockUnsafe(DAddr address, const void* src_pointer, size_t size);
    void WriteBlockForGpuReadback(DAddr address, const void* src_pointer, size_t size);

    Asid RegisterProcess(IMemory * memory);
    void UnregisterProcess(Asid id);

    void UpdatePagesCachedCount(DAddr addr, size_t size, s32 delta);
    void UpdatePagesReadCachedCount(DAddr addr, size_t size, s32 delta);
    // Called on the serialized GPU producer, under its execution lock.
    void RegisterNativeRead(DAddr addr, size_t size);
    void InvalidateNativeReadRegion(DAddr addr, size_t size);

    // Presence remains conservative across deletion, remapping and address reuse.
    void RegisterFlushCacheRegion(DAddr addr, u64 size, VideoCommon::CacheType types) {
        flush_cache_filter.Register(addr, size, types);
    }

    [[nodiscard]] VideoCommon::CacheType FilterFlushCaches(
        DAddr addr, u64 size, VideoCommon::CacheType requested) const {
        return flush_cache_filter.Filter(addr, size, requested);
    }

    static constexpr size_t AS_BITS = Traits::device_virtual_bits;

private:
    void ReportInvalidPhysicalAlias(PAddr physical, u64 page, u64 limit) const {
        thread_local VideoCommon::PhysicalAliasDiagnosticGate gate;
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        if (!gate.Observe(static_cast<u64>(now))) return;
        if (!VideoCommon::DebugDocument::Enabled()) return;
        std::ostringstream doc;
        doc << "{\"physical_address\":" << physical << ",\"rejected_page\":" << page
            << ",\"page_limit\":" << limit << ",\"physical_base\":" << physical_base
            << ",\"captured_backing_size\":" << captured_backing_size
            << ",\"current_backing_base\":" << CurrentBacking().base
            << ",\"current_backing_size\":" << CurrentBacking().size
            << ",\"backing_matches\":" << (BackingMatches() ? "true" : "false")
            << ",\"rejected_count\":" << gate.rejected_count
            << ",\"published_count\":" << gate.published_count
            << ",\"sampling_interval_ms\":250,\"reason\":\"outside_address_table\"}";
        VideoCommon::DebugDocument::Publish("physical-alias", doc.str());
    }
    static constexpr size_t device_virtual_bits = Traits::device_virtual_bits;
    static constexpr size_t device_as_size = 1ULL << device_virtual_bits;
    static constexpr size_t page_bits = 12;
    static constexpr size_t page_size = 1ULL << page_bits;
    static constexpr size_t page_mask = page_size - 1ULL;
    static constexpr u32 physical_address_base = 1U << page_bits;
    static constexpr u32 MULTI_FLAG_BITS = 31;
    static constexpr u32 MULTI_FLAG = 1U << MULTI_FLAG_BITS;
    static constexpr u32 MULTI_MASK = ~MULTI_FLAG;

    template <typename T>
    T* GetPointerFromRaw(PAddr addr) {
        return reinterpret_cast<T*>(physical_base + addr);
    }

    template <typename T>
    const T* GetPointerFromRaw(PAddr addr) const {
        return reinterpret_cast<T*>(physical_base + addr);
    }

    template <typename T>
    PAddr GetRawPhysicalAddr(const T* ptr) const {
        return static_cast<PAddr>(reinterpret_cast<uintptr_t>(ptr) - physical_base);
    }

    void WalkBlock(const DAddr addr, const std::size_t size, auto on_unmapped, auto on_memory,
                   auto increment);

    void InnerGatherDeviceAddresses(Common::ScratchBuffer<u32>& buffer, PAddr address);

    std::unique_ptr<DeviceMemoryManagerAllocator<Traits>> impl;

    const IDeviceMemory& source_backing;
    const uintptr_t physical_base;
    const uint64_t captured_backing_size;
    DeviceInterface* device_inter;
    Common::VirtualBuffer<u32> compressed_physical_ptr;
    Common::VirtualBuffer<u32> compressed_device_addr;
    Common::VirtualBuffer<u32> continuity_tracker;

    // Process memory interfaces

    std::deque<size_t> id_pool;
    std::deque<IMemory*> registered_processes;
    std::mutex process_guard;
    IMemory* GetRegisteredProcess(Asid asid);

    // Memory protection management

    static constexpr size_t guest_max_as_bits = 39;
    static constexpr size_t guest_as_size = 1ULL << guest_max_as_bits;
    static constexpr size_t guest_mask = guest_as_size - 1ULL;
    static constexpr size_t asid_start_bit = guest_max_as_bits;

    std::pair<Asid, VAddr> ExtractCPUBacking(size_t page_index) {
        auto content = cpu_backing_address[page_index];
        const VAddr address = content & guest_mask;
        const Asid asid{static_cast<size_t>(content >> asid_start_bit)};
        return std::make_pair(asid, address);
    }

    void InsertCPUBacking(size_t page_index, VAddr address, Asid asid) {
        cpu_backing_address[page_index] = address | (asid.id << asid_start_bit);
    }

    Common::VirtualBuffer<VAddr> cpu_backing_address;
    using CounterType = u8;
    using CounterAtomicType = std::atomic_uint8_t;
    static constexpr size_t subentries = 8 / sizeof(CounterType);
    static constexpr size_t subentries_mask = subentries - 1;
    static constexpr size_t subentries_shift =
        std::countr_zero(sizeof(u64)) - std::countr_zero(sizeof(CounterType));
    class CounterEntry final {
    public:
        CounterEntry() = default;

        CounterAtomicType& Count(std::size_t page) {
            return values[page & subentries_mask];
        }

        const CounterAtomicType& Count(std::size_t page) const {
            return values[page & subentries_mask];
        }

    private:
        std::array<CounterAtomicType, subentries> values{};
    };
    static_assert(sizeof(CounterEntry) == subentries * sizeof(CounterType),
                  "CounterEntry should be 8 bytes!");

    static constexpr size_t num_counter_entries =
        (1ULL << (device_virtual_bits - page_bits)) / subentries;
    using CachedPages = std::array<CounterEntry, num_counter_entries>;
    std::unique_ptr<CachedPages> cached_pages;
    VideoCommon::FlushRegionCacheFilter<Traits::device_virtual_bits> flush_cache_filter;
    // Physical pages opened by a completed native readback. Only actual GPU image
    // writes consume these observers; cache-management commands leave them alone.
    std::set<size_t> native_read_pages;
    Common::RangeMutex counter_guard;
    std::mutex mapping_guard;
};

} // namespace Core
