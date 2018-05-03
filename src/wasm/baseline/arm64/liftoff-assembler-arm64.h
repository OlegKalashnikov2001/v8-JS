// Copyright 2017 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_WASM_BASELINE_ARM64_LIFTOFF_ASSEMBLER_ARM64_H_
#define V8_WASM_BASELINE_ARM64_LIFTOFF_ASSEMBLER_ARM64_H_

#include "src/wasm/baseline/liftoff-assembler.h"

#define BAILOUT(reason) bailout("arm64 " reason)

namespace v8 {
namespace internal {
namespace wasm {

namespace liftoff {

// Liftoff Frames.
//
//  slot      Frame
//       +--------------------+---------------------------
//  n+4  | optional padding slot to keep the stack 16 byte aligned.
//  n+3  |   parameter n      |
//  ...  |       ...          |
//   4   |   parameter 1      | or parameter 2
//   3   |   parameter 0      | or parameter 1
//   2   |  (result address)  | or parameter 0
//  -----+--------------------+---------------------------
//   1   | return addr (lr)   |
//   0   | previous frame (fp)|
//  -----+--------------------+  <-- frame ptr (fp)
//  -1   | 0xa: WASM_COMPILED |
//  -2   |     instance       |
//  -----+--------------------+---------------------------
//  -3   |     slot 0         |   ^
//  -4   |     slot 1         |   |
//       |                    | Frame slots
//       |                    |   |
//       |                    |   v
//       | optional padding slot to keep the stack 16 byte aligned.
//  -----+--------------------+  <-- stack ptr (sp)
//

constexpr int32_t kInstanceOffset = 2 * kPointerSize;
constexpr int32_t kFirstStackSlotOffset = kInstanceOffset + kPointerSize;
constexpr int32_t kConstantStackSpace = 0;

inline MemOperand GetStackSlot(uint32_t index) {
  int32_t offset =
      kFirstStackSlotOffset + index * LiftoffAssembler::kStackSlotSize;
  return MemOperand(fp, -offset);
}

inline MemOperand GetInstanceOperand() {
  return MemOperand(fp, -kInstanceOffset);
}

inline CPURegister GetRegFromType(const LiftoffRegister& reg, ValueType type) {
  switch (type) {
    case kWasmI32:
      return reg.gp().W();
    case kWasmI64:
      return reg.gp().X();
    case kWasmF32:
      return reg.fp().S();
    case kWasmF64:
      return reg.fp().D();
    default:
      UNREACHABLE();
  }
}

inline CPURegList PadRegList(RegList list) {
  if ((base::bits::CountPopulation(list) & 1) != 0) list |= padreg.bit();
  return CPURegList(CPURegister::kRegister, kXRegSizeInBits, list);
}

inline CPURegList PadVRegList(RegList list) {
  if ((base::bits::CountPopulation(list) & 1) != 0) list |= fp_scratch.bit();
  return CPURegList(CPURegister::kVRegister, kDRegSizeInBits, list);
}

inline CPURegister AcquireByType(UseScratchRegisterScope* temps,
                                 ValueType type) {
  switch (type) {
    case kWasmI32:
      return temps->AcquireW();
    case kWasmI64:
      return temps->AcquireX();
    case kWasmF32:
      return temps->AcquireS();
    case kWasmF64:
      return temps->AcquireD();
    default:
      UNREACHABLE();
  }
}

inline MemOperand GetMemOp(LiftoffAssembler* assm,
                           UseScratchRegisterScope* temps, Register addr,
                           Register offset, uint32_t offset_imm) {
  // Wasm memory is limited to a size <2GB, so all offsets can be encoded as
  // immediate value (in 31 bits, interpreted as signed value).
  // If the offset is bigger, we always trap and this code is not reached.
  DCHECK(is_uint31(offset_imm));
  if (offset.IsValid()) {
    if (offset_imm == 0) return MemOperand(addr.X(), offset.W(), UXTW);
    Register tmp = temps->AcquireW();
    assm->Add(tmp, offset.W(), offset_imm);
    return MemOperand(addr.X(), tmp, UXTW);
  }
  return MemOperand(addr.X(), offset_imm);
}

}  // namespace liftoff

uint32_t LiftoffAssembler::PrepareStackFrame() {
  uint32_t offset = static_cast<uint32_t>(pc_offset());
  InstructionAccurateScope scope(this, 1);
  sub(sp, sp, 0);
  return offset;
}

void LiftoffAssembler::PatchPrepareStackFrame(uint32_t offset,
                                              uint32_t stack_slots) {
  static_assert(kStackSlotSize == kXRegSize,
                "kStackSlotSize must equal kXRegSize");
  uint32_t bytes = liftoff::kConstantStackSpace + kStackSlotSize * stack_slots;
  // The stack pointer is required to be quadword aligned.
  // Misalignment will cause a stack alignment fault.
  bytes = RoundUp(bytes, kQuadWordSizeInBytes);
  if (!IsImmAddSub(bytes)) {
    // Round the stack to a page to try to fit a add/sub immediate.
    bytes = RoundUp(bytes, 0x1000);
    if (!IsImmAddSub(bytes)) {
      // Stack greater than 4M! Because this is a quite improbable case, we
      // just fallback to Turbofan.
      BAILOUT("Stack too big");
      return;
    }
  }
#ifdef USE_SIMULATOR
  // When using the simulator, deal with Liftoff which allocates the stack
  // before checking it.
  // TODO(arm): Remove this when the stack check mechanism will be updated.
  if (bytes > KB / 2) {
    BAILOUT("Stack limited to 512 bytes to avoid a bug in StackCheck");
    return;
  }
#endif
  PatchingAssembler patching_assembler(IsolateData(isolate()), buffer_ + offset,
                                       1);
  patching_assembler.PatchSubSp(bytes);
}

void LiftoffAssembler::FinishCode() { CheckConstPool(true, false); }

void LiftoffAssembler::LoadConstant(LiftoffRegister reg, WasmValue value,
                                    RelocInfo::Mode rmode) {
  switch (value.type()) {
    case kWasmI32:
      Mov(reg.gp().W(), Immediate(value.to_i32(), rmode));
      break;
    case kWasmI64:
      Mov(reg.gp().X(), Immediate(value.to_i64(), rmode));
      break;
    case kWasmF32:
      Fmov(reg.fp().S(), value.to_f32_boxed().get_scalar());
      break;
    case kWasmF64:
      Fmov(reg.fp().D(), value.to_f64_boxed().get_scalar());
      break;
    default:
      UNREACHABLE();
  }
}

void LiftoffAssembler::LoadFromInstance(Register dst, uint32_t offset,
                                        int size) {
  DCHECK_LE(offset, kMaxInt);
  Ldr(dst, liftoff::GetInstanceOperand());
  DCHECK(size == 4 || size == 8);
  if (size == 4) {
    Ldr(dst.W(), MemOperand(dst, offset));
  } else {
    Ldr(dst, MemOperand(dst, offset));
  }
}

void LiftoffAssembler::SpillInstance(Register instance) {
  Str(instance, liftoff::GetInstanceOperand());
}

void LiftoffAssembler::FillInstanceInto(Register dst) {
  Ldr(dst, liftoff::GetInstanceOperand());
}

void LiftoffAssembler::Load(LiftoffRegister dst, Register src_addr,
                            Register offset_reg, uint32_t offset_imm,
                            LoadType type, LiftoffRegList pinned,
                            uint32_t* protected_load_pc, bool is_load_mem) {
  UseScratchRegisterScope temps(this);
  MemOperand src_op =
      liftoff::GetMemOp(this, &temps, src_addr, offset_reg, offset_imm);
  if (protected_load_pc) *protected_load_pc = pc_offset();
  switch (type.value()) {
    case LoadType::kI32Load8U:
    case LoadType::kI64Load8U:
      Ldrb(dst.gp().W(), src_op);
      break;
    case LoadType::kI32Load8S:
      Ldrsb(dst.gp().W(), src_op);
      break;
    case LoadType::kI64Load8S:
      Ldrsb(dst.gp().X(), src_op);
      break;
    case LoadType::kI32Load16U:
    case LoadType::kI64Load16U:
      Ldrh(dst.gp().W(), src_op);
      break;
    case LoadType::kI32Load16S:
      Ldrsh(dst.gp().W(), src_op);
      break;
    case LoadType::kI64Load16S:
      Ldrsh(dst.gp().X(), src_op);
      break;
    case LoadType::kI32Load:
    case LoadType::kI64Load32U:
      Ldr(dst.gp().W(), src_op);
      break;
    case LoadType::kI64Load32S:
      Ldrsw(dst.gp().X(), src_op);
      break;
    case LoadType::kI64Load:
      Ldr(dst.gp().X(), src_op);
      break;
    case LoadType::kF32Load:
      Ldr(dst.fp().S(), src_op);
      break;
    case LoadType::kF64Load:
      Ldr(dst.fp().D(), src_op);
      break;
    default:
      UNREACHABLE();
  }
}

void LiftoffAssembler::Store(Register dst_addr, Register offset_reg,
                             uint32_t offset_imm, LiftoffRegister src,
                             StoreType type, LiftoffRegList pinned,
                             uint32_t* protected_store_pc, bool is_store_mem) {
  UseScratchRegisterScope temps(this);
  MemOperand dst_op =
      liftoff::GetMemOp(this, &temps, dst_addr, offset_reg, offset_imm);
  if (protected_store_pc) *protected_store_pc = pc_offset();
  switch (type.value()) {
    case StoreType::kI32Store8:
    case StoreType::kI64Store8:
      Strb(src.gp().W(), dst_op);
      break;
    case StoreType::kI32Store16:
    case StoreType::kI64Store16:
      Strh(src.gp().W(), dst_op);
      break;
    case StoreType::kI32Store:
    case StoreType::kI64Store32:
      Str(src.gp().W(), dst_op);
      break;
    case StoreType::kI64Store:
      Str(src.gp().X(), dst_op);
      break;
    case StoreType::kF32Store:
      Str(src.fp().S(), dst_op);
      break;
    case StoreType::kF64Store:
      Str(src.fp().D(), dst_op);
      break;
    default:
      UNREACHABLE();
  }
}

void LiftoffAssembler::ChangeEndiannessLoad(LiftoffRegister dst, LoadType type,
                                            LiftoffRegList pinned) {
  // Nop.
}

void LiftoffAssembler::ChangeEndiannessStore(LiftoffRegister src,
                                             StoreType type,
                                             LiftoffRegList pinned) {
  // Nop.
}

void LiftoffAssembler::LoadCallerFrameSlot(LiftoffRegister dst,
                                           uint32_t caller_slot_idx,
                                           ValueType type) {
  int32_t offset = (caller_slot_idx + 1) * LiftoffAssembler::kStackSlotSize;
  Ldr(liftoff::GetRegFromType(dst, type), MemOperand(fp, offset));
}

void LiftoffAssembler::MoveStackValue(uint32_t dst_index, uint32_t src_index,
                                      ValueType type) {
  UseScratchRegisterScope temps(this);
  CPURegister scratch = liftoff::AcquireByType(&temps, type);
  Ldr(scratch, liftoff::GetStackSlot(src_index));
  Str(scratch, liftoff::GetStackSlot(dst_index));
}

void LiftoffAssembler::Move(Register dst, Register src, ValueType type) {
  if (type == kWasmI32) {
    Mov(dst.W(), src.W());
  } else {
    DCHECK_EQ(kWasmI64, type);
    Mov(dst.X(), src.X());
  }
}

void LiftoffAssembler::Move(DoubleRegister dst, DoubleRegister src,
                            ValueType type) {
  if (type == kWasmF32) {
    Fmov(dst.S(), src.S());
  } else {
    DCHECK_EQ(kWasmF64, type);
    Fmov(dst.D(), src.D());
  }
}

void LiftoffAssembler::Spill(uint32_t index, LiftoffRegister reg,
                             ValueType type) {
  RecordUsedSpillSlot(index);
  MemOperand dst = liftoff::GetStackSlot(index);
  Str(liftoff::GetRegFromType(reg, type), dst);
}

void LiftoffAssembler::Spill(uint32_t index, WasmValue value) {
  RecordUsedSpillSlot(index);
  MemOperand dst = liftoff::GetStackSlot(index);
  UseScratchRegisterScope temps(this);
  CPURegister src = CPURegister::no_reg();
  switch (value.type()) {
    case kWasmI32:
      src = temps.AcquireW();
      Mov(src.W(), value.to_i32());
      break;
    case kWasmI64:
      src = temps.AcquireX();
      Mov(src.X(), value.to_i64());
      break;
    default:
      // We do not track f32 and f64 constants, hence they are unreachable.
      UNREACHABLE();
  }
  Str(src, dst);
}

void LiftoffAssembler::Fill(LiftoffRegister reg, uint32_t index,
                            ValueType type) {
  MemOperand src = liftoff::GetStackSlot(index);
  Ldr(liftoff::GetRegFromType(reg, type), src);
}

void LiftoffAssembler::FillI64Half(Register, uint32_t half_index) {
  UNREACHABLE();
}

#define I32_BINOP(name, instruction)                             \
  void LiftoffAssembler::emit_##name(Register dst, Register lhs, \
                                     Register rhs) {             \
    instruction(dst.W(), lhs.W(), rhs.W());                      \
  }
#define I64_BINOP(name, instruction)                                           \
  void LiftoffAssembler::emit_##name(LiftoffRegister dst, LiftoffRegister lhs, \
                                     LiftoffRegister rhs) {                    \
    instruction(dst.gp().X(), lhs.gp().X(), rhs.gp().X());                     \
  }
#define UNIMPLEMENTED_GP_UNOP(name)                                \
  bool LiftoffAssembler::emit_##name(Register dst, Register src) { \
    BAILOUT("gp unop: " #name);                                    \
    return true;                                                   \
  }
#define FP32_BINOP(name, instruction)                                        \
  void LiftoffAssembler::emit_##name(DoubleRegister dst, DoubleRegister lhs, \
                                     DoubleRegister rhs) {                   \
    instruction(dst.S(), lhs.S(), rhs.S());                                  \
  }
