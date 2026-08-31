// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <utility>
#include <vector>

#include "core/hle/kernel/k_typed_address.h"
#include "core/hle/kernel/physical_memory.h"
#include "nxemu-module-spec/base.h"

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
struct CodeSet final : public IModuleInfo {
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

#if defined(_M_ARM64) || defined(ARCHITECTURE_arm64) || defined(__aarch64__)
    Segment& PatchSegment() {
        return patch_segment;
    }

    const Segment& PatchSegment() const {
        return patch_segment;
    }
#endif

    const uint8_t* Data(void) const override {
        return memory.empty() ? nullptr : memory.data();
    }

    uint32_t DataSize(void) const override {
        return static_cast<uint32_t>(memory.size());
    }

    uint64_t CodeSegmentAddr(void) const override { return CodeSegment().addr.GetValue(); }
    uint64_t CodeSegmentOffset(void) const override { return CodeSegment().offset; }
    uint64_t CodeSegmentSize(void) const override { return CodeSegment().size; }
    uint64_t RODataSegmentAddr(void) const override { return RODataSegment().addr.GetValue(); }
    uint64_t RODataSegmentOffset(void) const override { return RODataSegment().offset; }
    uint64_t RODataSegmentSize(void) const override { return RODataSegment().size; }
    uint64_t DataSegmentAddr(void) const override { return DataSegment().addr.GetValue(); }
    uint64_t DataSegmentOffset(void) const override { return DataSegment().offset; }
    uint64_t DataSegmentSize(void) const override { return DataSegment().size; }
    uint64_t PatchSegmentAddr(void) const override {
#if defined(_M_ARM64) || defined(ARCHITECTURE_arm64) || defined(__aarch64__)
        return PatchSegment().addr.GetValue();
#else
        return 0;
#endif
    }
    uint64_t PatchSegmentOffset(void) const override {
#if defined(_M_ARM64) || defined(ARCHITECTURE_arm64) || defined(__aarch64__)
        return PatchSegment().offset;
#else
        return 0;
#endif
    }
    uint64_t PatchSegmentSize(void) const override {
#if defined(_M_ARM64) || defined(ARCHITECTURE_arm64) || defined(__aarch64__)
        return PatchSegment().size;
#else
        return 0;
#endif
    }
    uint32_t PatchPostHandlerCount(void) const override {
#if defined(_M_ARM64) || defined(ARCHITECTURE_arm64) || defined(__aarch64__)
        return static_cast<uint32_t>(patch_post_handlers.size());
#else
        return 0;
#endif
    }
    bool PatchPostHandler(uint32_t index, uint64_t * module_pc, uint64_t * patch_pc) const override {
#if defined(_M_ARM64) || defined(ARCHITECTURE_arm64) || defined(__aarch64__)
        if (index >= patch_post_handlers.size() || module_pc == nullptr || patch_pc == nullptr) {
            return false;
        }
        *module_pc = patch_post_handlers[index].first;
        *patch_pc = patch_post_handlers[index].second;
        return true;
#else
        (void)index; (void)module_pc; (void)patch_pc; return false;
#endif
    }

    /// The overall data that backs this code set.
    Kernel::PhysicalMemory memory;

    /// The segments that comprise this code set.
    std::array<Segment, 3> segments;

#if defined(_M_ARM64) || defined(ARCHITECTURE_arm64) || defined(__aarch64__)
    Segment patch_segment;
    std::vector<std::pair<u64, u64>> patch_post_handlers;
#endif

    /// The entry point address for this code set.
    KProcessAddress entrypoint = 0;
};

} // namespace Kernel
