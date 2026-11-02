// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cstdint>
#include <limits>

namespace Common {
struct ReadbackSegment { std::uint64_t src_offset{},dst_offset{},size{}; };
// Validate BOTH complete ranges before emitting any segment. Preserve byte count,
// source/staging continuity and callback FIFO order, including a short final tail.
template<class Emit>
bool ForEachReadbackSegment(std::uint64_t src,std::uint64_t dst,std::uint64_t size,
                           std::uint64_t limit,Emit&& emit) {
    constexpr auto max=std::numeric_limits<std::uint64_t>::max();
    if(!limit || size>max-src || size>max-dst)return false;
    for(std::uint64_t offset=0;offset<size;) {
        const auto bytes=std::min(limit,size-offset);
        emit(ReadbackSegment{src+offset,dst+offset,bytes});
        offset+=bytes;
    }
    return true;
}
struct ReadbackRetiredPayload {
    std::uint64_t logical_bytes{},changed_bytes{},chunks{},changed_chunks{};
    void AddChunk(std::uint64_t bytes,bool changed) {
        logical_bytes+=bytes;++chunks;
        if(changed){changed_bytes+=bytes;++changed_chunks;}
    }
};
} // namespace Common
