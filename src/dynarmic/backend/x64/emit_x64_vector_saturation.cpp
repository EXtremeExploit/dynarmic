/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include "dynarmic/backend/x64/block_of_code.h"
#include "dynarmic/backend/x64/constants.h"
#include "dynarmic/backend/x64/emit_x64.h"
#include "dynarmic/common/common_types.h"
#include "dynarmic/ir/microinstruction.h"
#include "dynarmic/ir/opcodes.h"

#define FCODE(NAME)                  \
    [&code](auto... args) {          \
        if constexpr (esize == 32) { \
            code.NAME##s(args...);   \
        } else {                     \
            code.NAME##d(args...);   \
        }                            \
    }

#define ICODE(NAME)                  \
    [&code](auto... args) {          \
        if constexpr (esize == 32) { \
            code.NAME##d(args...);   \
        } else {                     \
            code.NAME##q(args...);   \
        }                            \
    }

namespace Dynarmic::Backend::X64 {

using namespace Xbyak::util;

namespace {

void EmitVectorSaturatedNative(BlockOfCode& code, EmitContext& ctx, IR::Inst* inst, void (Xbyak::CodeGenerator::*saturated_fn)(const Xbyak::Mmx& mmx, const Xbyak::Operand&), void (Xbyak::CodeGenerator::*unsaturated_fn)(const Xbyak::Mmx& mmx, const Xbyak::Operand&), void (Xbyak::CodeGenerator::*sub_fn)(const Xbyak::Mmx& mmx, const Xbyak::Operand&)) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);

    const Xbyak::Xmm result = ctx.reg_alloc.UseScratchXmm(args[0]);
    const Xbyak::Xmm addend = ctx.reg_alloc.UseXmm(args[1]);
    const Xbyak::Reg8 overflow = ctx.reg_alloc.ScratchGpr().cvt8();

    code.movaps(xmm0, result);

    (code.*saturated_fn)(result, addend);

    (code.*unsaturated_fn)(xmm0, addend);
    (code.*sub_fn)(xmm0, result);
    if (code.HasHostFeature(HostFeature::SSE41)) {
        code.ptest(xmm0, xmm0);
    } else {
        const Xbyak::Xmm tmp = ctx.reg_alloc.ScratchXmm();
        code.pxor(tmp, tmp);
        code.pcmpeqw(xmm0, tmp);
        code.pmovmskb(overflow.cvt32(), xmm0);
        code.xor_(overflow.cvt32(), 0xFFFF);
        code.test(overflow.cvt32(), overflow.cvt32());
    }
    code.setnz(overflow);
    code.or_(code.byte[code.r15 + code.GetJitStateInfo().offsetof_fpsr_qc], overflow);

    ctx.reg_alloc.DefineValue(inst, result);
}

enum class Op {
    Add,
    Sub,
};