#define FP32_UNOP(name, instruction)                                           \
  void LiftoffAssembler::emit_##name(DoubleRegister dst, DoubleRegister src) { \
    instruction(dst.S(), src.S());                                             \
  }
#define FP64_BINOP(name, instruction)                                        \
  void LiftoffAssembler::emit_##name(DoubleRegister dst, DoubleRegister lhs, \
                                     DoubleRegister rhs) {                   \
    instruction(dst.D(), lhs.D(), rhs.D());                                  \
  }
#define FP64_UNOP(name, instruction)                                           \
  void LiftoffAssembler::emit_##name(DoubleRegister dst, DoubleRegister src) { \
    instruction(dst.D(), src.D());                                             \
  }
#define I32_SHIFTOP(name, instruction)                                         \
  void LiftoffAssembler::emit_##name(Register dst, Register src,               \
                                     Register amount, LiftoffRegList pinned) { \
    instruction(dst.W(), src.W(), amount.W());                                 \
  }
#define I64_SHIFTOP(name, instruction)                                         \
  void LiftoffAssembler::emit_##name(LiftoffRegister dst, LiftoffRegister src, \
                                     Register amount, LiftoffRegList pinned) { \
    instruction(dst.gp().X(), src.gp().X(), amount.X());                       \
  }

