// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <utility>
#include <vector>

#include "core/hle/kernel/k_typed_address.h"
#include "core/hle/kernel/physical_memory.h"
#include <nxemu-module-spec/operating_system.h>

namespace Kernel {

/**
 * Represents executable data that may be loaded into a kernel process.
 *
 * A code set consists of three basic segments:
 *   - A code (AKA text) segment,
 *   - A read-only data segment (rodata)
 *   - A data segment
 *
 * The code segment is the portion of the object file that contains
 * executable instructions.
 *
 * The read-only data segment in the portion of the object file that
 * contains (as one would expect) read-only data, such as fixed constant
 * values and data structures.
 *
 * The data segment is similar to the read-only data segment -- it contains
 * variables and data structures that have predefined values, however,
 * entities within this segment can be modified.
 */
struct CodeSet :
    public IModuleInfo
{
    /// A single segment within a code set.
    struct Segment final {
        /// The byte offset that this segment is located at.
        std::size_t offset = 0;

        /// The address to map this segment to.
        KProcessAddress addr = 0;

        /// The size of this segment in bytes.
        u32 size = 0;
    };

    explicit CodeSet();
    ~CodeSet();

    CodeSet(const CodeSet&) = delete;
    CodeSet& operator=(const CodeSet&) = delete;

    CodeSet(CodeSet&&) = default;
    CodeSet& operator=(CodeSet&&) = default;

    const uint8_t * Data(void) const
    {
        return memory.data();
    }
    uint32_t DataSize(void) const
    {
        return (uint32_t)memory.size();
    }
    uint64_t CodeSegmentAddr(void) const
    {
        return segments[0].addr.GetValue();
    }
    uint64_t CodeSegmentOffset(void) const
    {
        return segments[0].offset;
    }
    uint64_t CodeSegmentSize(void) const
    {
        return segments[0].size;
    }
    uint64_t RODataSegmentAddr(void) const
    {
        return segments[1].addr.GetValue();
    }
    uint64_t RODataSegmentOffset(void) const
    {
        return segments[1].offset;
    }
    uint64_t RODataSegmentSize(void) const
    {
        return segments[1].size;
    }
    uint64_t DataSegmentAddr(void) const
    {
        return segments[2].addr.GetValue();
    }
    uint64_t DataSegmentOffset(void) const
    {
        return segments[2].offset;
    }
    uint64_t DataSegmentSize(void) const
    {
        return segments[2].size;
    }

    Segment& CodeSegment() {
        return segments[0];
    }

    const Segment& CodeSegment() const {
        return segments[0];
    }

    Segment& RODataSegment() {
        return segments[1];
    }

    const Segment& RODataSegment() const {
        return segments[1];
    }

    Segment& DataSegment() {
        return segments[2];
    }

    const Segment& DataSegment() const {
        return segments[2];
    }

    Segment& PatchSegment() {
        return patch_segment;
    }

    const Segment& PatchSegment() const {
        return patch_segment;
    }

    uint64_t PatchSegmentAddr(void) const override {
        return patch_segment.addr.GetValue();
    }
    uint64_t PatchSegmentOffset(void) const override {
        return patch_segment.offset;
    }
    uint64_t PatchSegmentSize(void) const override {
        return patch_segment.size;
    }

    uint32_t PatchPostHandlerCount(void) const override {
        return static_cast<uint32_t>(patch_post_handlers.size());
    }
    bool PatchPostHandler(uint32_t index, uint64_t * module_pc, uint64_t * patch_pc) const override {
        if (index >= patch_post_handlers.size() || module_pc == nullptr || patch_pc == nullptr) {
            return false;
        }
        *module_pc = patch_post_handlers[index].first;
        *patch_pc = patch_post_handlers[index].second;
        return true;
    }

    /// The overall data that backs this code set.
    Kernel::PhysicalMemory memory;

    /// The segments that comprise this code set.
    std::array<Segment, 3> segments;

    Segment patch_segment;
    std::vector<std::pair<u64, u64>> patch_post_handlers;

    /// The entry point address for this code set.
    KProcessAddress entrypoint = 0;
};

} // namespace Kernel