template<Op op, size_t esize>
void EmitVectorSignedSaturated(BlockOfCode& code, EmitContext& ctx, IR::Inst* inst) {
    static_assert(esize == 32 || esize == 64);
    constexpr u64 msb_mask = esize == 32 ? 0x8000000080000000 : 0x8000000000000000;

    auto args = ctx.reg_alloc.GetArgumentInfo(inst);

    const Xbyak::Xmm result = ctx.reg_alloc.UseScratchXmm(args[0]);
    const Xbyak::Xmm arg = ctx.reg_alloc.UseXmm(args[1]);
    const Xbyak::Xmm tmp = ctx.reg_alloc.ScratchXmm();
    const Xbyak::Reg8 overflow = ctx.reg_alloc.ScratchGpr().cvt8();

    code.movaps(tmp, result);

    if (code.HasHostFeature(HostFeature::AVX512_Ortho | HostFeature::AVX512DQ)) {
        if constexpr (op == Op::Add) {
            ICODE(vpadd)(result, result, arg);
            code.vpternlogd(tmp, result, arg, 0b00100100);
        } else {
            ICODE(vpsub)(result, result, arg);
            code.vpternlogd(tmp, result, arg, 0b00011000);
        }

        if constexpr (esize == 32) {
            code.vpmovd2m(k1, tmp);
        } else {
            code.vpmovq2m(k1, tmp);
        }
        ICODE(vpsra)(result | k1, result, u8(esize - 1));
        ICODE(vpxor)(result | k1, result, code.MConst(xword_b, msb_mask, msb_mask));

        code.ktestb(k1, k1);
        code.setnz(overflow);
        code.or_(code.byte[code.r15 + code.GetJitStateInfo().offsetof_fpsr_qc], overflow);

        ctx.reg_alloc.DefineValue(inst, result);
        return;
    }

    code.movaps(xmm0, result);

    if constexpr (op == Op::Add) {
        ICODE(padd)(result, arg);
    } else {
        ICODE(psub)(result, arg);
    }

    code.pxor(tmp, result);
    code.pxor(xmm0, arg);
    if constexpr (op == Op::Add) {
        code.pandn(xmm0, tmp);
    } else {
        code.pand(xmm0, tmp);
    }

    code.movaps(tmp, result);
    code.psrad(tmp, 31);
    if constexpr (esize == 64) {
        code.pshufd(tmp, tmp, 0b11110101);
    }
    code.pxor(tmp, code.MConst(xword, msb_mask, msb_mask));

    if (code.HasHostFeature(HostFeature::SSE41)) {
        code.ptest(xmm0, code.MConst(xword, msb_mask, msb_mask));
    } else {
        FCODE(movmskp)(overflow.cvt32(), xmm0);
        code.test(overflow.cvt32(), overflow.cvt32());
    }
    code.setnz(overflow);
    code.or_(code.byte[code.r15 + code.GetJitStateInfo().offsetof_fpsr_qc], overflow);

    if (code.HasHostFeature(HostFeature::SSE41)) {
        FCODE(blendvp)(result, tmp);

        ctx.reg_alloc.DefineValue(inst, result);
    } else {
        code.psrad(xmm0, 31);
        if constexpr (esize == 64) {
            code.pshufd(xmm0, xmm0, 0b11110101);
        }

        code.pand(tmp, xmm0);
        code.pandn(xmm0, result);
        code.por(tmp, xmm0);

        ctx.reg_alloc.DefineValue(inst, tmp);
    }
}

}  // anonymous namespace

void EmitX64::EmitVectorSignedSaturatedAdd8(EmitContext& ctx, IR::Inst* inst) {
    EmitVectorSaturatedNative(code, ctx, inst, &Xbyak::CodeGenerator::paddsb, &Xbyak::CodeGenerator::paddb, &Xbyak::CodeGenerator::psubb);
}

void EmitX64::EmitVectorSignedSaturatedAdd16(EmitContext& ctx, IR::Inst* inst) {
    EmitVectorSaturatedNative(code, ctx, inst, &Xbyak::CodeGenerator::paddsw, &Xbyak::CodeGenerator::paddw, &Xbyak::CodeGenerator::psubw);
}

void EmitX64::EmitVectorSignedSaturatedAdd32(EmitContext& ctx, IR::Inst* inst) {
    EmitVectorSignedSaturated<Op::Add, 32>(code, ctx, inst);
}

void EmitX64::EmitVectorSignedSaturatedAdd64(EmitContext& ctx, IR::Inst* inst) {
    EmitVectorSignedSaturated<Op::Add, 64>(code, ctx, inst);
}

void EmitX64::EmitVectorSignedSaturatedSub8(EmitContext& ctx, IR::Inst* inst) {
    EmitVectorSaturatedNative(code, ctx, inst, &Xbyak::CodeGenerator::psubsb, &Xbyak::CodeGenerator::psubb, &Xbyak::CodeGenerator::psubb);
}

void EmitX64::EmitVectorSignedSaturatedSub16(EmitContext& ctx, IR::Inst* inst) {
    EmitVectorSaturatedNative(code, ctx, inst, &Xbyak::CodeGenerator::psubsw, &Xbyak::CodeGenerator::psubw, &Xbyak::CodeGenerator::psubw);
}

void EmitX64::EmitVectorSignedSaturatedSub32(EmitContext& ctx, IR::Inst* inst) {
    EmitVectorSignedSaturated<Op::Sub, 32>(code, ctx, inst);
}

void EmitX64::EmitVectorSignedSaturatedSub64(EmitContext& ctx, IR::Inst* inst) {
    EmitVectorSignedSaturated<Op::Sub, 64>(code, ctx, inst);
}