I32_BINOP(i32_add, Add)
I32_BINOP(i32_sub, Sub)
I32_BINOP(i32_mul, Mul)
I32_BINOP(i32_and, And)
I32_BINOP(i32_or, Orr)
I32_BINOP(i32_xor, Eor)
I32_SHIFTOP(i32_shl, Lsl)
I32_SHIFTOP(i32_sar, Asr)
I32_SHIFTOP(i32_shr, Lsr)
I64_BINOP(i64_add, Add)
I64_BINOP(i64_sub, Sub)
I64_BINOP(i64_mul, Mul)
I64_BINOP(i64_and, And)
I64_BINOP(i64_or, Orr)
I64_BINOP(i64_xor, Eor)
I64_SHIFTOP(i64_shl, Lsl)
I64_SHIFTOP(i64_sar, Asr)
I64_SHIFTOP(i64_shr, Lsr)
UNIMPLEMENTED_GP_UNOP(i32_clz)
UNIMPLEMENTED_GP_UNOP(i32_ctz)
UNIMPLEMENTED_GP_UNOP(i32_popcnt)
FP32_BINOP(f32_add, Fadd)
FP32_BINOP(f32_sub, Fsub)
FP32_BINOP(f32_mul, Fmul)
FP32_BINOP(f32_div, Fdiv)
FP32_BINOP(f32_min, Fmin)
FP32_BINOP(f32_max, Fmax)
FP32_UNOP(f32_abs, Fabs)
FP32_UNOP(f32_neg, Fneg)
FP32_UNOP(f32_ceil, Frintp)
FP32_UNOP(f32_floor, Frintm)
FP32_UNOP(f32_trunc, Frintz)
FP32_UNOP(f32_nearest_int, Frintn)
FP32_UNOP(f32_sqrt, Fsqrt)
FP64_BINOP(f64_add, Fadd)
FP64_BINOP(f64_sub, Fsub)
FP64_BINOP(f64_mul, Fmul)
FP64_BINOP(f64_div, Fdiv)
FP64_BINOP(f64_min, Fmin)
FP64_BINOP(f64_max, Fmax)
FP64_UNOP(f64_abs, Fabs)
FP64_UNOP(f64_neg, Fneg)
FP64_UNOP(f64_ceil, Frintp)
FP64_UNOP(f64_floor, Frintm)
FP64_UNOP(f64_trunc, Frintz)
FP64_UNOP(f64_nearest_int, Frintn)
FP64_UNOP(f64_sqrt, Fsqrt)

