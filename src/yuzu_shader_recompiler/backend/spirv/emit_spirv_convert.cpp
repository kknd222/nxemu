// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "yuzu_shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "yuzu_shader_recompiler/backend/spirv/spirv_emit_context.h"
#include "yuzu_shader_recompiler/frontend/ir/modifiers.h"

namespace Shader::Backend::SPIRV {
namespace {
Id ExtractU16(EmitContext& ctx, Id value) {
    if (ctx.profile.support_int16) {
        return ctx.OpUConvert(ctx.U16, value);
    } else {
        return ctx.OpBitFieldUExtract(ctx.U32[1], value, ctx.u32_zero_value, ctx.Const(16u));
    }
}

Id ExtractS16(EmitContext& ctx, Id value) {
    if (ctx.profile.support_int16) {
        return ctx.OpSConvert(ctx.S16, value);
    } else {
        return ctx.OpBitFieldSExtract(ctx.U32[1], value, ctx.u32_zero_value, ctx.Const(16u));
    }
}

Id ExtractU8(EmitContext& ctx, Id value) {
    if (ctx.profile.support_int8) {
        return ctx.OpUConvert(ctx.U8, value);
    } else {
        return ctx.OpBitFieldUExtract(ctx.U32[1], value, ctx.u32_zero_value, ctx.Const(8u));
    }
}

Id ExtractS8(EmitContext& ctx, Id value) {
    if (ctx.profile.support_int8) {
        return ctx.OpSConvert(ctx.S8, value);
    } else {
        return ctx.OpBitFieldSExtract(ctx.U32[1], value, ctx.u32_zero_value, ctx.Const(8u));
    }
}
} // Anonymous namespace

Id EmitConvertS16F16(EmitContext& ctx, Id value) {
    if (ctx.profile.support_int16) {
        return ctx.OpSConvert(ctx.U32[1], ctx.OpConvertFToS(ctx.U16, value));
    } else {
        return ExtractS16(ctx, ctx.OpConvertFToS(ctx.U32[1], value));
    }
}

Id EmitConvertS16F32(EmitContext& ctx, Id value) {
    if (ctx.profile.support_int16) {
        return ctx.OpSConvert(ctx.U32[1], ctx.OpConvertFToS(ctx.U16, value));
    } else {
        return ExtractS16(ctx, ctx.OpConvertFToS(ctx.U32[1], value));
    }
}

Id EmitConvertS16F64(EmitContext& ctx, Id value) {
    if (ctx.profile.support_int16) {
        return ctx.OpSConvert(ctx.U32[1], ctx.OpConvertFToS(ctx.U16, value));
    } else {
        return ExtractS16(ctx, ctx.OpConvertFToS(ctx.U32[1], value));
    }
}

Id EmitConvertS32F16(EmitContext& ctx, Id value) {
    return ctx.OpConvertFToS(ctx.U32[1], value);
}

Id EmitConvertS32F32(EmitContext& ctx, Id value) {
    if (ctx.profile.has_broken_signed_operations) {
        return ctx.OpBitcast(ctx.U32[1], ctx.OpConvertFToS(ctx.S32[1], value));
    } else {
        return ctx.OpConvertFToS(ctx.U32[1], value);
    }
}

Id EmitConvertS32F64(EmitContext& ctx, Id value) {
    return ctx.OpConvertFToS(ctx.U32[1], value);
}

Id EmitConvertS64F16(EmitContext& ctx, Id value) {
    return ctx.OpConvertFToS(ctx.U64, value);
}

Id EmitConvertS64F32(EmitContext& ctx, Id value) {
    return ctx.OpConvertFToS(ctx.U64, value);
}

Id EmitConvertS64F64(EmitContext& ctx, Id value) {
    return ctx.OpConvertFToS(ctx.U64, value);
}

Id EmitConvertU16F16(EmitContext& ctx, Id value) {
    if (ctx.profile.support_int16) {
        return ctx.OpUConvert(ctx.U32[1], ctx.OpConvertFToU(ctx.U16, value));
    } else {
        return ExtractU16(ctx, ctx.OpConvertFToU(ctx.U32[1], value));
    }
}

Id EmitConvertU16F32(EmitContext& ctx, Id value) {
    if (ctx.profile.support_int16) {
        return ctx.OpUConvert(ctx.U32[1], ctx.OpConvertFToU(ctx.U16, value));
    } else {
        return ExtractU16(ctx, ctx.OpConvertFToU(ctx.U32[1], value));
    }
}

Id EmitConvertU16F64(EmitContext& ctx, Id value) {
    if (ctx.profile.support_int16) {
        return ctx.OpUConvert(ctx.U32[1], ctx.OpConvertFToU(ctx.U16, value));
    } else {
        return ExtractU16(ctx, ctx.OpConvertFToU(ctx.U32[1], value));
    }
}

Id EmitConvertU32F16(EmitContext& ctx, Id value) {
    return ctx.OpConvertFToU(ctx.U32[1], value);
}

Id EmitConvertU32F32(EmitContext& ctx, Id value) {
    return ctx.OpConvertFToU(ctx.U32[1], value);
}

Id EmitConvertU32F64(EmitContext& ctx, Id value) {
    return ctx.OpConvertFToU(ctx.U32[1], value);
}

Id EmitConvertU64F16(EmitContext& ctx, Id value) {
    return ctx.OpConvertFToU(ctx.U64, value);
}

Id EmitConvertU64F32(EmitContext& ctx, Id value) {
    return ctx.OpConvertFToU(ctx.U64, value);
}

Id EmitConvertU64F64(EmitContext& ctx, Id value) {
    return ctx.OpConvertFToU(ctx.U64, value);
}

Id EmitConvertU64U32(EmitContext& ctx, Id value) {
    return ctx.OpUConvert(ctx.U64, value);
}

Id EmitConvertU32U64(EmitContext& ctx, Id value) {
    return ctx.OpUConvert(ctx.U32[1], value);
}

Id EmitConvertF16F32(EmitContext& ctx, IR::Inst* inst, Id value) {
    const auto flags = inst->Flags<IR::FpControl>();
    if (flags.rounding != IR::FpRounding::RN || flags.fmz_mode != IR::FmzMode::None) {
        return ctx.OpFConvert(ctx.F16[1], value);
    }
    // F32 -> F16 RNE is not OpFConvert's unspecified host rounding. Construct
    // the IEEE bits with U32 operations so RN is local to this conversion and
    // does not change other instructions' rounding or require shaderInt16.
    const auto c = [&](u32 n) { return ctx.Const(n); };
    const auto select = [&](Id condition, Id yes, Id no) {
        return ctx.OpSelect(ctx.U32[1], condition, yes, no);
    };
    const auto bits = ctx.OpBitcast(ctx.U32[1], value);
    const auto sign = ctx.OpBitwiseAnd(ctx.U32[1], ctx.OpShiftRightLogical(ctx.U32[1], bits, c(16)), c(0x8000));
    const auto exponent = ctx.OpBitwiseAnd(ctx.U32[1], ctx.OpShiftRightLogical(ctx.U32[1], bits, c(23)), c(255));
    const auto fraction = ctx.OpBitwiseAnd(ctx.U32[1], bits, c(0x7fffff));
    const auto normal_base = ctx.OpBitwiseOr(ctx.U32[1],
        ctx.OpShiftLeftLogical(ctx.U32[1], ctx.OpISub(ctx.U32[1], exponent, c(112)), c(10)),
        ctx.OpShiftRightLogical(ctx.U32[1], fraction, c(13)));
    const auto round = [&](Id base, Id remainder, Id midpoint) {
        const auto odd = ctx.OpINotEqual(ctx.U1, ctx.OpBitwiseAnd(ctx.U32[1], base, c(1)), c(0));
        const auto tie_odd = ctx.OpLogicalAnd(ctx.U1, ctx.OpIEqual(ctx.U1, remainder, midpoint), odd);
        const auto increment = ctx.OpLogicalOr(ctx.U1, ctx.OpUGreaterThan(ctx.U1, remainder, midpoint), tie_odd);
        return ctx.OpIAdd(ctx.U32[1], base, select(increment, c(1), c(0)));
    };
    const auto normal = round(normal_base, ctx.OpBitwiseAnd(ctx.U32[1], fraction, c(0x1fff)), c(0x1000));
    // Both OpSelect branches execute: clamp before all dynamic shifts. The
    // subnormal significand uses shift 14..24, including the 2^-25 tie to zero.
    const auto high_clamped = select(ctx.OpUGreaterThan(ctx.U1, exponent, c(112)), c(112), exponent);
    const auto safe_exponent = select(ctx.OpULessThan(ctx.U1, high_clamped, c(102)), c(102), high_clamped);
    const auto shift = ctx.OpISub(ctx.U32[1], c(126), safe_exponent);
    const auto significand = ctx.OpBitwiseOr(ctx.U32[1], fraction, c(0x800000));
    const auto sub_base = ctx.OpShiftRightLogical(ctx.U32[1], significand, shift);
    const auto mask = ctx.OpISub(ctx.U32[1], ctx.OpShiftLeftLogical(ctx.U32[1], c(1), shift), c(1));
    const auto midpoint = ctx.OpShiftLeftLogical(ctx.U32[1], c(1), ctx.OpISub(ctx.U32[1], shift, c(1)));
    const auto rounded_sub = round(sub_base, ctx.OpBitwiseAnd(ctx.U32[1], significand, mask), midpoint);
    const auto subnormal = select(ctx.OpULessThan(ctx.U1, exponent, c(102)), c(0), rounded_sub);
    auto magnitude = select(ctx.OpULessThanEqual(ctx.U1, exponent, c(112)), subnormal, normal);
    magnitude = select(ctx.OpUGreaterThanEqual(ctx.U1, exponent, c(143)), c(0x7c00), magnitude);
    // Preserve sign and a bounded payload, quiet all NaNs. Payload fidelity is
    // not a console contract; only NaN class/quiet and infinity are guaranteed.
    const auto nan = ctx.OpBitwiseOr(ctx.U32[1], c(0x7e00), ctx.OpShiftRightLogical(ctx.U32[1], fraction, c(13)));
    const auto special = select(ctx.OpIEqual(ctx.U1, fraction, c(0)), c(0x7c00), nan);
    magnitude = select(ctx.OpIEqual(ctx.U1, exponent, c(255)), special, magnitude);
    const auto half_bits = ctx.OpBitwiseOr(ctx.U32[1], sign, magnitude);
    return ctx.OpCompositeExtract(ctx.F16[1], ctx.OpBitcast(ctx.F16[2], half_bits), 0);
}

Id EmitConvertF32F16(EmitContext& ctx, Id value) {
    return ctx.OpFConvert(ctx.F32[1], value);
}

Id EmitConvertF32F64(EmitContext& ctx, Id value) {
    return ctx.OpFConvert(ctx.F32[1], value);
}

Id EmitConvertF64F32(EmitContext& ctx, Id value) {
    return ctx.OpFConvert(ctx.F64[1], value);
}

Id EmitConvertF16S8(EmitContext& ctx, Id value) {
    return ctx.OpConvertSToF(ctx.F16[1], ExtractS8(ctx, value));
}

Id EmitConvertF16S16(EmitContext& ctx, Id value) {
    return ctx.OpConvertSToF(ctx.F16[1], ExtractS16(ctx, value));
}

Id EmitConvertF16S32(EmitContext& ctx, Id value) {
    return ctx.OpConvertSToF(ctx.F16[1], value);
}

Id EmitConvertF16S64(EmitContext& ctx, Id value) {
    return ctx.OpConvertSToF(ctx.F16[1], value);
}

Id EmitConvertF16U8(EmitContext& ctx, Id value) {
    return ctx.OpConvertUToF(ctx.F16[1], ExtractU8(ctx, value));
}

Id EmitConvertF16U16(EmitContext& ctx, Id value) {
    return ctx.OpConvertUToF(ctx.F16[1], ExtractU16(ctx, value));
}

Id EmitConvertF16U32(EmitContext& ctx, Id value) {
    return ctx.OpConvertUToF(ctx.F16[1], value);
}

Id EmitConvertF16U64(EmitContext& ctx, Id value) {
    return ctx.OpConvertUToF(ctx.F16[1], value);
}

Id EmitConvertF32S8(EmitContext& ctx, Id value) {
    return ctx.OpConvertSToF(ctx.F32[1], ExtractS8(ctx, value));
}

Id EmitConvertF32S16(EmitContext& ctx, Id value) {
    return ctx.OpConvertSToF(ctx.F32[1], ExtractS16(ctx, value));
}

Id EmitConvertF32S32(EmitContext& ctx, Id value) {
    if (ctx.profile.has_broken_signed_operations) {
        value = ctx.OpBitcast(ctx.S32[1], value);
    }
    return ctx.OpConvertSToF(ctx.F32[1], value);
}

Id EmitConvertF32S64(EmitContext& ctx, Id value) {
    return ctx.OpConvertSToF(ctx.F32[1], value);
}

Id EmitConvertF32U8(EmitContext& ctx, Id value) {
    return ctx.OpConvertUToF(ctx.F32[1], ExtractU8(ctx, value));
}

Id EmitConvertF32U16(EmitContext& ctx, Id value) {
    return ctx.OpConvertUToF(ctx.F32[1], ExtractU16(ctx, value));
}

Id EmitConvertF32U32(EmitContext& ctx, Id value) {
    return ctx.OpConvertUToF(ctx.F32[1], value);
}

Id EmitConvertF32U64(EmitContext& ctx, Id value) {
    return ctx.OpConvertUToF(ctx.F32[1], value);
}

Id EmitConvertF64S8(EmitContext& ctx, Id value) {
    return ctx.OpConvertSToF(ctx.F64[1], ExtractS8(ctx, value));
}

Id EmitConvertF64S16(EmitContext& ctx, Id value) {
    return ctx.OpConvertSToF(ctx.F64[1], ExtractS16(ctx, value));
}

Id EmitConvertF64S32(EmitContext& ctx, Id value) {
    if (ctx.profile.has_broken_signed_operations) {
        value = ctx.OpBitcast(ctx.S32[1], value);
    }
    return ctx.OpConvertSToF(ctx.F64[1], value);
}

Id EmitConvertF64S64(EmitContext& ctx, Id value) {
    return ctx.OpConvertSToF(ctx.F64[1], value);
}

Id EmitConvertF64U8(EmitContext& ctx, Id value) {
    return ctx.OpConvertUToF(ctx.F64[1], ExtractU8(ctx, value));
}

Id EmitConvertF64U16(EmitContext& ctx, Id value) {
    return ctx.OpConvertUToF(ctx.F64[1], ExtractU16(ctx, value));
}

Id EmitConvertF64U32(EmitContext& ctx, Id value) {
    return ctx.OpConvertUToF(ctx.F64[1], value);
}

Id EmitConvertF64U64(EmitContext& ctx, Id value) {
    return ctx.OpConvertUToF(ctx.F64[1], value);
}

} // namespace Shader::Backend::SPIRV