void EmitX64::EmitVectorUnsignedSaturatedAdd8(EmitContext& ctx, IR::Inst* inst) {
    EmitVectorSaturatedNative(code, ctx, inst, &Xbyak::CodeGenerator::paddusb, &Xbyak::CodeGenerator::paddb, &Xbyak::CodeGenerator::psubb);
}

void EmitX64::EmitVectorUnsignedSaturatedAdd16(EmitContext& ctx, IR::Inst* inst) {
    EmitVectorSaturatedNative(code, ctx, inst, &Xbyak::CodeGenerator::paddusw, &Xbyak::CodeGenerator::paddw, &Xbyak::CodeGenerator::psubw);
}

void EmitX64::EmitVectorUnsignedSaturatedAdd32(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);

    if (code.HasHostFeature(HostFeature::AVX512_Ortho | HostFeature::AVX512DQ)) {
        const Xbyak::Xmm operand1 = ctx.reg_alloc.UseXmm(args[0]);
        const Xbyak::Xmm operand2 = ctx.reg_alloc.UseXmm(args[1]);
        const Xbyak::Xmm result = ctx.reg_alloc.ScratchXmm();
        const Xbyak::Reg8 overflow = ctx.reg_alloc.ScratchGpr().cvt8();

        code.vpaddd(result, operand1, operand2);
        code.vpcmpud(k1, result, operand2, CmpInt::LessThan);
        code.vpternlogd(result | k1, result, result, 0xFF);
        code.ktestb(k1, k1);

        code.setnz(overflow);
        code.or_(code.byte[code.r15 + code.GetJitStateInfo().offsetof_fpsr_qc], overflow);

        ctx.reg_alloc.DefineValue(inst, result);
        return;
    }

    const Xbyak::Xmm operand1 = code.HasHostFeature(HostFeature::AVX) ? ctx.reg_alloc.UseXmm(args[0]) : ctx.reg_alloc.UseScratchXmm(args[0]);
    const Xbyak::Xmm operand2 = ctx.reg_alloc.UseXmm(args[1]);
    const Xbyak::Xmm result = code.HasHostFeature(HostFeature::AVX) ? ctx.reg_alloc.ScratchXmm() : operand1;
    const Xbyak::Reg8 overflow = ctx.reg_alloc.ScratchGpr().cvt8();
    const Xbyak::Xmm tmp = ctx.reg_alloc.ScratchXmm();

    if (code.HasHostFeature(HostFeature::AVX)) {
        code.vpxor(xmm0, operand1, operand2);
        code.vpand(tmp, operand1, operand2);
        code.vpaddd(result, operand1, operand2);
    } else {
        code.movaps(tmp, operand1);
        code.movaps(xmm0, operand1);

        code.pxor(xmm0, operand2);
        code.pand(tmp, operand2);
        code.paddd(result, operand2);
    }

    code.psrld(xmm0, 1);
    code.paddd(tmp, xmm0);
    code.psrad(tmp, 31);

    code.por(result, tmp);

    if (code.HasHostFeature(HostFeature::SSE41)) {
        code.ptest(tmp, tmp);
    } else {
        code.movmskps(overflow.cvt32(), tmp);
        code.test(overflow.cvt32(), overflow.cvt32());
    }

    code.setnz(overflow);
    code.or_(code.byte[code.r15 + code.GetJitStateInfo().offsetof_fpsr_qc], overflow);

    ctx.reg_alloc.DefineValue(inst, result);
}