#undef I32_BINOP
#undef I64_BINOP
#undef UNIMPLEMENTED_GP_UNOP
#undef FP32_BINOP
#undef FP32_UNOP
#undef FP64_BINOP
#undef FP64_UNOP
#undef I32_SHIFTOP
#undef I64_SHIFTOP

void LiftoffAssembler::emit_i32_divs(Register dst, Register lhs, Register rhs,
                                     Label* trap_div_by_zero,
                                     Label* trap_div_unrepresentable) {
  BAILOUT("i32_divs");
}

void LiftoffAssembler::emit_i32_divu(Register dst, Register lhs, Register rhs,
                                     Label* trap_div_by_zero) {
  BAILOUT("i32_divu");
}

void LiftoffAssembler::emit_i32_rems(Register dst, Register lhs, Register rhs,
                                     Label* trap_div_by_zero) {
  BAILOUT("i32_rems");
}

void LiftoffAssembler::emit_i32_remu(Register dst, Register lhs, Register rhs,
                                     Label* trap_div_by_zero) {
  BAILOUT("i32_remu");
}

bool LiftoffAssembler::emit_i64_divs(LiftoffRegister dst, LiftoffRegister lhs,
                                     LiftoffRegister rhs,
                                     Label* trap_div_by_zero,
                                     Label* trap_div_unrepresentable) {
  BAILOUT("i64_divs");
  return true;
}

