// SPDX-License-Identifier: GPL-2.0-or-later
#include "yuzu_shader_recompiler/ssa_fixture.h"

#include <algorithm>
#include <bit>
#include <climits>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <unordered_set>

namespace Shader::IR {
// This bridge deliberately does not use Resolve, SetArg, Use or AddBranch.
// Those APIs change identity, use counts, pseudo links or predecessor ordering.
struct FixtureAccess {
    static auto Tag(const Value& v) { return v.type; }
    static Inst* Ref(const Value& v) { return v.type == Type::Opaque ? v.inst : nullptr; }
    static uint64_t Payload(const Value& v) {
        switch (v.type) {
        case Type::Void: return 0;
        case Type::Reg: return static_cast<uint64_t>(v.reg);
        case Type::Pred: return static_cast<uint64_t>(v.pred);
        case Type::Attribute: return static_cast<uint64_t>(v.attribute);
        case Type::Patch: return static_cast<uint64_t>(v.patch);
        case Type::U1: return v.imm_u1;
        case Type::U8: return v.imm_u8;
        case Type::U16: case Type::F16: return v.imm_u16;
        case Type::U32: return v.imm_u32;
        case Type::U64: return v.imm_u64;
        case Type::F32: return std::bit_cast<uint32_t>(v.imm_f32);
        case Type::F64: return std::bit_cast<uint64_t>(v.imm_f64);
        default: throw std::runtime_error("SSA fixture: unsupported raw value tag");
        }
    }
    static Value Raw(Type t, uint64_t p, Inst* ref) {
        Value v;
        v.type = t;
        switch (t) {
        case Type::Void: break;
        case Type::Opaque: v.inst = ref; break;
        case Type::Reg: v.reg = static_cast<IR::Reg>(p); break;
        case Type::Pred: v.pred = static_cast<IR::Pred>(p); break;
        case Type::Attribute: v.attribute = static_cast<IR::Attribute>(p); break;
        case Type::Patch: v.patch = static_cast<IR::Patch>(p); break;
        case Type::U1: v.imm_u1 = p != 0; break;
        case Type::U8: v.imm_u8 = static_cast<u8>(p); break;
        case Type::U16: case Type::F16: v.imm_u16 = static_cast<u16>(p); break;
        case Type::U32: v.imm_u32 = static_cast<u32>(p); break;
        case Type::U64: v.imm_u64 = p; break;
        case Type::F32: v.imm_f32 = std::bit_cast<float>(static_cast<u32>(p)); break;
        case Type::F64: v.imm_f64 = std::bit_cast<double>(p); break;
        default: throw std::runtime_error("SSA fixture: unsupported raw value tag");
        }
        return v;
    }
    static auto& Args(Inst& i) { return i.args; }
    static auto& Phi(Inst& i) { return i.phi_args; }
    static auto& Assoc(Inst& i) { return i.associated_insts; }
    static int& Uses(Inst& i) { return i.use_count; }
    static u32& Flags(Inst& i) { return i.flags; }
    static u32& Definition(Inst& i) { return i.definition; }
    static auto& Preds(Block& b) { return b.imm_predecessors; }
    static auto& Succs(Block& b) { return b.imm_successors; }
    static auto& Cache(Block& b) { return b.ssa_reg_values; }
    static bool& Sealed(Block& b) { return b.is_ssa_sealed; }
    static u32& Order(Block& b) { return b.order; }
    static u32& Definition(Block& b) { return b.definition; }
};
} // namespace Shader::IR