void EmitX64::EmitVectorUnsignedSaturatedAdd64(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);

    if (code.HasHostFeature(HostFeature::AVX512_Ortho | HostFeature::AVX512DQ)) {
        const Xbyak::Xmm operand1 = ctx.reg_alloc.UseXmm(args[0]);
        const Xbyak::Xmm operand2 = ctx.reg_alloc.UseXmm(args[1]);
        const Xbyak::Xmm result = ctx.reg_alloc.ScratchXmm();
        const Xbyak::Reg8 overflow = ctx.reg_alloc.ScratchGpr().cvt8();

        code.vpaddq(result, operand1, operand2);
        code.vpcmpuq(k1, result, operand1, CmpInt::LessThan);
        code.vpternlogq(result | k1, result, result, 0xFF);
        code.ktestb(k1, k1);

        code.setnz(overflow);
        code.or_(code.byte[code.r15 + code.GetJitStateInfo().offsetof_fpsr_qc], overflow);

        ctx.reg_alloc.DefineValue(inst, result);

        return;
    }

    const Xbyak::Xmm operand1 = code.HasHostFeature(HostFeature::AVX) ? ctx.reg_alloc.UseXmm(args[0]) : ctx.reg_alloc.UseScratchXmm(args[0]);
    const Xbyak::Xmm operand2 = ctx.reg_alloc.UseXmm(args[1]);
    const Xbyak::Xmm result = code.HasHostFeature(HostFeature::AVX) ? ctx.reg_alloc.ScratchXmm() : operand1;
    const Xbyak::Reg8 overflow = ctx.reg_alloc.ScratchGpr().cvt8();
    const Xbyak::Xmm tmp = ctx.reg_alloc.ScratchXmm();

    if (code.HasHostFeature(HostFeature::AVX)) {
        code.vpxor(xmm0, operand1, operand2);
        code.vpand(tmp, operand1, operand2);
        code.vpaddq(result, operand1, operand2);
    } else {
        code.movaps(xmm0, operand1);
        code.movaps(tmp, operand1);

        code.pxor(xmm0, operand2);
        code.pand(tmp, operand2);
        code.paddq(result, operand2);
    }

    code.psrlq(xmm0, 1);
    code.paddq(tmp, xmm0);
    code.psrad(tmp, 31);
    code.pshufd(tmp, tmp, 0b11110101);

    code.por(result, tmp);

    if (code.HasHostFeature(HostFeature::SSE41)) {
        code.ptest(tmp, tmp);
    } else {
        code.movmskpd(overflow.cvt32(), tmp);
        code.test(overflow.cvt32(), overflow.cvt32());
    }

    code.setnz(overflow);
    code.or_(code.byte[code.r15 + code.GetJitStateInfo().offsetof_fpsr_qc], overflow);

    ctx.reg_alloc.DefineValue(inst, result);
}

void EmitX64::EmitVectorUnsignedSaturatedSub8(EmitContext& ctx, IR::Inst* inst) {
    EmitVectorSaturatedNative(code, ctx, inst, &Xbyak::CodeGenerator::psubusb, &Xbyak::CodeGenerator::psubb, &Xbyak::CodeGenerator::psubb);
}

void EmitX64::EmitVectorUnsignedSaturatedSub16(EmitContext& ctx, IR::Inst* inst) {
    EmitVectorSaturatedNative(code, ctx, inst, &Xbyak::CodeGenerator::psubusw, &Xbyak::CodeGenerator::psubw, &Xbyak::CodeGenerator::psubw);
}

void EmitX64::EmitVectorUnsignedSaturatedSub32(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);

    if (code.HasHostFeature(HostFeature::AVX512_Ortho | HostFeature::AVX512DQ)) {
        const Xbyak::Xmm operand1 = ctx.reg_alloc.UseXmm(args[0]);
        const Xbyak::Xmm operand2 = ctx.reg_alloc.UseXmm(args[1]);
        const Xbyak::Xmm result = ctx.reg_alloc.ScratchXmm();
        const Xbyak::Reg8 overflow = ctx.reg_alloc.ScratchGpr().cvt8();

        code.vpsubd(result, operand1, operand2);
        code.vpcmpud(k1, result, operand1, CmpInt::GreaterThan);
        code.vpxord(result | k1, result, result);
        code.ktestb(k1, k1);

        code.setnz(overflow);
        code.or_(code.byte[code.r15 + code.GetJitStateInfo().offsetof_fpsr_qc], overflow);

        ctx.reg_alloc.DefineValue(inst, result);
        return;
    }

    const Xbyak::Xmm operand1 = code.HasHostFeature(HostFeature::AVX) ? ctx.reg_alloc.UseXmm(args[0]) : ctx.reg_alloc.UseScratchXmm(args[0]);
    const Xbyak::Xmm operand2 = ctx.reg_alloc.UseXmm(args[1]);
    const Xbyak::Xmm result = code.HasHostFeature(HostFeature::AVX) ? ctx.reg_alloc.ScratchXmm() : operand1;
    const Xbyak::Xmm tmp = ctx.reg_alloc.ScratchXmm();
    const Xbyak::Reg8 overflow = ctx.reg_alloc.ScratchGpr().cvt8();

    if (code.HasHostFeature(HostFeature::AVX)) {
        code.vpxor(tmp, operand1, operand2);
        code.vpsubd(result, operand1, operand2);
        code.vpand(xmm0, operand2, tmp);
    } else {
        code.movaps(tmp, operand1);
        code.movaps(xmm0, operand2);

        code.pxor(tmp, operand2);
        code.psubd(result, operand2);
        code.pand(xmm0, tmp);
    }

    code.psrld(tmp, 1);
    code.psubd(tmp, xmm0);
    code.psrad(tmp, 31);

    if (code.HasHostFeature(HostFeature::SSE41)) {
        code.ptest(tmp, tmp);
    } else {
        code.movmskps(overflow.cvt32(), tmp);
        code.test(overflow.cvt32(), overflow.cvt32());
    }
    code.setnz(overflow);
    code.or_(code.byte[code.r15 + code.GetJitStateInfo().offsetof_fpsr_qc], overflow);

    code.pandn(tmp, result);
    ctx.reg_alloc.DefineValue(inst, tmp);
}

