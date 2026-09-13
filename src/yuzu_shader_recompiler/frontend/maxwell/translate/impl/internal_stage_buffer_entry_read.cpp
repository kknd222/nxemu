// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "yuzu_common/bit_field.h"
#include "yuzu_common/common_types.h"
#include "yuzu_shader_recompiler/frontend/maxwell/translate/impl/impl.h"

namespace Shader::Maxwell {
namespace {
enum class Mode : u64 {
    Default,
    Patch,
    Prim,
    Attr,
};

enum class SZ : u64 {
    U8,
    U16,
    U32,
    F32,
};

enum class Shift : u64 {
    Default,
    U16,
    B32,
};

IR::U32 ScaleIndex(IR::IREmitter& ir, IR::U32 index, Shift shift) {
    switch (shift) {
    case Shift::Default:
        return index;
    case Shift::U16:
        return ir.ShiftLeftLogical(index, ir.Imm32(1));
    case Shift::B32:
        return ir.ShiftLeftLogical(index, ir.Imm32(2));
    default:
        UNREACHABLE();
    }
}

IR::U32 SkewBytes(IR::IREmitter& ir, SZ size_read) {
    const IR::U32 lane{ir.LaneId()};
    switch (size_read) {
    case SZ::U8:
        return lane;
    case SZ::U16:
        return ir.ShiftLeftLogical(lane, ir.Imm32(1));
    case SZ::U32:
    case SZ::F32:
        return ir.ShiftLeftLogical(lane, ir.Imm32(2));
    default:
        UNREACHABLE();
    }
}

IR::U32 LoadGlobalSmall(IR::IREmitter& ir, IR::U32 byte_address, u32 bit_count) {
    const IR::U32 aligned_address{ir.BitwiseAnd(byte_address, ir.Imm32(~u32{3}))};
    const IR::U32 word{ir.LoadGlobal32(ir.UConvert(64, aligned_address))};
    const IR::U32 byte_offset{ir.BitwiseAnd(byte_address, ir.Imm32(u32{3}))};
    const IR::U32 bit_offset{ir.ShiftLeftLogical(byte_offset, ir.Imm32(3))};
    return ir.BitFieldExtract(word, bit_offset, ir.Imm32(bit_count), false);
}

} // Anonymous namespace

void TranslatorVisitor::ISBERD(u64 insn) {
    LOG_DEBUG(Shader, "called with insn={:#x}", insn);

    union {
        u64 raw;
        BitField<0, 8, IR::Reg> dest_reg;
        BitField<8, 8, IR::Reg> src_reg;
        BitField<8, 8, u32> src_reg_num;
        BitField<24, 8, u32> imm;
        BitField<31, 1, u64> skew;
        BitField<32, 1, u64> o;
        BitField<33, 2, Mode> mode;
        BitField<36, 4, SZ> sz;
        BitField<47, 2, Shift> shift;
    } const isberd{insn};

    IR::U32 index{};
    if (isberd.src_reg_num.Value() == 0xff) {
        index = ir.Imm32(isberd.imm.Value());
    } else {
        const IR::U32 scaled_index{ScaleIndex(ir, X(isberd.src_reg.Value()), isberd.shift.Value())};
        index = ir.IAdd(scaled_index, ir.Imm32(isberd.imm.Value()));
    }

    if (isberd.o.Value()) {
        if (isberd.skew.Value()) {
            index = ir.IAdd(index, SkewBytes(ir, isberd.sz.Value()));
        }

        IR::U32 value{};
        switch (isberd.sz.Value()) {
        case SZ::U8:
            value = LoadGlobalSmall(ir, index, 8);
            break;
        case SZ::U16:
            value = LoadGlobalSmall(ir, index, 16);
            break;
        case SZ::U32:
        case SZ::F32:
            value = ir.LoadGlobal32(ir.UConvert(64, index));
            break;
        default:
            UNREACHABLE();
        }
        X(isberd.dest_reg.Value(), value);
        return;
    }

    if (isberd.mode.Value() != Mode::Default) {
        if (isberd.skew.Value()) {
            index = ir.IAdd(index, SkewBytes(ir, SZ::U32));
        }

        IR::F32 value{};
        switch (isberd.mode.Value()) {
        case Mode::Patch:
            value = ir.GetPatch(index.Patch());
            break;
        case Mode::Prim:
            value = ir.GetAttribute(index.Attribute());
            break;
        case Mode::Attr:
            value = ir.GetAttributeIndexed(index);
            break;
        default:
            UNREACHABLE();
        }
        X(isberd.dest_reg.Value(), ir.BitCast<IR::U32>(value));
        return;
    }

    if (isberd.skew.Value()) {
        X(isberd.dest_reg.Value(), ir.IAdd(X(isberd.src_reg.Value()), ir.LaneId()));
        return;
    }

    X(isberd.dest_reg.Value(), X(isberd.src_reg.Value()));
}

} // namespace Shader::Maxwell