bool LiftoffAssembler::emit_i64_divu(LiftoffRegister dst, LiftoffRegister lhs,
                                     LiftoffRegister rhs,
                                     Label* trap_div_by_zero) {
  BAILOUT("i64_divu");
  return true;
}

bool LiftoffAssembler::emit_i64_rems(LiftoffRegister dst, LiftoffRegister lhs,
                                     LiftoffRegister rhs,
                                     Label* trap_div_by_zero) {
  BAILOUT("i64_rems");
  return true;
}

bool LiftoffAssembler::emit_i64_remu(LiftoffRegister dst, LiftoffRegister lhs,
                                     LiftoffRegister rhs,
                                     Label* trap_div_by_zero) {
  BAILOUT("i64_remu");
  return true;
}

bool LiftoffAssembler::emit_type_conversion(WasmOpcode opcode,
                                            LiftoffRegister dst,
                                            LiftoffRegister src, Label* trap) {
  BAILOUT("emit_type_conversion");
  return true;
}

void LiftoffAssembler::emit_jump(Label* label) { B(label); }

void LiftoffAssembler::emit_jump(Register target) { BAILOUT("emit_jump"); }

void LiftoffAssembler::emit_cond_jump(Condition cond, Label* label,
                                      ValueType type, Register lhs,
                                      Register rhs) {
  BAILOUT("emit_cond_jump");
}

void LiftoffAssembler::emit_i32_eqz(Register dst, Register src) {
  Cmp(src.W(), wzr);
  Cset(dst.W(), eq);
}

void LiftoffAssembler::emit_i32_set_cond(Condition cond, Register dst,
                                         Register lhs, Register rhs) {
  Cmp(lhs.W(), rhs.W());
  Cset(dst.W(), cond);
}

void LiftoffAssembler::emit_i64_eqz(Register dst, LiftoffRegister src) {
  BAILOUT("emit_i64_eqz");
}

void LiftoffAssembler::emit_i64_set_cond(Condition cond, Register dst,
                                         LiftoffRegister lhs,
                                         LiftoffRegister rhs) {
  BAILOUT("emit_i64_set_cond");
}