namespace Shader::SsaFixture {
namespace {
using A = IR::FixtureAccess;
constexpr uint32_t Null = std::numeric_limits<uint32_t>::max();
constexpr std::string_view Magic = "NXSSA001";
constexpr std::array PseudoOps{IR::Opcode::GetZeroFromOp, IR::Opcode::GetSignFromOp,
    IR::Opcode::GetCarryFromOp, IR::Opcode::GetOverflowFromOp,
    IR::Opcode::GetSparseFromOp, IR::Opcode::GetInBoundsFromOp};
[[noreturn]] void Fail(const char* what) {
    throw std::runtime_error(std::string("SSA fixture: ") + what);
}
void Require(bool ok, const char* what) { if (!ok) Fail(what); }

uint64_t Fingerprint() {
    uint64_t h = 14695981039346656037ULL;
    auto byte = [&](uint8_t v) { h = (h ^ v) * 1099511628211ULL; };
    auto number = [&](uint64_t n) { for (unsigned j = 0; j != 8; ++j) byte(static_cast<uint8_t>(n >> (8*j))); };
    auto name = [&](std::string_view s) { number(s.size()); for (char c : s) byte(static_cast<uint8_t>(c)); };
    name("NXSSA001 enum-table v1");
    number(std::size(IR::Detail::META_TABLE));
    for (size_t n = 0; n < std::size(IR::Detail::META_TABLE); ++n) {
        const auto& m = IR::Detail::META_TABLE[n];
        number(n); name(m.name); number(static_cast<uint32_t>(m.type));
        for (auto t : m.arg_types) number(static_cast<uint32_t>(t));
    }
    // Include even raw scalar/vector tags that no current opcode happens to use.
    constexpr IR::Type types[]{IR::Type::Void, IR::Type::Opaque, IR::Type::Reg,
        IR::Type::Pred, IR::Type::Attribute, IR::Type::Patch, IR::Type::U1,
        IR::Type::U8, IR::Type::U16, IR::Type::U32, IR::Type::U64, IR::Type::F16,
        IR::Type::F32, IR::Type::F64, IR::Type::U32x2, IR::Type::U32x3, IR::Type::U32x4,
        IR::Type::F16x2, IR::Type::F16x3, IR::Type::F16x4, IR::Type::F32x2,
        IR::Type::F32x3, IR::Type::F32x4, IR::Type::F64x2, IR::Type::F64x3, IR::Type::F64x4};
    for (auto t : types) number(static_cast<uint32_t>(t));
    number(static_cast<uint32_t>(IR::Reg::RZ));
    number(static_cast<uint32_t>(IR::Pred::PT));
    number(static_cast<uint64_t>(IR::Attribute::DrawID));
    number(static_cast<uint64_t>(IR::Patch::Component119));
    for (auto s : {Stage::VertexB, Stage::TessellationControl, Stage::TessellationEval,
                   Stage::Geometry, Stage::Fragment, Stage::Compute, Stage::VertexA}) number(static_cast<uint32_t>(s));
    for (auto t : {OutputTopology::PointList, OutputTopology::LineStrip, OutputTopology::TriangleStrip}) number(static_cast<uint32_t>(t));
    return h;
}
struct Writer {
    Bytes bytes;
    void N(uint64_t n, unsigned width) {
        Require(bytes.size() <= MaxBytes-width, "byte budget exceeded");
        for (unsigned i = 0; i < width; ++i) bytes.push_back(static_cast<uint8_t>(n >> (8*i)));
    }
    void U(uint32_t n) { N(n, 4); }
    void Q(uint64_t n) { N(n, 8); }
};
struct Reader {
    std::span<const uint8_t> bytes;
    size_t pos{};
    uint64_t N(unsigned width) {
        Require(width <= bytes.size()-pos, "truncated input");
        uint64_t n{};
        for (unsigned i = 0; i < width; ++i) n |= uint64_t{bytes[pos++]} << (8*i);
        return n;
    }
    uint32_t U() { return static_cast<uint32_t>(N(4)); }
    uint64_t Q() { return N(8); }
    uint32_t Count(uint32_t cap, size_t min_bytes) {
        const auto n = U();
        Require(n <= cap && n <= (bytes.size()-pos)/min_bytes, "invalid count");
        return n;
    }
    bool Bool() { auto n = U(); Require(n <= 1, "invalid boolean"); return n != 0; }
};
uint32_t AddBlock(IdentityMap& ids, IR::Block* b) {
    Require(b != nullptr, "null block");
    if (auto it = ids.blocks.find(b); it != ids.blocks.end()) return it->second;
    Require(ids.block_ids.size() < MaxBlocks, "block budget exceeded");
    const auto n = static_cast<uint32_t>(ids.block_ids.size());
    ids.blocks.emplace(b, n); ids.block_ids.push_back(b); return n;
}
uint32_t AddInst(IdentityMap& ids, IR::Inst* i) {
    Require(i != nullptr, "null opaque instruction");
    if (auto it = ids.insts.find(i); it != ids.insts.end()) return it->second;
    Require(ids.inst_ids.size() < MaxInsts, "instruction budget exceeded");
    const auto n = static_cast<uint32_t>(ids.inst_ids.size());
    ids.insts.emplace(i, n); ids.inst_ids.push_back(i); return n;
}
std::array<IR::Inst*, 6> Pseudos(IR::Inst& i) {
    std::array<IR::Inst*, 6> result{};
    if (auto& assoc = A::Assoc(i); assoc) {
        auto* slot = assoc->zero_inst; // zero/sparse/inbounds share this slot
        if (slot) {
            size_t n = 0;
            if (slot->GetOpcode() == PseudoOps[4]) n = 4;
            else if (slot->GetOpcode() == PseudoOps[5]) n = 5;
            else Require(slot->GetOpcode() == PseudoOps[0], "invalid shared pseudo slot");
            result[n] = slot;
        }
        result[1] = assoc->sign_inst; result[2] = assoc->carry_inst; result[3] = assoc->overflow_inst;
    }
    for (size_t n = 0; n < result.size(); ++n)
        Require(!result[n] || result[n]->GetOpcode() == PseudoOps[n], "invalid pseudo opcode");
    return result;
}
void CheckIds(const IdentityMap& ids) {
    Require(ids.block_ids.size() <= MaxBlocks && ids.inst_ids.size() <= MaxInsts, "identity budget exceeded");
    Require(ids.blocks.size() == ids.block_ids.size() && ids.insts.size() == ids.inst_ids.size(), "identity map size mismatch");
    for (size_t n = 0; n < ids.block_ids.size(); ++n) {
        auto it = ids.blocks.find(ids.block_ids[n]);
        Require(ids.block_ids[n] && it != ids.blocks.end() && it->second == n, "block identity mismatch");
    }
    for (size_t n = 0; n < ids.inst_ids.size(); ++n) {
        auto it = ids.insts.find(ids.inst_ids[n]);
        Require(ids.inst_ids[n] && it != ids.insts.end() && it->second == n, "instruction identity mismatch");
    }
}
void Closure(IR::Program& p, IdentityMap& ids) {
    CheckIds(ids);
    for (auto* b : p.blocks) AddBlock(ids, b);
    for (auto* b : p.post_order_blocks) AddBlock(ids, b);
    auto value = [&](const IR::Value& v) { if (A::Tag(v) == IR::Type::Opaque) AddInst(ids, A::Ref(v)); };
    size_t bn{}, in{};
    while (bn < ids.block_ids.size() || in < ids.inst_ids.size()) {
        while (bn < ids.block_ids.size()) {
            auto* b = ids.block_ids[bn++];
            for (auto* x : A::Preds(*b)) AddBlock(ids, x);
            for (auto* x : A::Succs(*b)) AddBlock(ids, x);
            for (auto& i : b->Instructions()) AddInst(ids, &i);
            for (auto& v : A::Cache(*b)) value(v);
        }
        while (in < ids.inst_ids.size()) {
            auto* i = ids.inst_ids[in++];
            Require(static_cast<size_t>(i->GetOpcode()) < std::size(IR::Detail::META_TABLE), "invalid opcode");
            if (IR::IsPhi(*i)) {
                for (auto& [b, v] : A::Phi(*i)) { AddBlock(ids, b); value(v); }
            } else {
                // Preserve all five raw slots, not just current NumArgs.
                for (auto& v : A::Args(*i)) value(v);
            }
            for (auto* x : Pseudos(*i)) if (x) AddInst(ids, x);
        }
    }
}
void ValidateValue(IR::Type t, uint64_t p, uint32_t insts) {
    switch (t) {
    case IR::Type::Void: Require(p == 0, "nonzero void payload"); return;
    case IR::Type::Opaque: Require(p < insts, "invalid opaque ID"); return;
    case IR::Type::Reg: Require(p <= static_cast<uint64_t>(IR::Reg::RZ), "invalid register"); return;
    case IR::Type::Pred: Require(p <= static_cast<uint64_t>(IR::Pred::PT), "invalid predicate"); return;
    case IR::Type::Attribute: Require(p <= static_cast<uint64_t>(IR::Attribute::DrawID), "invalid attribute"); return;
    case IR::Type::Patch: Require(p <= static_cast<uint64_t>(IR::Patch::Component119), "invalid patch"); return;
    case IR::Type::U1: Require(p <= 1, "invalid U1"); return;
    case IR::Type::U8: Require(p <= 255, "invalid U8"); return;
    case IR::Type::U16: case IR::Type::F16: Require(p <= 65535, "invalid 16-bit payload"); return;
    case IR::Type::U32: case IR::Type::F32: Require(p <= UINT32_MAX, "invalid 32-bit payload"); return;
    case IR::Type::U64: case IR::Type::F64: return;
    default: Fail("unsupported raw value tag");
    }
}
void WriteValue(Writer& w, const IR::Value& v, const IdentityMap& ids) {
    const auto t = A::Tag(v);
    const auto p = t == IR::Type::Opaque ? uint64_t{ids.insts.at(A::Ref(v))} : A::Payload(v);
    ValidateValue(t, p, static_cast<uint32_t>(ids.inst_ids.size()));
    w.U(static_cast<uint32_t>(t)); w.Q(p);
}
struct RawValue { IR::Type type{}; uint64_t payload{}; };
RawValue ReadValue(Reader& r, uint32_t ni) {
    RawValue v{static_cast<IR::Type>(r.U()), r.Q()};
    ValidateValue(v.type, v.payload, ni); return v;
}
IR::Value RestoreValue(const RawValue& v, Replay& replay) {
    return A::Raw(v.type, v.payload, v.type == IR::Type::Opaque ? replay.ids.inst_ids[static_cast<size_t>(v.payload)] : nullptr);
}
template <typename T, typename Map>
void WriteList(Writer& w, const T& list, const Map& map, uint32_t cap) {
    Require(list.size() <= cap, "list budget exceeded");
    w.U(static_cast<uint32_t>(list.size()));
    std::unordered_set<uint32_t> seen;
    for (auto* x : list) { auto id = map.at(x); Require(seen.insert(id).second, "duplicate list member"); w.U(id); }
}
std::vector<uint32_t> ReadList(Reader& r, uint32_t cap) {
    auto n = r.Count(cap, 4);
    std::vector<uint32_t> list; list.reserve(n);
    std::unordered_set<uint32_t> seen;
    for (uint32_t j = 0; j < n; ++j) {
        auto id = r.U(); Require(id < cap && seen.insert(id).second, "invalid or duplicate list ID"); list.push_back(id);
    }
    return list;
}
struct BlockRecord {
    uint32_t order{}, definition{};
    bool sealed{};
    std::vector<uint32_t> members, preds, succs;
    std::vector<std::pair<uint32_t, RawValue>> cache;
};
struct InstRecord {
    IR::Opcode opcode{};
    uint32_t flags{}, uses{}, definition{};
    std::vector<RawValue> args;
    std::vector<uint32_t> phi_blocks;
    bool associated{};
    std::array<uint32_t, 6> pseudo{};
};
void ValidateGraph(const std::vector<BlockRecord>& blocks, const std::vector<InstRecord>& insts) {
    std::vector<uint32_t> owner(insts.size(), Null);
    for (uint32_t n = 0; n < blocks.size(); ++n) {
        const auto& b = blocks[n];
        for (auto id : b.members) { Require(owner[id] == Null, "multiple instruction memberships"); owner[id] = n; }
        for (auto id : b.preds) {
            auto& other = blocks[id].succs;
            Require(std::find(other.begin(), other.end(), n) != other.end(), "nonreciprocal predecessor");
        }
        for (auto id : b.succs) {
            auto& other = blocks[id].preds;
            Require(std::find(other.begin(), other.end(), n) != other.end(), "nonreciprocal successor");
        }
    }
    for (uint32_t n = 0; n < insts.size(); ++n) {
        const auto& i = insts[n];
        if (i.opcode == IR::Opcode::Phi && owner[n] != Null) {
            const auto& preds = blocks[owner[n]].preds;
            std::unordered_set<uint32_t> seen;
            for (auto b : i.phi_blocks)
                Require(seen.insert(b).second && std::find(preds.begin(), preds.end(), b) != preds.end(), "invalid phi predecessor membership");
        }
        unsigned shared{};
        for (size_t j = 0; j < i.pseudo.size(); ++j) {
            auto id = i.pseudo[j];
            if (id == Null) continue;
            Require(i.associated && id < insts.size() && insts[id].opcode == PseudoOps[j], "invalid pseudo association");
            if (j == 0 || j >= 4) ++shared;
            const auto& args = insts[id].args;
            Require(!args.empty() && args[0].type == IR::Type::Opaque && args[0].payload == n, "nonreciprocal pseudo association");
        }
        Require(shared <= 1, "overlapping shared pseudo associations");
    }
}
void ProgramFields(Writer& w, const IR::Program& p) {
    auto stage = static_cast<uint32_t>(p.stage), topology = static_cast<uint32_t>(p.output_topology);
    Require(stage <= static_cast<uint32_t>(Stage::VertexA), "invalid stage");
    Require(topology == 0 || topology == 1 || topology == 6 || topology == 7, "invalid output topology");
    w.U(stage); for (auto n : p.workgroup_size) w.U(n); w.U(topology);
    w.U(p.output_vertices); w.U(p.invocations); w.U(p.local_memory_size); w.U(p.shared_memory_size);
    w.U(p.shared_memory_16_to_12_packing); w.U(p.is_geometry_passthrough);
}
void ProgramFields(Reader& r, IR::Program& p) {
    auto stage = r.U(); Require(stage <= static_cast<uint32_t>(Stage::VertexA), "invalid stage"); p.stage = static_cast<Stage>(stage);
    for (auto& n : p.workgroup_size) n = r.U();
    auto t = r.U(); Require(t == 0 || t == 1 || t == 6 || t == 7, "invalid output topology"); p.output_topology = static_cast<OutputTopology>(t);
    p.output_vertices = r.U(); p.invocations = r.U(); p.local_memory_size = r.U(); p.shared_memory_size = r.U();
    p.shared_memory_16_to_12_packing = r.Bool(); p.is_geometry_passthrough = r.Bool();
}
struct Parsed { IR::Program program; uint32_t phase{}; std::vector<uint32_t> program_blocks, post_order; std::vector<BlockRecord> blocks; std::vector<InstRecord> insts; };
Parsed Parse(std::span<const uint8_t> bytes);
} // namespace

Bytes Encode(IR::Program& program, IdentityMap& ids, uint32_t phase) {
    Require(phase <= 1, "invalid phase");
    Closure(program, ids);
    Writer w;
    for (char c : Magic) w.N(static_cast<uint8_t>(c), 1);
    w.U(1); w.Q(Fingerprint()); w.U(phase);
    w.U(static_cast<uint32_t>(ids.block_ids.size())); w.U(static_cast<uint32_t>(ids.inst_ids.size()));
    ProgramFields(w, program);
    WriteList(w, program.blocks, ids.blocks, MaxBlocks);
    WriteList(w, program.post_order_blocks, ids.blocks, MaxBlocks);
    std::unordered_set<IR::Inst*> membership;
    for (auto* b : ids.block_ids) {
        w.U(A::Order(*b)); w.U(A::Sealed(*b)); w.U(A::Definition(*b));
        Require(b->size() <= MaxInsts, "membership budget exceeded"); w.U(static_cast<uint32_t>(b->size()));
        for (auto& i : b->Instructions()) {
            Require(membership.insert(&i).second, "multiple instruction memberships"); w.U(ids.insts.at(&i));
        }
        WriteList(w, A::Preds(*b), ids.blocks, MaxBlocks); WriteList(w, A::Succs(*b), ids.blocks, MaxBlocks);
        uint32_t count{};
        for (const auto& v : A::Cache(*b)) if (A::Tag(v) != IR::Type::Void) ++count;
        w.U(count);
        for (uint32_t n = 0; n < IR::NUM_REGS; ++n) {
            const auto& v = A::Cache(*b)[n];
            if (A::Tag(v) != IR::Type::Void) { w.U(n); WriteValue(w, v, ids); }
        }
    }
    for (auto* i : ids.inst_ids) {
        w.U(static_cast<uint32_t>(i->GetOpcode())); w.U(A::Flags(*i));
        Require(A::Uses(*i) >= 0, "negative use count"); w.U(static_cast<uint32_t>(A::Uses(*i))); w.U(A::Definition(*i));
        if (IR::IsPhi(*i)) {
            Require(A::Phi(*i).size() <= MaxBlocks, "phi budget exceeded"); w.U(static_cast<uint32_t>(A::Phi(*i).size()));
            for (const auto& [b,v] : A::Phi(*i)) { w.U(ids.blocks.at(b)); WriteValue(w, v, ids); }
        } else {
            w.U(5); for (const auto& v : A::Args(*i)) WriteValue(w, v, ids);
        }
        w.U(A::Assoc(*i) != nullptr);
        for (auto* x : Pseudos(*i)) w.U(x ? ids.insts.at(x) : Null);
    }
    // Use the same strict validator for capture and replay; no invalid fixture is published.
    // Capture's bounded allocation/copy overhead is intentionally explicit at this opt-in boundary.
    auto validated = Parse(w.bytes);
    return std::move(w.bytes);
}

namespace {
Parsed Parse(std::span<const uint8_t> bytes) {
    Require(bytes.size() <= MaxBytes, "byte budget exceeded");
    Reader r{bytes};
    for (char c : Magic) Require(r.N(1) == static_cast<uint8_t>(c), "invalid magic");
    Require(r.U() == 1, "unsupported version"); Require(r.Q() == Fingerprint(), "enum fingerprint mismatch");
    const auto phase = r.U(); Require(phase <= 1, "invalid phase");
    const auto nb = r.Count(MaxBlocks, 28), ni = r.Count(MaxInsts, 48);
    Parsed parsed; parsed.phase = phase;
    ProgramFields(r, parsed.program);
    auto program_blocks = ReadList(r, nb), post_order = ReadList(r, nb);
    std::vector<BlockRecord> blocks(nb);
    size_t total_members{};
    for (auto& b : blocks) {
        b.order = r.U(); b.sealed = r.Bool(); b.definition = r.U();
        b.members = ReadList(r, ni); total_members += b.members.size(); Require(total_members <= ni, "membership budget exceeded");
        b.preds = ReadList(r, nb); b.succs = ReadList(r, nb);
        auto n = r.Count(static_cast<uint32_t>(IR::NUM_REGS), 16);
        std::array<bool, IR::NUM_REGS> seen{};
        for (uint32_t j = 0; j < n; ++j) {
            auto reg = r.U(); Require(reg < IR::NUM_REGS && !seen[reg], "invalid cache index"); seen[reg] = true;
            auto v = ReadValue(r, ni); Require(v.type != IR::Type::Void, "void sparse cache entry"); b.cache.emplace_back(reg, v);
        }
    }
    std::vector<InstRecord> insts(ni);
    for (auto& i : insts) {
        auto op = r.U(); Require(op < std::size(IR::Detail::META_TABLE), "invalid opcode"); i.opcode = static_cast<IR::Opcode>(op);
        i.flags = r.U(); i.uses = r.U(); Require(i.uses <= INT_MAX, "invalid use count"); i.definition = r.U();
        const bool phi = i.opcode == IR::Opcode::Phi;
        auto n = r.Count(phi ? MaxBlocks : 5, phi ? 16 : 12); Require(phi || n == 5, "missing raw argument slots");
        i.args.reserve(n); if (phi) i.phi_blocks.reserve(n);
        for (uint32_t j = 0; j < n; ++j) {
            if (phi) { auto id = r.U(); Require(id < nb, "invalid phi block ID"); i.phi_blocks.push_back(id); }
            i.args.push_back(ReadValue(r, ni));
        }
        i.associated = r.Bool();
        for (auto& id : i.pseudo) { id = r.U(); Require(id == Null || id < ni, "invalid pseudo ID"); }
    }
    Require(r.pos == bytes.size(), "trailing bytes");
    ValidateGraph(blocks, insts);
    // Reject Identity-only cycles before any recursive production Resolve call.
    std::vector<uint8_t> state(insts.size());
    for (uint32_t n = 0; n < insts.size(); ++n) {
        if (state[n] || insts[n].opcode != IR::Opcode::Identity) continue;
        std::vector<uint32_t> path; auto at = n;
        while (insts[at].opcode == IR::Opcode::Identity && !state[at]) {
            state[at] = 1; path.push_back(at);
            const auto& v = insts[at].args[0];
            if (v.type != IR::Type::Opaque) break;
            at = static_cast<uint32_t>(v.payload);
            Require(state[at] != 1, "Identity-only cycle");
        }
        for (auto id : path) state[id] = 2;
    }
    parsed.blocks = std::move(blocks); parsed.insts = std::move(insts);
    parsed.program_blocks = std::move(program_blocks); parsed.post_order = std::move(post_order);
    return parsed;
}
} // namespace
std::unique_ptr<Replay> Decode(std::span<const uint8_t> bytes) {
    auto parsed = Parse(bytes);
    auto replay = std::make_unique<Replay>(); replay->phase = parsed.phase;
    replay->program = std::move(parsed.program);
    auto& blocks = parsed.blocks; auto& insts = parsed.insts;
    auto& program_blocks = parsed.program_blocks; auto& post_order = parsed.post_order;
    const auto nb = static_cast<uint32_t>(blocks.size()), ni = static_cast<uint32_t>(insts.size());
    for (const auto& i : insts) AddInst(replay->ids, replay->inst_pool.Create(i.opcode, i.flags));
    for (uint32_t n = 0; n < nb; ++n) AddBlock(replay->ids, replay->block_pool.Create(replay->inst_pool));
    auto block = [&](uint32_t id) { return replay->ids.block_ids[id]; };
    auto inst = [&](uint32_t id) { return replay->ids.inst_ids[id]; };
    for (uint32_t n = 0; n < ni; ++n) {
        auto& dst = *inst(n); const auto& src = insts[n];
        A::Uses(dst) = static_cast<int>(src.uses); A::Definition(dst) = src.definition;
        for (size_t j = 0; j < src.args.size(); ++j) {
            auto v = RestoreValue(src.args[j], *replay);
            if (src.opcode == IR::Opcode::Phi) A::Phi(dst).emplace_back(block(src.phi_blocks[j]), v);
            else A::Args(dst)[j] = v;
        }
        if (src.associated) {
            auto& assoc = A::Assoc(dst); assoc = std::make_unique<IR::AssociatedInsts>();
            for (size_t j : {size_t{0}, size_t{4}, size_t{5}}) if (src.pseudo[j] != Null) assoc->zero_inst = inst(src.pseudo[j]);
            if (src.pseudo[1] != Null) assoc->sign_inst = inst(src.pseudo[1]);
            if (src.pseudo[2] != Null) assoc->carry_inst = inst(src.pseudo[2]);
            if (src.pseudo[3] != Null) assoc->overflow_inst = inst(src.pseudo[3]);
        }
    }
    for (uint32_t n = 0; n < nb; ++n) {
        auto& dst = *block(n); const auto& src = blocks[n];
        A::Order(dst) = src.order; A::Sealed(dst) = src.sealed; A::Definition(dst) = src.definition;
        for (auto id : src.members) dst.Instructions().push_back(*inst(id));
        for (auto id : src.preds) A::Preds(dst).push_back(block(id));
        for (auto id : src.succs) A::Succs(dst).push_back(block(id));
        for (const auto& [reg,v] : src.cache) A::Cache(dst)[reg] = RestoreValue(v, *replay);
    }
    for (auto id : program_blocks) replay->program.blocks.push_back(block(id));
    for (auto id : post_order) replay->program.post_order_blocks.push_back(block(id));
    return replay;
}
} // namespace Shader::SsaFixture
