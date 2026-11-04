// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>
#include "yuzu_shader_recompiler/frontend/ir/program.h"
#include "yuzu_shader_recompiler/object_pool.h"

namespace Shader::SsaFixture {
using Bytes = std::vector<uint8_t>;
inline constexpr size_t MaxBytes = 64 * 1024 * 1024;
inline constexpr uint32_t MaxBlocks = 8192;
inline constexpr uint32_t MaxInsts = 524288;

struct IdentityMap {
    std::unordered_map<const IR::Block*, uint32_t> blocks;
    std::unordered_map<const IR::Inst*, uint32_t> insts;
    std::vector<IR::Block*> block_ids;
    std::vector<IR::Inst*> inst_ids;
};

// SSA-only snapshot: Info and abstract syntax are intentionally not serialized.
// phase 0 = preSSA, 1 = postSSA. Decode never runs a pass.
Bytes Encode(IR::Program& program, IdentityMap& ids, uint32_t phase);
struct Replay {
    ObjectPool<IR::Inst> inst_pool{1024};
    ObjectPool<IR::Block> block_pool{64};
    IR::Program program;
    IdentityMap ids;
    uint32_t phase{};
};
std::unique_ptr<Replay> Decode(std::span<const uint8_t> bytes);
} // namespace Shader::SsaFixture