void LiftoffAssembler::emit_f32_set_cond(Condition cond, Register dst,
                                         DoubleRegister lhs,
                                         DoubleRegister rhs) {
  BAILOUT("emit_f32_set_cond");
}

void LiftoffAssembler::emit_f64_set_cond(Condition cond, Register dst,
                                         DoubleRegister lhs,
                                         DoubleRegister rhs) {
  BAILOUT("emit_f64_set_cond");
}

void LiftoffAssembler::StackCheck(Label* ool_code) {
  ExternalReference stack_limit =
      ExternalReference::address_of_stack_limit(isolate());
  UseScratchRegisterScope temps(this);
  Register scratch = temps.AcquireX();
  Mov(scratch, Operand(stack_limit));
  Ldr(scratch, MemOperand(scratch));
  Cmp(sp, scratch);
  B(ool_code, ls);
}

void LiftoffAssembler::CallTrapCallbackForTesting() {
  BAILOUT("CallTrapCallbackForTesting");
}

void LiftoffAssembler::AssertUnreachable(AbortReason reason) {
  BAILOUT("AssertUnreachable");
}

void LiftoffAssembler::PushRegisters(LiftoffRegList regs) {
  PushCPURegList(liftoff::PadRegList(regs.GetGpList()));
  PushCPURegList(liftoff::PadVRegList(regs.GetFpList()));
}

void LiftoffAssembler::PopRegisters(LiftoffRegList regs) {
  PopCPURegList(liftoff::PadVRegList(regs.GetFpList()));
  PopCPURegList(liftoff::PadRegList(regs.GetGpList()));
}

void LiftoffAssembler::DropStackSlotsAndRet(uint32_t num_stack_slots) {
  DropSlots(num_stack_slots);
  Ret();
}

void LiftoffAssembler::CallC(wasm::FunctionSig* sig,
                             const LiftoffRegister* args,
                             const LiftoffRegister* rets,
                             ValueType out_argument_type, int stack_bytes,
                             ExternalReference ext_ref) {
  BAILOUT("CallC");
}

void LiftoffAssembler::CallNativeWasmCode(Address addr) {
  BAILOUT("CallNativeWasmCode");
}

void LiftoffAssembler::CallRuntime(Zone* zone, Runtime::FunctionId fid) {
  BAILOUT("CallRuntime");
}

void LiftoffAssembler::CallIndirect(wasm::FunctionSig* sig,
                                    compiler::CallDescriptor* call_descriptor,
                                    Register target) {
  BAILOUT("CallIndirect");
}

void LiftoffAssembler::AllocateStackSlot(Register addr, uint32_t size) {
  BAILOUT("AllocateStackSlot");
}

void LiftoffAssembler::DeallocateStackSlot(uint32_t size) {
  BAILOUT("DeallocateStackSlot");
}

void LiftoffStackSlots::Construct() {
  size_t slot_count = slots_.size();
  // The stack pointer is required to be quadword aligned.
  asm_->Claim(RoundUp(slot_count, 2));
  size_t slot_index = 0;
  for (auto& slot : slots_) {
    size_t poke_offset = (slot_count - slot_index - 1) * kXRegSize;
    switch (slot.src_.loc()) {
      case LiftoffAssembler::VarState::kStack: {
        UseScratchRegisterScope temps(asm_);
        CPURegister scratch = liftoff::AcquireByType(&temps, slot.src_.type());
        asm_->Ldr(scratch, liftoff::GetStackSlot(slot.src_index_));
        asm_->Poke(scratch, poke_offset);
        break;
      }
      case LiftoffAssembler::VarState::kRegister:
        asm_->Poke(liftoff::GetRegFromType(slot.src_.reg(), slot.src_.type()),
                   poke_offset);
        break;
      case LiftoffAssembler::VarState::KIntConst: {
        UseScratchRegisterScope temps(asm_);
        Register scratch = temps.AcquireW();
        asm_->Mov(scratch, slot.src_.i32_const());
        asm_->Poke(scratch, poke_offset);
        break;
      }
    }
    slot_index++;
  }
}

}  // namespace wasm
}  // namespace internal
}  // namespace v8

#undef BAILOUT

#endif  // V8_WASM_BASELINE_ARM64_LIFTOFF_ASSEMBLER_ARM64_H_