void EmitX64::EmitVectorUnsignedSaturatedSub64(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);

    if (code.HasHostFeature(HostFeature::AVX512_Ortho | HostFeature::AVX512DQ)) {
        const Xbyak::Xmm operand1 = ctx.reg_alloc.UseXmm(args[0]);
        const Xbyak::Xmm operand2 = ctx.reg_alloc.UseXmm(args[1]);
        const Xbyak::Xmm result = ctx.reg_alloc.ScratchXmm();
        const Xbyak::Reg8 overflow = ctx.reg_alloc.ScratchGpr().cvt8();

        code.vpsubq(result, operand1, operand2);
        code.vpcmpuq(k1, result, operand1, CmpInt::GreaterThan);
        code.vpxorq(result | k1, result, result);
        code.ktestb(k1, k1);

        code.setnz(overflow);
        code.or_(code.byte[code.r15 + code.GetJitStateInfo().offsetof_fpsr_qc], overflow);

        ctx.reg_alloc.DefineValue(inst, result);
        return;
    }

    const Xbyak::Xmm operand1 = code.HasHostFeature(HostFeature::AVX) ? ctx.reg_alloc.UseXmm(args[0]) : ctx.reg_alloc.UseScratchXmm(args[0]);
    const Xbyak::Xmm operand2 = ctx.reg_alloc.UseXmm(args[1]);
    const Xbyak::Xmm result = code.HasHostFeature(HostFeature::AVX) ? ctx.reg_alloc.ScratchXmm() : operand1;
    const Xbyak::Xmm tmp = ctx.reg_alloc.ScratchXmm();
    const Xbyak::Reg8 overflow = ctx.reg_alloc.ScratchGpr().cvt8();

    if (code.HasHostFeature(HostFeature::AVX)) {
        code.vpxor(tmp, operand1, operand2);
        code.vpsubq(result, operand1, operand2);
        code.vpand(xmm0, operand2, tmp);
    } else {
        code.movaps(tmp, operand1);
        code.movaps(xmm0, operand2);

        code.pxor(tmp, operand2);
        code.psubq(result, operand2);
        code.pand(xmm0, tmp);
    }

    code.psrlq(tmp, 1);
    code.psubq(tmp, xmm0);
    code.psrad(tmp, 31);
    code.pshufd(tmp, tmp, 0b11110101);

    if (code.HasHostFeature(HostFeature::SSE41)) {
        code.ptest(tmp, tmp);
    } else {
        code.movmskpd(overflow.cvt32(), tmp);
        code.test(overflow.cvt32(), overflow.cvt32());
    }
    code.setnz(overflow);
    code.or_(code.byte[code.r15 + code.GetJitStateInfo().offsetof_fpsr_qc], overflow);

    code.pandn(tmp, result);
    ctx.reg_alloc.DefineValue(inst, tmp);
}

}  // namespace Dynarmic::Backend::X64
