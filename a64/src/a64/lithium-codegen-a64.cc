// Copyright 2013 the V8 project authors. All rights reserved.
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
//       copyright notice, this list of conditions and the following
//       disclaimer in the documentation and/or other materials provided
//       with the distribution.
//     * Neither the name of Google Inc. nor the names of its
//       contributors may be used to endorse or promote products derived
//       from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "v8.h"

#include "a64/lithium-codegen-a64.h"
#include "a64/lithium-gap-resolver-a64.h"
#include "code-stubs.h"
#include "stub-cache.h"

namespace v8 {
namespace internal {


class SafepointGenerator : public CallWrapper {
 public:
  SafepointGenerator(LCodeGen* codegen,
                     LPointerMap* pointers,
                     Safepoint::DeoptMode mode)
      : codegen_(codegen),
        pointers_(pointers),
        deopt_mode_(mode) { }
  virtual ~SafepointGenerator() { }

  virtual void BeforeCall(int call_size) const { }

  virtual void AfterCall() const {
    codegen_->RecordSafepoint(pointers_, deopt_mode_);
  }

 private:
  LCodeGen* codegen_;
  LPointerMap* pointers_;
  Safepoint::DeoptMode deopt_mode_;
};


#define __ masm()->

// Emit code to branch if the given condition holds.
// The code generated here doesn't modify the flags and they must have
// been set by some prior instructions.
//
// The EmitInverted function simply inverts the condition.
class BranchOnCondition : public BranchGenerator {
 public:
  BranchOnCondition(LCodeGen* codegen, Condition cond)
    : BranchGenerator(codegen),
      cond_(cond) { }

  virtual void Emit(Label* label) const {
    __ B(cond_, label);
  }

  virtual void EmitInverted(Label* label) const {
    if (cond_ != al) {
      __ B(InvertCondition(cond_), label);
    }
  }

 private:
  Condition cond_;
};


// Emit code to compare lhs and rhs and branch if the condition holds.
// This uses MacroAssembler's CompareAndBranch function so it will handle
// converting the comparison to Cbz/Cbnz if the right-hand side is 0.
//
// EmitInverted still compares the two operands but inverts the condition.
class CompareAndBranch : public BranchGenerator {
 public:
  CompareAndBranch(LCodeGen* codegen,
                   Condition cond,
                   const Register& lhs,
                   const Operand& rhs)
    : BranchGenerator(codegen),
      cond_(cond),
      lhs_(lhs),
      rhs_(rhs) { }

  virtual void Emit(Label* label) const {
    __ CompareAndBranch(lhs_, rhs_, cond_, label);
  }

  virtual void EmitInverted(Label* label) const {
    __ CompareAndBranch(lhs_, rhs_, InvertCondition(cond_), label);
  }

 private:
  Condition cond_;
  const Register& lhs_;
  const Operand& rhs_;
};


// Test the input with the given mask and branch if the condition holds.
// If the condition is 'eq' or 'ne' this will use MacroAssembler's
// TestAndBranchIfAllClear and TestAndBranchIfAnySet so it will handle the
// conversion to Tbz/Tbnz when possible.
class TestAndBranch : public BranchGenerator {
 public:
  TestAndBranch(LCodeGen* codegen,
                Condition cond,
                const Register& value,
                uint64_t mask)
    : BranchGenerator(codegen),
      cond_(cond),
      value_(value),
      mask_(mask) { }

  virtual void Emit(Label* label) const {
    switch (cond_) {
      case eq:
        __ TestAndBranchIfAllClear(value_, mask_, label);
        break;
      case ne:
        __ TestAndBranchIfAnySet(value_, mask_, label);
        break;
      default:
        __ Tst(value_, mask_);
        __ B(cond_, label);
    }
  }

  virtual void EmitInverted(Label* label) const {
    // The inverse of "all clear" is "any set" and vice versa.
    switch (cond_) {
      case eq:
        __ TestAndBranchIfAnySet(value_, mask_, label);
        break;
      case ne:
        __ TestAndBranchIfAllClear(value_, mask_, label);
        break;
      default:
        __ Tst(value_, mask_);
        __ B(InvertCondition(cond_), label);
    }
  }

 private:
  Condition cond_;
  const Register& value_;
  uint64_t mask_;
};


// Test the input and branch if it is non-zero and not a NaN.
class BranchIfNonZeroNumber : public BranchGenerator {
 public:
  BranchIfNonZeroNumber(LCodeGen* codegen, const FPRegister& value,
                        const FPRegister& scratch)
    : BranchGenerator(codegen), value_(value), scratch_(scratch) { }

  virtual void Emit(Label* label) const {
    __ Fabs(scratch_, value_);
    // Compare with 0.0. Because scratch_ is positive, the result can be one of
    // nZCv (equal), nzCv (greater) or nzCV (unordered).
    __ Fcmp(scratch_, 0.0);
    __ B(gt, label);
  }

  virtual void EmitInverted(Label* label) const {
    __ Fabs(scratch_, value_);
    __ Fcmp(scratch_, 0.0);
    __ B(le, label);
  }

 private:
  const FPRegister& value_;
  const FPRegister& scratch_;
};


void LCodeGen::WriteTranslation(LEnvironment* environment,
                                Translation* translation) {
  if (environment == NULL) return;

  // The translation includes one command per value in the environment.
  int translation_size = environment->translation_size();
  // The output frame height does not include the parameters.
  int height = translation_size - environment->parameter_count();

  WriteTranslation(environment->outer(), translation);
  bool has_closure_id = !info()->closure().is_null() &&
      !info()->closure().is_identical_to(environment->closure());
  int closure_id = has_closure_id
      ? DefineDeoptimizationLiteral(environment->closure())
      : Translation::kSelfLiteralId;

  switch (environment->frame_type()) {
    case JS_FUNCTION:
      translation->BeginJSFrame(environment->ast_id(), closure_id, height);
      break;
    case JS_CONSTRUCT:
      translation->BeginConstructStubFrame(closure_id, translation_size);
      break;
    case JS_GETTER:
      ASSERT(translation_size == 1);
      ASSERT(height == 0);
      translation->BeginGetterStubFrame(closure_id);
      break;
    case JS_SETTER:
      ASSERT(translation_size == 2);
      ASSERT(height == 0);
      translation->BeginSetterStubFrame(closure_id);
      break;
    case STUB:
      translation->BeginCompiledStubFrame();
      break;
    case ARGUMENTS_ADAPTOR:
      translation->BeginArgumentsAdaptorFrame(closure_id, translation_size);
      break;
    default:
      UNREACHABLE();
  }

  for (int i = 0; i < translation_size; ++i) {
    LOperand* value = environment->values()->at(i);

    // TODO(mstarzinger): Introduce marker operands to indicate that this value
    // is not present and must be reconstructed from the deoptimizer. Currently
    // this is only used for the arguments object.
    if (value == NULL) {
      int arguments_count = environment->values()->length() - translation_size;
      translation->BeginArgumentsObject(arguments_count);
      for (int i = 0; i < arguments_count; ++i) {
        LOperand* value = environment->values()->at(translation_size + i);
        AddToTranslation(translation,
                         value,
                         environment->HasTaggedValueAt(translation_size + i),
                         environment->HasUint32ValueAt(translation_size + i));
      }
      continue;
    }

    AddToTranslation(translation,
                     value,
                     environment->HasTaggedValueAt(i),
                     environment->HasUint32ValueAt(i));
  }
}


void LCodeGen::AddToTranslation(Translation* translation,
                                LOperand* op,
                                bool is_tagged,
                                bool is_uint32) {
  if (op->IsStackSlot()) {
    if (is_tagged) {
      translation->StoreStackSlot(op->index());
    } else if (is_uint32) {
      translation->StoreUint32StackSlot(op->index());
    } else {
      translation->StoreInt32StackSlot(op->index());
    }
  } else if (op->IsDoubleStackSlot()) {
    translation->StoreDoubleStackSlot(op->index());
  } else if (op->IsArgument()) {
    ASSERT(is_tagged);
    int src_index = GetStackSlotCount() + op->index();
    translation->StoreStackSlot(src_index);
  } else if (op->IsRegister()) {
    Register reg = ToRegister(op);
    if (is_tagged) {
      translation->StoreRegister(reg);
    } else if (is_uint32) {
      translation->StoreUint32Register(reg);
    } else {
      translation->StoreInt32Register(reg);
    }
  } else if (op->IsDoubleRegister()) {
    DoubleRegister reg = ToDoubleRegister(op);
    translation->StoreDoubleRegister(reg);
  } else if (op->IsConstantOperand()) {
    HConstant* constant = chunk()->LookupConstant(LConstantOperand::cast(op));
    int src_index = DefineDeoptimizationLiteral(constant->handle());
    translation->StoreLiteral(src_index);
  } else {
    UNREACHABLE();
  }
}


int LCodeGen::DefineDeoptimizationLiteral(Handle<Object> literal) {
  int result = deoptimization_literals_.length();
  for (int i = 0; i < deoptimization_literals_.length(); ++i) {
    if (deoptimization_literals_[i].is_identical_to(literal)) return i;
  }
  deoptimization_literals_.Add(literal, zone());
  return result;
}


void LCodeGen::RegisterEnvironmentForDeoptimization(LEnvironment* environment,
                                                    Safepoint::DeoptMode mode) {
  if (!environment->HasBeenRegistered()) {
    int frame_count = 0;
    int jsframe_count = 0;
    for (LEnvironment* e = environment; e != NULL; e = e->outer()) {
      ++frame_count;
      if (e->frame_type() == JS_FUNCTION) {
        ++jsframe_count;
      }
    }
    Translation translation(&translations_, frame_count, jsframe_count, zone());
    WriteTranslation(environment, &translation);
    int deoptimization_index = deoptimizations_.length();
    int pc_offset = masm()->pc_offset();
    environment->Register(deoptimization_index,
                          translation.index(),
                          (mode == Safepoint::kLazyDeopt) ? pc_offset : -1);
    deoptimizations_.Add(environment, zone());
  }
}


void LCodeGen::CallCode(Handle<Code> code,
                        RelocInfo::Mode mode,
                        LInstruction* instr) {
  CallCodeGeneric(code, mode, instr, RECORD_SIMPLE_SAFEPOINT);
}


void LCodeGen::CallCodeGeneric(Handle<Code> code,
                               RelocInfo::Mode mode,
                               LInstruction* instr,
                               SafepointMode safepoint_mode) {
  ASSERT(instr != NULL);

  Assembler::BlockConstPoolScope scope(masm_);
  LPointerMap* pointers = instr->pointer_map();
  RecordPosition(pointers->position());
  __ Call(code, mode);
  RecordSafepointWithLazyDeopt(instr, safepoint_mode);

  if ((code->kind() == Code::BINARY_OP_IC) ||
      (code->kind() == Code::COMPARE_IC)) {
    // Signal that we don't inline smi code before these stubs in the
    // optimizing code generator.
    InlineSmiCheckInfo::EmitNotInlined(masm());
  }
}


void LCodeGen::DoCallFunction(LCallFunction* instr) {
  ASSERT(ToRegister(instr->function()).Is(x1));
  ASSERT(ToRegister(instr->result()).Is(x0));

  int arity = instr->arity();
  CallFunctionStub stub(arity, NO_CALL_FUNCTION_FLAGS);
  CallCode(stub.GetCode(isolate()), RelocInfo::CODE_TARGET, instr);
  __ Ldr(cp, MemOperand(fp, StandardFrameConstants::kContextOffset));
}


void LCodeGen::DoCallNew(LCallNew* instr) {
  ASSERT(instr->IsMarkedAsCall());
  ASSERT(ToRegister(instr->constructor()).is(x1));

  __ Mov(x0, instr->arity());
  // No cell in x2 for construct type feedback in optimized code.
  Handle<Object> undefined_value(isolate()->factory()->undefined_value());
  __ Mov(x2, Operand(undefined_value));

  CallConstructStub stub(NO_CALL_FUNCTION_FLAGS);
  CallCode(stub.GetCode(isolate()), RelocInfo::CONSTRUCT_CALL, instr);

  ASSERT(ToRegister(instr->result()).is(x0));
}


void LCodeGen::DoCallNewArray(LCallNewArray* instr) {
  ASSERT(instr->IsMarkedAsCall());
  ASSERT(ToRegister(instr->constructor()).is(x1));

  __ Mov(x0, Operand(instr->arity()));
  __ Mov(x2, Operand(instr->hydrogen()->property_cell()));

  ElementsKind kind = instr->hydrogen()->elements_kind();
  bool disable_allocation_sites =
      (AllocationSiteInfo::GetMode(kind) == TRACK_ALLOCATION_SITE);

  if (instr->arity() == 0) {
    ArrayNoArgumentConstructorStub stub(kind, disable_allocation_sites);
    CallCode(stub.GetCode(isolate()), RelocInfo::CONSTRUCT_CALL, instr);
  } else if (instr->arity() == 1) {
    Label done;
    if (IsFastPackedElementsKind(kind)) {
      Label packed_case;

      // We might need to create a holey array; look at the first argument.
      __ Peek(x10, 0);
      __ Cbz(x10, &packed_case);

      ElementsKind holey_kind = GetHoleyElementsKind(kind);
      ArraySingleArgumentConstructorStub stub(holey_kind,
                                              disable_allocation_sites);
      CallCode(stub.GetCode(isolate()), RelocInfo::CONSTRUCT_CALL, instr);
      __ B(&done);
      __ Bind(&packed_case);
    }

    ArraySingleArgumentConstructorStub stub(kind, disable_allocation_sites);
    CallCode(stub.GetCode(isolate()), RelocInfo::CONSTRUCT_CALL, instr);
    __ Bind(&done);
  } else {
    ArrayNArgumentsConstructorStub stub(kind, disable_allocation_sites);
    CallCode(stub.GetCode(isolate()), RelocInfo::CONSTRUCT_CALL, instr);
  }

  ASSERT(ToRegister(instr->result()).is(x0));
}


void LCodeGen::CallRuntime(const Runtime::Function* function,
                           int num_arguments,
                           LInstruction* instr) {
  ASSERT(instr != NULL);
  LPointerMap* pointers = instr->pointer_map();
  ASSERT(pointers != NULL);
  RecordPosition(pointers->position());

  __ CallRuntime(function, num_arguments);
  RecordSafepointWithLazyDeopt(instr, RECORD_SIMPLE_SAFEPOINT);
}


void LCodeGen::CallRuntimeFromDeferred(Runtime::FunctionId id,
                                       int argc,
                                       LInstruction* instr) {
  __ CallRuntimeSaveDoubles(id);
  RecordSafepointWithRegisters(
      instr->pointer_map(), argc, Safepoint::kNoLazyDeopt);
}


void LCodeGen::RecordPosition(int position) {
  if (position == RelocInfo::kNoPosition) return;
  masm()->positions_recorder()->RecordPosition(position);
}


void LCodeGen::RecordSafepointWithLazyDeopt(LInstruction* instr,
                                            SafepointMode safepoint_mode) {
  if (safepoint_mode == RECORD_SIMPLE_SAFEPOINT) {
    RecordSafepoint(instr->pointer_map(), Safepoint::kLazyDeopt);
  } else {
    ASSERT(safepoint_mode == RECORD_SAFEPOINT_WITH_REGISTERS_AND_NO_ARGUMENTS);
    RecordSafepointWithRegisters(
        instr->pointer_map(), 0, Safepoint::kLazyDeopt);
  }
}


void LCodeGen::RecordSafepoint(LPointerMap* pointers,
                               Safepoint::Kind kind,
                               int arguments,
                               Safepoint::DeoptMode deopt_mode) {
  ASSERT(expected_safepoint_kind_ == kind);

  const ZoneList<LOperand*>* operands = pointers->GetNormalizedOperands();
  Safepoint safepoint = safepoints_.DefineSafepoint(
      masm(), kind, arguments, deopt_mode);

  for (int i = 0; i < operands->length(); i++) {
    LOperand* pointer = operands->at(i);
    if (pointer->IsStackSlot()) {
      safepoint.DefinePointerSlot(pointer->index(), zone());
    } else if (pointer->IsRegister() && (kind & Safepoint::kWithRegisters)) {
      safepoint.DefinePointerRegister(ToRegister(pointer), zone());
    }
  }

  if (kind & Safepoint::kWithRegisters) {
    // Register cp always contains a pointer to the context.
    safepoint.DefinePointerRegister(cp, zone());
  }
}

void LCodeGen::RecordSafepoint(LPointerMap* pointers,
                               Safepoint::DeoptMode deopt_mode) {
  RecordSafepoint(pointers, Safepoint::kSimple, 0, deopt_mode);
}


void LCodeGen::RecordSafepoint(Safepoint::DeoptMode deopt_mode) {
  LPointerMap empty_pointers(RelocInfo::kNoPosition, zone());
  RecordSafepoint(&empty_pointers, deopt_mode);
}


void LCodeGen::RecordSafepointWithRegisters(LPointerMap* pointers,
                                            int arguments,
                                            Safepoint::DeoptMode deopt_mode) {
  RecordSafepoint(
      pointers, Safepoint::kWithRegisters, arguments, deopt_mode);
}


bool LCodeGen::GenerateCode() {
  LPhase phase("Z_Code generation", chunk());
  ASSERT(is_unused());
  status_ = GENERATING;

  // Open a frame scope to indicate that there is a frame on the stack.  The
  // NONE indicates that the scope shouldn't actually generate code to set up
  // the frame (that is done in GeneratePrologue).
  FrameScope frame_scope(masm_, StackFrame::NONE);

  return GeneratePrologue() &&
      GenerateBody() &&
      GenerateDeferredCode() &&
      GenerateDeoptJumpTable() &&
      GenerateSafepointTable();
}


bool LCodeGen::GeneratePrologue() {
  ASSERT(is_generating());

  if (info()->IsOptimizing()) {
    ProfileEntryHookStub::MaybeCallEntryHook(masm_);

    // TODO(all): Add support for stop_t FLAG in DEBUG mode.

    // Strict mode functions and builtins need to replace the receiver
    // with undefined when called as functions (without an explicit
    // receiver object).
    // x5 holds the call kind and is zero for method calls and non-zero for
    // function calls.
    if (!info_->is_classic_mode() || info_->is_native()) {
      Label ok;
      __ Cbz(x5, &ok);
      int receiver_offset = scope()->num_parameters() * kPointerSize;
      __ LoadRoot(x10, Heap::kUndefinedValueRootIndex);
      __ Poke(x10, receiver_offset);
      __ Bind(&ok);
    }
  }

  ASSERT(__ StackPointer().Is(jssp));
  info()->set_prologue_offset(masm_->pc_offset());
  if (NeedsEagerFrame()) {
    if (info()->IsStub()) {
      // TODO(jbramley): Does x1 contain a JSFunction here, or does it already
      // have the special STUB smi?
      __ Mov(x10, Operand(Smi::FromInt(StackFrame::STUB)));
      // Compiled stubs don't age, and so they don't need the predictable code
      // ageing sequence.
      __ Push(lr, fp, cp, x10);
      __ Add(fp, jssp, 2 * kPointerSize);
    } else {
      // This call emits the following sequence in a way that can be patched for
      // code ageing support:
      //  Push(lr, fp, cp, x1);
      //  Add(fp, jssp, 2 * kPointerSize);
      __ EmitFrameSetupForCodeAgePatching();
    }
    frame_is_built_ = true;
    info_->AddNoFrameRange(0, masm_->pc_offset());
  }

  // Reserve space for the stack slots needed by the code.
  int slots = GetStackSlotCount();
  if (slots > 0) {
    __ Claim(slots, kPointerSize);
  }

  if (info()->saves_caller_doubles()) {
    Comment(";;; Save clobbered callee double registers");
    ASSERT(NeedsEagerFrame());
    BitVector* doubles = chunk()->allocated_double_registers();
    BitVector::Iterator iterator(doubles);
    int count = 0;
    while (!iterator.Done()) {
      FPRegister value = FPRegister::FromAllocationIndex(iterator.Current());
      // TODO(jbramley): Make Poke support FPRegisters.
      __ Str(value, MemOperand(__ StackPointer(), count * kDoubleSize));
      iterator.Advance();
      count++;
    }
  }

  // Allocate a local context if needed.
  int heap_slots = info()->num_heap_slots() - Context::MIN_CONTEXT_SLOTS;
  if (heap_slots > 0) {
    Comment(";;; Allocate local context");
    // Argument to NewContext is the function, which is in x1.
    __ Push(x1);
    if (heap_slots <= FastNewContextStub::kMaximumSlots) {
      FastNewContextStub stub(heap_slots);
      __ CallStub(&stub);
    } else {
      __ CallRuntime(Runtime::kNewFunctionContext, 1);
    }
    RecordSafepoint(Safepoint::kNoLazyDeopt);
    // Context is returned in both x0 and cp. It replaces the context passed to
    // us. It's saved in the stack and kept live in cp.
    __ Str(cp, MemOperand(fp, StandardFrameConstants::kContextOffset));
    // Copy any necessary parameters into the context.
    int num_parameters = scope()->num_parameters();
    for (int i = 0; i < num_parameters; i++) {
      Variable* var = scope()->parameter(i);
      if (var->IsContextSlot()) {
        Register value = x0;
        Register scratch = x3;

        int parameter_offset = StandardFrameConstants::kCallerSPOffset +
            (num_parameters - 1 - i) * kPointerSize;
        // Load parameter from stack.
        __ Ldr(value, MemOperand(fp, parameter_offset));
        // Store it in the context.
        MemOperand target = ContextMemOperand(cp, var->index());
        __ Str(value, target);
        // Update the write barrier. This clobbers value and scratch.
        __ RecordWriteContextSlot(cp, target.offset(), value, scratch,
                                  GetLinkRegisterState(), kSaveFPRegs);
      }
    }
    Comment(";;; End allocate local context");
  }

  // Trace the call.
  if (FLAG_trace && info()->IsOptimizing()) {
    __ CallRuntime(Runtime::kTraceEnter, 0);
  }

  return !is_aborted();
}


bool LCodeGen::GenerateBody() {
  ASSERT(is_generating());
  bool emit_instructions = true;

  for (current_instruction_ = 0;
       !is_aborted() && (current_instruction_ < instructions_->length());
       current_instruction_++) {
    LInstruction* instr = instructions_->at(current_instruction_);

    // Don't emit code for basic blocks with a replacement.
    if (instr->IsLabel()) {
      emit_instructions = !LLabel::cast(instr)->HasReplacement();
    }
    if (!emit_instructions) continue;

    if (FLAG_code_comments && instr->HasInterestingComment(this)) {
      Comment(";;; <@%d,#%d> %s",
              current_instruction_,
              instr->hydrogen_value()->id(),
              instr->Mnemonic());
    }

    instr->CompileToNative(this);
  }
  EnsureSpaceForLazyDeopt();
  return !is_aborted();
}


bool LCodeGen::GenerateDeferredCode() {
  ASSERT(is_generating());
  if (deferred_.length() > 0) {
    for (int i = 0; !is_aborted() && (i < deferred_.length()); i++) {
      LDeferredCode* code = deferred_[i];

      Comment(";;; <@%d,#%d> "
              "-------------------- Deferred %s --------------------",
              code->instruction_index(),
              code->instr()->hydrogen_value()->id(),
              code->instr()->Mnemonic());

      __ Bind(code->entry());

      if (NeedsDeferredFrame()) {
        Comment(";;; Build frame");
        ASSERT(!frame_is_built_);
        ASSERT(info()->IsStub());
        frame_is_built_ = true;
        __ Push(lr, fp, cp);
        __ Mov(fp, Operand(Smi::FromInt(StackFrame::STUB)));
        __ Push(fp);
        __ Add(fp, __ StackPointer(), 2 * kPointerSize);
        Comment(";;; Deferred code");
      }

      code->Generate();

      if (NeedsDeferredFrame()) {
        Comment(";;; Destroy frame");
        ASSERT(frame_is_built_);
        __ Pop(xzr, cp, fp, lr);
        frame_is_built_ = false;
      }

      __ B(code->exit());
    }
  }

  // Force constant pool emission at the end of the deferred code to make
  // sure that no constant pools are emitted after deferred code because
  // deferred code generation is the last step which generates code. The two
  // following steps will only output data used by crakshaft.
  masm()->CheckConstPool(true, false);

  return !is_aborted();
}


bool LCodeGen::GenerateDeoptJumpTable() {
  TODO_UNIMPLEMENTED("generate level 1 deopt table");

  // TODO(jbramley): On ARM, the deopt entry for stubs is different in that it
  // inserts a special marker instead of a function pointer. We need to do that
  // same on A64, but since we don't use the jump table, we have to do it
  // in LCodeGen::Deoptimize().

  // The deoptimization jump table is the last part of the instruction
  // sequence. Mark the generated code as done unless we bailed out.
  if (!is_aborted()) status_ = DONE;
  return !is_aborted();
}


bool LCodeGen::GenerateSafepointTable() {
  ASSERT(is_done());
  safepoints_.Emit(masm(), GetStackSlotCount());
  return !is_aborted();
}


void LCodeGen::FinishCode(Handle<Code> code) {
  ASSERT(is_done());
  code->set_stack_slots(GetStackSlotCount());
  code->set_safepoint_table_offset(safepoints_.GetCodeOffset());
  if (FLAG_weak_embedded_maps_in_optimized_code) {
    RegisterDependentCodeForEmbeddedMaps(code);
  }
  PopulateDeoptimizationData(code);
  info()->CommitDependencies(code);
}


void LCodeGen::Abort(const char* reason) {
  info()->set_bailout_reason(reason);
  status_ = ABORTED;
}


void LCodeGen::Comment(const char* format, ...) {
  if (!FLAG_code_comments) return;
  char buffer[4 * KB];
  StringBuilder builder(buffer, ARRAY_SIZE(buffer));
  va_list arguments;
  va_start(arguments, format);
  builder.AddFormattedList(format, arguments);
  va_end(arguments);

  // Copy the string before recording it in the assembler to avoid
  // issues when the stack allocated buffer goes out of scope.
  size_t length = builder.position();
  Vector<char> copy = Vector<char>::New(length + 1);
  memcpy(copy.start(), builder.Finalize(), copy.length());
  masm()->RecordComment(copy.start());
}


void LCodeGen::RegisterDependentCodeForEmbeddedMaps(Handle<Code> code) {
  ZoneList<Handle<Map> > maps(1, zone());
  int mode_mask = RelocInfo::ModeMask(RelocInfo::EMBEDDED_OBJECT);
  for (RelocIterator it(*code, mode_mask); !it.done(); it.next()) {
    RelocInfo::Mode mode = it.rinfo()->rmode();
    if (mode == RelocInfo::EMBEDDED_OBJECT &&
        it.rinfo()->target_object()->IsMap()) {
      Handle<Map> map(Map::cast(it.rinfo()->target_object()));
      if (map->CanTransition()) {
        maps.Add(map, zone());
      }
    }
  }
#ifdef VERIFY_HEAP
  // This disables verification of weak embedded maps after full GC.
  // AddDependentCode can cause a GC, which would observe the state where
  // this code is not yet in the depended code lists of the embedded maps.
  NoWeakEmbeddedMapsVerificationScope disable_verification_of_embedded_maps;
#endif
  for (int i = 0; i < maps.length(); i++) {
    maps.at(i)->AddDependentCode(DependentCode::kWeaklyEmbeddedGroup, code);
  }
}


void LCodeGen::PopulateDeoptimizationData(Handle<Code> code) {
  int length = deoptimizations_.length();
  if (length == 0) return;

  Handle<DeoptimizationInputData> data =
      factory()->NewDeoptimizationInputData(length, TENURED);

  Handle<ByteArray> translations =
      translations_.CreateByteArray(isolate()->factory());
  data->SetTranslationByteArray(*translations);
  data->SetInlinedFunctionCount(Smi::FromInt(inlined_function_count_));

  Handle<FixedArray> literals =
      factory()->NewFixedArray(deoptimization_literals_.length(), TENURED);
  { AllowDeferredHandleDereference copy_handles;
    for (int i = 0; i < deoptimization_literals_.length(); i++) {
      literals->set(i, *deoptimization_literals_[i]);
    }
    data->SetLiteralArray(*literals);
  }

  data->SetOsrAstId(Smi::FromInt(info_->osr_ast_id().ToInt()));
  data->SetOsrPcOffset(Smi::FromInt(osr_pc_offset_));

  // Populate the deoptimization entries.
  for (int i = 0; i < length; i++) {
    LEnvironment* env = deoptimizations_[i];
    data->SetAstId(i, env->ast_id());
    data->SetTranslationIndex(i, Smi::FromInt(env->translation_index()));
    data->SetArgumentsStackHeight(i,
                                  Smi::FromInt(env->arguments_stack_height()));
    data->SetPc(i, Smi::FromInt(env->pc_offset()));
  }

  code->set_deoptimization_data(*data);
}


void LCodeGen::PopulateDeoptimizationLiteralsWithInlinedFunctions() {
  ASSERT(deoptimization_literals_.length() == 0);

  const ZoneList<Handle<JSFunction> >* inlined_closures =
      chunk()->inlined_closures();

  for (int i = 0, length = inlined_closures->length(); i < length; i++) {
    DefineDeoptimizationLiteral(inlined_closures->at(i));
  }

  inlined_function_count_ = deoptimization_literals_.length();
}


void LCodeGen::Deoptimize(LEnvironment* environment,
                          Deoptimizer::BailoutType bailout_type) {
  RegisterEnvironmentForDeoptimization(environment, Safepoint::kNoLazyDeopt);
  ASSERT(environment->HasBeenRegistered());
  ASSERT(info()->IsOptimizing() || info()->IsStub());
  int id = environment->deoptimization_index();
  Address entry =
      Deoptimizer::GetDeoptimizationEntry(isolate(), id, bailout_type);

  if (entry == NULL) {
    Abort("bailout was not prepared");
    return;
  }

  TODO_UNIMPLEMENTED("Add support for deopt_every_n_times flag.");
  TODO_UNIMPLEMENTED("Add support for trap_on_deopt flag.");

  // TODO(all): Currently this code directly jump to the second level deopt
  // table entry. This code need to be updated if we decide to use the
  // 2 levels of table.
  ASSERT(info()->IsStub() || frame_is_built_);
  bool needs_lazy_deopt = info()->IsStub();
  if (frame_is_built_) {
    if (needs_lazy_deopt) {
      __ Call(entry, RelocInfo::RUNTIME_ENTRY);
    } else {
      __ Jump(entry, RelocInfo::RUNTIME_ENTRY);
    }
  } else {
    // We need to build a frame to deoptimize a stub. Because stubs don't have a
    // function pointer to put in the frame, put a special marker there instead.
    // TODO(jbramley): In other architectures, this happens in the jump table.
    // This is a temporary hack until we implement jump tables in A64.
    __ Mov(__ Tmp1(), Operand(Smi::FromInt(StackFrame::STUB)));
    __ Push(lr, fp, cp, __ Tmp1());
    __ Add(fp, __ StackPointer(), 2 * kPointerSize);
    // TODO(jbramley): Can this be a jump, rather than a call?
    __ Call(entry, RelocInfo::RUNTIME_ENTRY);
  }
}


void LCodeGen::Deoptimize(LEnvironment* environment) {
  Deoptimizer::BailoutType bailout_type = info()->IsStub() ? Deoptimizer::LAZY
                                                           : Deoptimizer::EAGER;
  Deoptimize(environment, bailout_type);
}


void LCodeGen::SoftDeoptimize(LEnvironment* environment) {
  ASSERT(!info()->IsStub());
  Deoptimize(environment, Deoptimizer::SOFT);
}


void LCodeGen::DeoptimizeIf(Condition cond, LEnvironment* environment) {
  Label dont_deopt;
  __ B(InvertCondition(cond), &dont_deopt);
  Deoptimize(environment);
  __ Bind(&dont_deopt);
}


void LCodeGen::DeoptimizeIfZero(Register rt, LEnvironment* environment) {
  Label dont_deopt;
  __ Cbnz(rt, &dont_deopt);
  Deoptimize(environment);
  __ Bind(&dont_deopt);
}


void LCodeGen::DeoptimizeIfNegative(Register rt, LEnvironment* environment) {
  Label dont_deopt;
  __ Tbz(rt, rt.Is64Bits() ? kXSignBit : kWSignBit, &dont_deopt);
  Deoptimize(environment);
  __ Bind(&dont_deopt);
}


void LCodeGen::DeoptimizeIfSmi(Register rt,
                               LEnvironment* environment) {
  Label dont_deopt;
  __ JumpIfNotSmi(rt, &dont_deopt);
  Deoptimize(environment);
  __ Bind(&dont_deopt);
}


void LCodeGen::DeoptimizeIfNotSmi(Register rt, LEnvironment* environment) {
  Label dont_deopt;
  __ JumpIfSmi(rt, &dont_deopt);
  Deoptimize(environment);
  __ Bind(&dont_deopt);
}


void LCodeGen::DeoptimizeIfRoot(Register rt,
                                Heap::RootListIndex index,
                                LEnvironment* environment) {
  Label dont_deopt;
  __ JumpIfNotRoot(rt, index, &dont_deopt);
  Deoptimize(environment);
  __ Bind(&dont_deopt);
}


void LCodeGen::DeoptimizeIfNotRoot(Register rt,
                                   Heap::RootListIndex index,
                                   LEnvironment* environment) {
  Label dont_deopt;
  __ JumpIfRoot(rt, index, &dont_deopt);
  Deoptimize(environment);
  __ Bind(&dont_deopt);
}


void LCodeGen::EnsureSpaceForLazyDeopt() {
  if (info()->IsStub()) return;
  // Ensure that we have enough space after the previous lazy-bailout
  // instruction for patching the code here.
  intptr_t current_pc = masm()->pc_offset();
  int patch_size = Deoptimizer::patch_size();

  if (current_pc < (last_lazy_deopt_pc_ + patch_size)) {
    intptr_t padding_size = last_lazy_deopt_pc_ + patch_size - current_pc;
    ASSERT((padding_size % kInstructionSize) == 0);
    InstructionAccurateScope instruction_accurate(
        masm(), padding_size / kInstructionSize);

    while (padding_size > 0) {
      __ nop();
      padding_size -= kInstructionSize;
    }
  }
  last_lazy_deopt_pc_ = masm()->pc_offset();
}


Register LCodeGen::ToRegister(LOperand* op) const {
  // TODO(all): support zero register results, as ToRegister32.
  ASSERT((op != NULL) && op->IsRegister());
  return Register::FromAllocationIndex(op->index());
}


Register LCodeGen::ToRegister32(LOperand* op) const {
  ASSERT(op != NULL);
  if (op->IsConstantOperand()) {
    // If this is a constant operand, the result must be the zero register.
    ASSERT(ToInteger32(LConstantOperand::cast(op)) == 0);
    return wzr;
  } else {
    return ToRegister(op).W();
  }
}


Smi* LCodeGen::ToSmi(LConstantOperand* op) const {
  HConstant* constant = chunk_->LookupConstant(op);
  return Smi::FromInt(constant->Integer32Value());
}


DoubleRegister LCodeGen::ToDoubleRegister(LOperand* op) const {
  ASSERT((op != NULL) && op->IsDoubleRegister());
  return DoubleRegister::FromAllocationIndex(op->index());
}


Operand LCodeGen::ToOperand(LOperand* op) {
  ASSERT(op != NULL);
  if (op->IsConstantOperand()) {
    LConstantOperand* const_op = LConstantOperand::cast(op);
    HConstant* constant = chunk()->LookupConstant(const_op);
    Representation r = chunk_->LookupLiteralRepresentation(const_op);
    if (r.IsInteger32()) {
      ASSERT(constant->HasInteger32Value());
      return Operand(constant->Integer32Value());
    } else if (r.IsDouble()) {
      Abort("ToOperand unsupported double immediate.");
    }
    ASSERT(r.IsTagged());
    return Operand(constant->handle());
  } else if (op->IsRegister()) {
    return Operand(ToRegister(op));
  } else if (op->IsDoubleRegister()) {
    Abort("ToOperand IsDoubleRegister unimplemented");
    return Operand(0);
  }
  // Stack slots not implemented, use ToMemOperand instead.
  UNREACHABLE();
  return Operand(0);
}


Operand LCodeGen::ToOperand32(LOperand* op) {
  ASSERT(op != NULL);
  if (op->IsRegister()) {
    return Operand(ToRegister32(op));
  } else if (op->IsConstantOperand()) {
    LConstantOperand* const_op = LConstantOperand::cast(op);
    HConstant* constant = chunk()->LookupConstant(const_op);
    Representation r = chunk_->LookupLiteralRepresentation(const_op);
    if (r.IsInteger32()) {
      ASSERT(constant->HasInteger32Value());
      return Operand(constant->Integer32Value());
    } else {
      // Other constants not implemented.
      Abort("ToOperand32 unsupported immediate.");
    }
  }
  // Other cases are not implemented.
  UNREACHABLE();
  return Operand(0);
}


MemOperand LCodeGen::ToMemOperand(LOperand* op) const {
  ASSERT(op != NULL);
  ASSERT(!op->IsRegister());
  ASSERT(!op->IsDoubleRegister());
  ASSERT(op->IsStackSlot() || op->IsDoubleStackSlot());
  return MemOperand(fp, StackSlotOffset(op->index()));
}


Handle<Object> LCodeGen::ToHandle(LConstantOperand* op) const {
  HConstant* constant = chunk_->LookupConstant(op);
  ASSERT(chunk_->LookupLiteralRepresentation(op).IsSmiOrTagged());
  return constant->handle();
}


bool LCodeGen::IsSmi(LConstantOperand* op) const {
  return chunk_->LookupLiteralRepresentation(op).IsSmi();
}


bool LCodeGen::IsInteger32Constant(LConstantOperand* op) const {
  return op->IsConstantOperand() &&
      chunk_->LookupLiteralRepresentation(op).IsSmiOrInteger32();
}


int32_t LCodeGen::ToInteger32(LConstantOperand* op) const {
  HConstant* constant = chunk_->LookupConstant(op);
  return constant->Integer32Value();
}


double LCodeGen::ToDouble(LConstantOperand* op) const {
  HConstant* constant = chunk_->LookupConstant(op);
  ASSERT(constant->HasDoubleValue());
  return constant->DoubleValue();
}


Condition LCodeGen::TokenToCondition(Token::Value op, bool is_unsigned) {
  Condition cond = nv;
  switch (op) {
    case Token::EQ:
    case Token::EQ_STRICT:
      cond = eq;
      break;
    case Token::LT:
      cond = is_unsigned ? lo : lt;
      break;
    case Token::GT:
      cond = is_unsigned ? hi : gt;
      break;
    case Token::LTE:
      cond = is_unsigned ? ls : le;
      break;
    case Token::GTE:
      cond = is_unsigned ? hs : ge;
      break;
    case Token::IN:
    case Token::INSTANCEOF:
    default:
      UNREACHABLE();
  }
  return cond;
}


template<class InstrType>
void LCodeGen::EmitBranchGeneric(InstrType instr,
                                 const BranchGenerator& branch) {
  int left_block = instr->TrueDestination(chunk_);
  int right_block = instr->FalseDestination(chunk_);

  int next_block = GetNextEmittedBlock();

  if (right_block == left_block) {
    EmitGoto(left_block);
  } else if (left_block == next_block) {
    branch.EmitInverted(chunk_->GetAssemblyLabel(right_block));
  } else if (right_block == next_block) {
    branch.Emit(chunk_->GetAssemblyLabel(left_block));
  } else {
    branch.Emit(chunk_->GetAssemblyLabel(left_block));
    __ B(chunk_->GetAssemblyLabel(right_block));
  }
}


template<class InstrType>
void LCodeGen::EmitBranch(InstrType instr, Condition condition) {
  BranchOnCondition branch(this, condition);
  EmitBranchGeneric(instr, branch);
}


template<class InstrType>
void LCodeGen::EmitCompareAndBranch(InstrType instr,
                                    Condition condition,
                                    const Register& lhs,
                                    const Operand& rhs) {
  CompareAndBranch branch(this, condition, lhs, rhs);
  EmitBranchGeneric(instr, branch);
}


template<class InstrType>
void LCodeGen::EmitTestAndBranch(InstrType instr,
                                 Condition condition,
                                 const Register& value,
                                 uint64_t mask) {
  TestAndBranch branch(this, condition, value, mask);
  EmitBranchGeneric(instr, branch);
}


template<class InstrType>
void LCodeGen::EmitBranchIfNonZeroNumber(InstrType instr,
                                         const FPRegister& value,
                                         const FPRegister& scratch) {
  BranchIfNonZeroNumber branch(this, value, scratch);
  EmitBranchGeneric(instr, branch);
}


void LCodeGen::DoGap(LGap* gap) {
  for (int i = LGap::FIRST_INNER_POSITION;
       i <= LGap::LAST_INNER_POSITION;
       i++) {
    LGap::InnerPosition inner_pos = static_cast<LGap::InnerPosition>(i);
    LParallelMove* move = gap->GetParallelMove(inner_pos);
    if (move != NULL) {
      resolver_.Resolve(move);
    }
  }
}


void LCodeGen::DoAccessArgumentsAt(LAccessArgumentsAt* instr) {
  Register arguments = ToRegister(instr->arguments());
  Register result = ToRegister(instr->result());

  if (instr->length()->IsConstantOperand() &&
      instr->index()->IsConstantOperand()) {
    ASSERT(instr->temp() == NULL);
    int index = ToInteger32(LConstantOperand::cast(instr->index()));
    int length = ToInteger32(LConstantOperand::cast(instr->length()));
    int offset = ((length - index) + 1) * kPointerSize;
    __ Ldr(result, MemOperand(arguments, offset));
  } else {
    ASSERT(instr->temp() != NULL);
    Register temp = ToRegister32(instr->temp());
    Register length = ToRegister32(instr->length());
    Operand index = ToOperand32(instr->index());
    // There are two words between the frame pointer and the last arguments.
    // Subtracting from length accounts for only one, so we add one more.
    __ Sub(temp, length, index);
    __ Add(temp, temp, 1);
    __ Ldr(result, MemOperand(arguments, temp, UXTW, kPointerSizeLog2));
  }
}


void LCodeGen::DoAddI(LAddI* instr) {
  bool can_overflow = instr->hydrogen()->CheckFlag(HValue::kCanOverflow);
  Register result = ToRegister32(instr->result());
  Register left = ToRegister32(instr->left());
  Operand right = ToOperand32(instr->right());
  if (can_overflow) {
    __ Adds(result, left, right);
    DeoptimizeIf(vs, instr->environment());
  } else {
    __ Add(result, left, right);
  }
}


void LCodeGen::DoAllocate(LAllocate* instr) {
  class DeferredAllocate: public LDeferredCode {
   public:
    DeferredAllocate(LCodeGen* codegen, LAllocate* instr)
        : LDeferredCode(codegen), instr_(instr) { }
    virtual void Generate() { codegen()->DoDeferredAllocate(instr_); }
    virtual LInstruction* instr() { return instr_; }
   private:
    LAllocate* instr_;
  };

  DeferredAllocate* deferred = new(zone()) DeferredAllocate(this, instr);

  Register result = ToRegister(instr->result());
  Register temp1 = ToRegister(instr->temp1());
  Register temp2 = ToRegister(instr->temp2());

  // Allocate memory for the object.
  AllocationFlags flags = TAG_OBJECT;
  if (instr->hydrogen()->MustAllocateDoubleAligned()) {
    flags = static_cast<AllocationFlags>(flags | DOUBLE_ALIGNMENT);
  }

  if (instr->hydrogen()->CanAllocateInOldPointerSpace()) {
    ASSERT(!instr->hydrogen()->CanAllocateInOldDataSpace());
    flags = static_cast<AllocationFlags>(flags | PRETENURE_OLD_POINTER_SPACE);
  } else if (instr->hydrogen()->CanAllocateInOldDataSpace()) {
    flags = static_cast<AllocationFlags>(flags | PRETENURE_OLD_DATA_SPACE);
  }

  if (instr->size()->IsConstantOperand()) {
    int32_t size = ToInteger32(LConstantOperand::cast(instr->size()));
    __ Allocate(size, result, temp1, temp2, deferred->entry(), flags);
  } else {
    Register size = ToRegister(instr->size());
    __ Allocate(size, result, temp1, temp2, deferred->entry(), flags);
  }

  __ Bind(deferred->exit());
}


void LCodeGen::DoDeferredAllocate(LAllocate* instr) {
  Register result = ToRegister(instr->result());

  // TODO(3095996): Get rid of this. For now, we need to make the
  // result register contain a valid pointer because it is already
  // contained in the register pointer map.
  __ Mov(result, Operand(Smi::FromInt(0)));

  PushSafepointRegistersScope scope(this, Safepoint::kWithRegisters);
  if (instr->size()->IsConstantOperand()) {
    int32_t size = ToInteger32(LConstantOperand::cast(instr->size()));
    // Use result as a scratch register.
    __ Mov(result, Operand(Smi::FromInt(size)));
    __ Push(result);
  } else {
    Register size = ToRegister(instr->size());
    __ SmiTag(size);
    __ Push(size);
  }
  if (instr->hydrogen()->CanAllocateInOldPointerSpace()) {
    ASSERT(!instr->hydrogen()->CanAllocateInOldDataSpace());
    CallRuntimeFromDeferred(Runtime::kAllocateInOldPointerSpace, 1, instr);
  } else if (instr->hydrogen()->CanAllocateInOldDataSpace()) {
    CallRuntimeFromDeferred(Runtime::kAllocateInOldDataSpace, 1, instr);
  } else {
    CallRuntimeFromDeferred(Runtime::kAllocateInNewSpace, 1, instr);
  }
  __ StoreToSafepointRegisterSlot(x0, result);
}


void LCodeGen::DoAllocateObject(LAllocateObject* instr) {
  class DeferredAllocateObject: public LDeferredCode {
   public:
    DeferredAllocateObject(LCodeGen* codegen, LAllocateObject* instr)
        : LDeferredCode(codegen), instr_(instr) { }
    virtual void Generate() { codegen()->DoDeferredAllocateObject(instr_); }
    virtual LInstruction* instr() { return instr_; }
   private:
    LAllocateObject* instr_;
  };

  DeferredAllocateObject* deferred =
      new(zone()) DeferredAllocateObject(this, instr);

  Register result = ToRegister(instr->result());
  Register scratch1 = ToRegister(instr->temp1());
  Register scratch2 = ToRegister(instr->temp2());
  Handle<JSFunction> constructor = instr->hydrogen()->constructor();
  Handle<Map> initial_map = instr->hydrogen()->constructor_initial_map();
  int instance_size = initial_map->instance_size();

  ASSERT(initial_map->pre_allocated_property_fields() +
         initial_map->unused_property_fields() -
         initial_map->inobject_properties() == 0);

  __ Allocate(instance_size, result, scratch1, scratch2, deferred->entry(),
              TAG_OBJECT);

  __ Bind(deferred->exit());
  if (FLAG_debug_code) {
    Label is_in_new_space;
    __ JumpIfInNewSpace(result, &is_in_new_space);
    __ Abort("Allocated object is not in new-space");
    __ Bind(&is_in_new_space);
  }

  // Load the initial map.
  Register map = scratch1;
  __ LoadHeapObject(map, constructor);
  __ Ldr(map, FieldMemOperand(map, JSFunction::kPrototypeOrInitialMapOffset));

  // Initialize map and field of the newly allocated object.
  ASSERT(initial_map->instance_type() == JS_OBJECT_TYPE);
  __ Str(map, FieldMemOperand(result, JSObject::kMapOffset));

  Register empty_array = scratch1;
  __ LoadRoot(empty_array, Heap::kEmptyFixedArrayRootIndex);
  __ Str(empty_array, FieldMemOperand(result, JSObject::kElementsOffset));
  __ Str(empty_array, FieldMemOperand(result, JSObject::kPropertiesOffset));

  if (initial_map->inobject_properties() != 0) {
    Register undef = scratch1;
    __ LoadRoot(undef, Heap::kUndefinedValueRootIndex);
    for (int i = 0; i < initial_map->inobject_properties(); i++) {
      int property_offset = JSObject::kHeaderSize + i * kPointerSize;
      __ Str(undef, FieldMemOperand(result, property_offset));
    }
  }
}


void LCodeGen::DoDeferredAllocateObject(LAllocateObject* instr) {
  Register result = ToRegister(instr->result());
  Handle<Map> initial_map = instr->hydrogen()->constructor_initial_map();
  int instance_size = initial_map->instance_size();


  // TODO(3095996): Get rid of this. For now, we need to make the
  // result register contain a valid pointer because it is already
  // contained in the register pointer map.
  __ Mov(result, 0);

  PushSafepointRegistersScope scope(this, Safepoint::kWithRegisters);
  __ Mov(x0, Operand(Smi::FromInt(instance_size)));
  __ Push(x0);
  CallRuntimeFromDeferred(Runtime::kAllocateInNewSpace, 1, instr);
  __ StoreToSafepointRegisterSlot(x0, result);
}


void LCodeGen::DoApplyArguments(LApplyArguments* instr) {
  Register receiver = ToRegister(instr->receiver());
  Register function = ToRegister(instr->function());
  Register length = ToRegister(instr->length());
  Register elements = ToRegister(instr->elements());
  Register scratch = x5;
  ASSERT(receiver.Is(x0));  // Used for parameter count.
  ASSERT(function.Is(x1));  // Required by InvokeFunction.
  ASSERT(ToRegister(instr->result()).Is(x0));
  ASSERT(instr->IsMarkedAsCall());

  // Copy the arguments to this function possibly from the
  // adaptor frame below it.
  const uint32_t kArgumentsLimit = 1 * KB;
  __ Cmp(length, kArgumentsLimit);
  DeoptimizeIf(hi, instr->environment());

  // Push the receiver and use the register to keep the original
  // number of arguments.
  __ Push(receiver);
  Register argc = receiver;
  receiver = NoReg;
  __ Mov(argc, length);
  // The arguments are at a one pointer size offset from elements.
  __ Add(elements, elements, 1 * kPointerSize);

  // Loop through the arguments pushing them onto the execution
  // stack.
  Label invoke, loop;
  // length is a small non-negative integer, due to the test above.
  __ Cbz(length, &invoke);
  __ Bind(&loop);
  __ Ldr(scratch, MemOperand(elements, length, LSL, kPointerSizeLog2));
  __ Push(scratch);
  __ Subs(length, length, 1);
  __ B(ne, &loop);

  __ Bind(&invoke);
  ASSERT(instr->HasPointerMap());
  LPointerMap* pointers = instr->pointer_map();
  RecordPosition(pointers->position());
  SafepointGenerator safepoint_generator(this, pointers, Safepoint::kLazyDeopt);
  // The number of arguments is stored in argc (receiver) which is x0, as
  // expected by InvokeFunction.
  ParameterCount actual(argc);
  __ InvokeFunction(function, actual, CALL_FUNCTION,
                    safepoint_generator, CALL_AS_METHOD);
  __ Ldr(cp, MemOperand(fp, StandardFrameConstants::kContextOffset));
}


void LCodeGen::DoArgumentsElements(LArgumentsElements* instr) {
  Register result = ToRegister(instr->result());

  if (instr->hydrogen()->from_inlined()) {
    // When we are inside an inlined function, the arguments are the last things
    // that have been pushed on the stack. Therefore the arguments array can be
    // accessed directly from jssp.
    // However in the normal case, it is accessed via fp but there are two words
    // on the stack between fp and the arguments (the saved lr and fp) and the
    // LAccessArgumentsAt implementation take that into account.
    // In the inlined case we need to subtract the size of 2 words to jssp to
    // get a pointer which will work well with LAccessArgumentsAt.
    ASSERT(masm()->StackPointer().Is(jssp));
    __ Sub(result, jssp, 2 * kPointerSize);
  } else {
    ASSERT(instr->temp() != NULL);
    Register previous_fp = ToRegister(instr->temp());

    __ Ldr(previous_fp,
           MemOperand(fp, StandardFrameConstants::kCallerFPOffset));
    __ Ldr(result,
           MemOperand(previous_fp, StandardFrameConstants::kContextOffset));
    __ Cmp(result, Operand(Smi::FromInt(StackFrame::ARGUMENTS_ADAPTOR)));
    __ Csel(result, fp, previous_fp, ne);
  }
}


void LCodeGen::DoArgumentsLength(LArgumentsLength* instr) {
  Register elements = ToRegister(instr->elements());
  Register result = ToRegister(instr->result());
  Label done;

  // If no arguments adaptor frame the number of arguments is fixed.
  __ Cmp(fp, elements);
  __ Mov(result, scope()->num_parameters());
  __ B(eq, &done);

  // Arguments adaptor frame present. Get argument length from there.
  __ Ldr(result, MemOperand(fp, StandardFrameConstants::kCallerFPOffset));
  __ Ldrsw(result,
           UntagSmiMemOperand(result,
                              ArgumentsAdaptorFrameConstants::kLengthOffset));

  // Argument length is in result register.
  __ Bind(&done);
}


void LCodeGen::DoArithmeticD(LArithmeticD* instr) {
  DoubleRegister left = ToDoubleRegister(instr->left());
  DoubleRegister right = ToDoubleRegister(instr->right());
  DoubleRegister result = ToDoubleRegister(instr->result());

  switch (instr->op()) {
    case Token::ADD: __ Fadd(result, left, right); break;
    case Token::SUB: __ Fsub(result, left, right); break;
    case Token::MUL: __ Fmul(result, left, right); break;
    case Token::DIV: __ Fdiv(result, left, right); break;
    case Token::MOD: {
      // The ECMA-262 remainder operator is the remainder from a truncating
      // (round-towards-zero) division. Note that this differs from IEEE-754.
      //
      // TODO(jbramley): See if it's possible to do this inline, rather than by
      // calling a helper function. With frintz (to produce the intermediate
      // quotient) and fmsub (to calculate the remainder without loss of
      // precision), it should be possible. However, we would need support for
      // fdiv in round-towards-zero mode, and the A64 simulator doesn't support
      // that yet.
      ASSERT(left.Is(d0));
      ASSERT(right.Is(d1));
      __ CallCFunction(
          ExternalReference::double_fp_operation(Token::MOD, isolate()),
          0, 2);
      ASSERT(result.Is(d0));
      break;
    }
    default:
      UNREACHABLE();
      break;
  }
}


void LCodeGen::DoArithmeticT(LArithmeticT* instr) {
  ASSERT(ToRegister(instr->left()).is(x1));
  ASSERT(ToRegister(instr->right()).is(x0));
  ASSERT(ToRegister(instr->result()).is(x0));

  BinaryOpStub stub(instr->op(), NO_OVERWRITE);
  CallCode(stub.GetCode(isolate()), RelocInfo::CODE_TARGET, instr);
}


void LCodeGen::DoBitI(LBitI* instr) {
  LOperand* left_op = instr->left();
  LOperand* right_op = instr->right();
  Register left = ToRegister(left_op);
  Register result = ToRegister(instr->result());

  ASSERT(right_op->IsRegister() || right_op->IsConstantOperand());
  Operand right = ToOperand(right_op);

  switch (instr->op()) {
    case Token::BIT_AND: __ And(result, left, right); break;
    case Token::BIT_OR:  __ Orr(result, left, right); break;
    case Token::BIT_XOR: __ Eor(result, left, right); break;
    default:
      UNREACHABLE();
      break;
  }
}


void LCodeGen::DoBitNotI(LBitNotI* instr) {
  Register input = ToRegister(instr->value()).W();
  Register result = ToRegister(instr->result()).W();
  __ Mvn(result, input);
}


void LCodeGen::DoBoundsCheck(LBoundsCheck *instr) {
  if (instr->hydrogen()->skip_check()) return;

  Register length = ToRegister(instr->length());

  if (instr->index()->IsConstantOperand()) {
    int constant_index =
        ToInteger32(LConstantOperand::cast(instr->index()));

    if (instr->hydrogen()->length()->representation().IsSmi()) {
      __ Cmp(length, Operand(Smi::FromInt(constant_index)));
    } else {
      __ Cmp(length, Operand(constant_index));
    }
  } else {
    __ Cmp(length, ToRegister(instr->index()));
  }
  DeoptimizeIf(ls, instr->environment());
}


void LCodeGen::DoBranch(LBranch* instr) {
  Representation r = instr->hydrogen()->value()->representation();
  Label* true_label = instr->TrueLabel(chunk_);
  Label* false_label = instr->FalseLabel(chunk_);

  if (r.IsInteger32()) {
    ASSERT(!info()->IsStub());
    EmitCompareAndBranch(instr, ne, ToRegister32(instr->value()), 0);
  } else if (r.IsSmi()) {
    ASSERT(!info()->IsStub());
    STATIC_ASSERT(kSmiTag == 0);
    EmitCompareAndBranch(instr, ne, ToRegister(instr->value()), 0);
  } else if (r.IsDouble()) {
    DoubleRegister value = ToDoubleRegister(instr->value());
    // Test the double value. Zero and NaN are false.
    EmitBranchIfNonZeroNumber(instr, value, double_scratch());
  } else {
    ASSERT(r.IsTagged());
    Register value = ToRegister(instr->value());
    HType type = instr->hydrogen()->value()->type();

    if (type.IsBoolean()) {
      ASSERT(!info()->IsStub());
      __ CompareRoot(value, Heap::kTrueValueRootIndex);
      EmitBranch(instr, eq);
    } else if (type.IsSmi()) {
      ASSERT(!info()->IsStub());
      EmitCompareAndBranch(instr, ne, value, Operand(Smi::FromInt(0)));
    } else if (type.IsJSArray()) {
      ASSERT(!info()->IsStub());
      EmitBranch(instr, al);
    } else if (type.IsHeapNumber()) {
      ASSERT(!info()->IsStub());
      __ Ldr(double_scratch(), FieldMemOperand(value,
                                               HeapNumber::kValueOffset));
      // Test the double value. Zero and NaN are false.
      EmitBranchIfNonZeroNumber(instr, double_scratch(), double_scratch());
    } else if (type.IsString()) {
      ASSERT(!info()->IsStub());
      Register temp = ToRegister(instr->temp1());
      __ Ldr(temp, FieldMemOperand(value, String::kLengthOffset));
      EmitCompareAndBranch(instr, ne, temp, 0);
    } else {
      ToBooleanStub::Types expected = instr->hydrogen()->expected_input_types();
      // Avoid deopts in the case where we've never executed this path before.
      if (expected.IsEmpty()) expected = ToBooleanStub::Types::Generic();

      if (expected.Contains(ToBooleanStub::UNDEFINED)) {
        // undefined -> false.
        __ JumpIfRoot(
            value, Heap::kUndefinedValueRootIndex, false_label);
      }

      if (expected.Contains(ToBooleanStub::BOOLEAN)) {
        // Boolean -> its value.
        __ JumpIfRoot(
            value, Heap::kTrueValueRootIndex, true_label);
        __ JumpIfRoot(
            value, Heap::kFalseValueRootIndex, false_label);
      }

      if (expected.Contains(ToBooleanStub::NULL_TYPE)) {
        // 'null' -> false.
        __ JumpIfRoot(
            value, Heap::kNullValueRootIndex, false_label);
      }

      if (expected.Contains(ToBooleanStub::SMI)) {
        // Smis: 0 -> false, all other -> true.
        ASSERT(Smi::FromInt(0) == 0);
        __ Cbz(value, false_label);
        __ JumpIfSmi(value, true_label);
      } else if (expected.NeedsMap()) {
        // If we need a map later and have a smi, deopt.
        DeoptimizeIfSmi(value, instr->environment());
      }

      Register map = NoReg;
      Register scratch = NoReg;

      if (expected.NeedsMap()) {
        ASSERT((instr->temp1() != NULL) && (instr->temp2() != NULL));
        map = ToRegister(instr->temp1());
        scratch = ToRegister(instr->temp2());

        __ Ldr(map, FieldMemOperand(value, HeapObject::kMapOffset));

        if (expected.CanBeUndetectable()) {
          // Undetectable -> false.
          __ Ldrb(scratch, FieldMemOperand(map, Map::kBitFieldOffset));
          __ TestAndBranchIfAnySet(
              scratch, 1 << Map::kIsUndetectable, false_label);
        }
      }

      if (expected.Contains(ToBooleanStub::SPEC_OBJECT)) {
        // spec object -> true.
        __ CompareInstanceType(map, scratch, FIRST_SPEC_OBJECT_TYPE);
        __ B(ge, true_label);
      }

      if (expected.Contains(ToBooleanStub::STRING)) {
        // String value -> false iff empty.
        Label not_string;
        __ CompareInstanceType(map, scratch, FIRST_NONSTRING_TYPE);
        __ B(ge, &not_string);
        __ Ldr(scratch, FieldMemOperand(value, String::kLengthOffset));
        __ Cbz(scratch, false_label);
        __ B(true_label);
        __ Bind(&not_string);
      }

      if (expected.Contains(ToBooleanStub::SYMBOL)) {
        // Symbol value -> true.
        __ CompareInstanceType(map, scratch, SYMBOL_TYPE);
        __ B(eq, true_label);
      }

      if (expected.Contains(ToBooleanStub::HEAP_NUMBER)) {
        Label not_heap_number;
        __ JumpIfNotRoot(map, Heap::kHeapNumberMapRootIndex, &not_heap_number);

        __ Ldr(double_scratch(),
               FieldMemOperand(value, HeapNumber::kValueOffset));
        __ Fcmp(double_scratch(), 0.0);
        // If we got a NaN (overflow bit is set), jump to the false branch.
        __ B(vs, false_label);
        __ B(eq, false_label);
        __ B(true_label);
        __ Bind(&not_heap_number);
      }

      if (!expected.IsGeneric()) {
        // We've seen something for the first time -> deopt.
        // This can only happen if we are not generic already.
        Deoptimize(instr->environment());
      }
    }
  }
}


void LCodeGen::CallKnownFunction(Handle<JSFunction> function,
                                 int formal_parameter_count,
                                 int arity,
                                 LInstruction* instr,
                                 CallKind call_kind,
                                 Register function_reg) {
  bool dont_adapt_arguments =
      formal_parameter_count == SharedFunctionInfo::kDontAdaptArgumentsSentinel;
  bool can_invoke_directly =
      dont_adapt_arguments || formal_parameter_count == arity;

  // The function interface relies on the following register assignments.
  ASSERT(function_reg.Is(x1) || function_reg.IsNone());
  Register arity_reg = x0;
  Register call_kind_reg = x5;

  LPointerMap* pointers = instr->pointer_map();
  RecordPosition(pointers->position());

  // If necessary, load the function object.
  if (function_reg.IsNone()) {
    function_reg = x1;
    __ LoadHeapObject(function_reg, function);
  }

  if (FLAG_debug_code) {
    Label is_not_smi;
    // Try to confirm that function_reg (x1) is a tagged pointer.
    __ JumpIfNotSmi(function_reg, &is_not_smi);
    __ Abort("In CallKnownFunction, a function object is expected in x1.");
    __ Bind(&is_not_smi);
  }

  if (can_invoke_directly) {
    // Change context.
    __ Ldr(cp, FieldMemOperand(function_reg, JSFunction::kContextOffset));

    // Set the arguments count if adaption is not needed. Assumes that x0 is
    // available to write to at this point.
    if (dont_adapt_arguments) {
      __ Mov(arity_reg, arity);
    }

    // Invoke function.
    __ SetCallKind(call_kind_reg, call_kind);
    __ Ldr(x10, FieldMemOperand(function_reg, JSFunction::kCodeEntryOffset));
    __ Call(x10);

    // Set up deoptimization.
    RecordSafepointWithLazyDeopt(instr, RECORD_SIMPLE_SAFEPOINT);
  } else {
    SafepointGenerator generator(this, pointers, Safepoint::kLazyDeopt);
    ParameterCount count(arity);
    ParameterCount expected(formal_parameter_count);
    __ InvokeFunction(function, expected, count, CALL_FUNCTION, generator,
                      call_kind, function_reg);
  }

  // Restore context.
  __ Ldr(cp, MemOperand(fp, StandardFrameConstants::kContextOffset));
}


void LCodeGen::DoCallConstantFunction(LCallConstantFunction* instr) {
  ASSERT(ToRegister(instr->result()).is(x0));
  CallKnownFunction(instr->hydrogen()->function(),
                    instr->hydrogen()->formal_parameter_count(),
                    instr->arity(), instr, CALL_AS_METHOD);
}


void LCodeGen::DoCallKnownGlobal(LCallKnownGlobal* instr) {
  ASSERT(ToRegister(instr->result()).is(x0));
  CallKnownFunction(instr->hydrogen()->target(),
                    instr->hydrogen()->formal_parameter_count(),
                    instr->arity(), instr, CALL_AS_FUNCTION);
}


void LCodeGen::DoCallGlobal(LCallGlobal* instr) {
  ASSERT(ToRegister(instr->result()).is(x0));

  int arity = instr->arity();
  RelocInfo::Mode mode = RelocInfo::CODE_TARGET_CONTEXT;
  Handle<Code> ic =
      isolate()->stub_cache()->ComputeCallInitialize(arity, mode);
  __ Mov(x2, Operand(instr->name()));
  CallCode(ic, mode, instr);
  __ Ldr(cp, MemOperand(fp, StandardFrameConstants::kContextOffset));
}


void LCodeGen::DoCallKeyed(LCallKeyed* instr) {
  ASSERT(ToRegister(instr->key()).Is(x2));
  ASSERT(ToRegister(instr->result()).Is(x0));

  int arity = instr->arity();
  Handle<Code> ic =
      isolate()->stub_cache()->ComputeKeyedCallInitialize(arity);
  CallCode(ic, RelocInfo::CODE_TARGET, instr);
  __ Ldr(cp, MemOperand(fp, StandardFrameConstants::kContextOffset));
}


void LCodeGen::DoCallNamed(LCallNamed* instr) {
  ASSERT(ToRegister(instr->result()).is(x0));

  int arity = instr->arity();
  RelocInfo::Mode mode = RelocInfo::CODE_TARGET;
  Handle<Code> ic =
      isolate()->stub_cache()->ComputeCallInitialize(arity, mode);

  // IC needs a pointer to the name of the function to be called in x2.
  __ Mov(x2, Operand(instr->name()));
  CallCode(ic, mode, instr);
  // Restore context register.
  __ Ldr(cp, MemOperand(fp, StandardFrameConstants::kContextOffset));
}


void LCodeGen::DoCallRuntime(LCallRuntime* instr) {
  CallRuntime(instr->function(), instr->arity(), instr);
}


void LCodeGen::DoCallStub(LCallStub* instr) {
  ASSERT(ToRegister(instr->result()).is(x0));
  switch (instr->hydrogen()->major_key()) {
    case CodeStub::RegExpConstructResult: {
      RegExpConstructResultStub stub;
      CallCode(stub.GetCode(isolate()), RelocInfo::CODE_TARGET, instr);
      break;
    }
    case CodeStub::RegExpExec: {
      RegExpExecStub stub;
      CallCode(stub.GetCode(isolate()), RelocInfo::CODE_TARGET, instr);
      break;
    }
    case CodeStub::SubString: {
      SubStringStub stub;
      CallCode(stub.GetCode(isolate()), RelocInfo::CODE_TARGET, instr);
      break;
    }
    case CodeStub::NumberToString: {
      NumberToStringStub stub;
      CallCode(stub.GetCode(isolate()), RelocInfo::CODE_TARGET, instr);
      break;
    }
    case CodeStub::StringAdd: {
      // TODO(jbramley): In bleeding_edge, there is no StringAdd case here.
      StringAddStub stub(NO_STRING_ADD_FLAGS);
      CallCode(stub.GetCode(isolate()), RelocInfo::CODE_TARGET, instr);
      break;
    }
    case CodeStub::StringCompare: {
      StringCompareStub stub;
      CallCode(stub.GetCode(isolate()), RelocInfo::CODE_TARGET, instr);
      break;
    }
    case CodeStub::TranscendentalCache: {
      __ Peek(x0, 0);
      TranscendentalCacheStub stub(instr->transcendental_type(),
                                   TranscendentalCacheStub::TAGGED);
      CallCode(stub.GetCode(isolate()), RelocInfo::CODE_TARGET, instr);
      break;
    }
    default:
      UNREACHABLE();
  }
}


void LCodeGen::DoUnknownOSRValue(LUnknownOSRValue* instr) {
  // Record the address of the first unknown OSR value as the place to enter.
  if (osr_pc_offset_ == -1) osr_pc_offset_ = masm()->pc_offset();
}


void LCodeGen::DoCheckMaps(LCheckMaps* instr) {
  Register object = ToRegister(instr->value());
  Register map_reg = ToRegister(instr->temp());

  Label success;
  SmallMapList* map_set = instr->hydrogen()->map_set();
  __ Ldr(map_reg, FieldMemOperand(object, HeapObject::kMapOffset));
  for (int i = 0; i < map_set->length(); i++) {
    Handle<Map> map = map_set->at(i);
    __ CompareMap(map_reg, map, &success);
    __ B(eq, &success);
  }

  // If we didn't match a map, deoptimize.
  Deoptimize(instr->environment());

  __ Bind(&success);
}


void LCodeGen::DoCheckNonSmi(LCheckNonSmi* instr) {
  if (!instr->hydrogen()->value()->IsHeapObject()) {
    // TODO(all): Depending of how we chose to implement the deopt, if we could
    // guarantee that we have a deopt handler reachable by a tbz instruction,
    // we could use tbz here and produce less code to support this instruction.
    DeoptimizeIfSmi(ToRegister(instr->value()), instr->environment());
  }
}


void LCodeGen::DoCheckPrototypeMaps(LCheckPrototypeMaps* instr) {
  ZoneList<Handle<JSObject> >* prototypes = instr->prototypes();
  ZoneList<Handle<Map> >* maps = instr->maps();
  ASSERT(prototypes->length() == maps->length());

  if (!instr->hydrogen()->CanOmitPrototypeChecks()) {
    // TODO(jbramley): The temp registers are only needed in this case.
    Label success, deopt;
    Register temp1 = ToRegister(instr->temp1());
    Register temp2 = ToRegister(instr->temp2());
    for (int i = 0; i < prototypes->length(); i++) {
      __ LoadHeapObject(temp1, prototypes->at(i));
      __ Ldr(temp2, FieldMemOperand(temp1, HeapObject::kMapOffset));
      __ CompareMap(temp2, maps->at(i), &success);
      __ B(eq, &success);
    }
    // If we didn't match a map, deoptimize.
    Deoptimize(instr->environment());
    __ Bind(&success);
  }
}


void LCodeGen::DoCheckSmi(LCheckSmi* instr) {
  Register value = ToRegister(instr->value());
  ASSERT(ToRegister(instr->result()).Is(value));
  // TODO(all): See DoCheckNonSmi for comments on use of tbz.
  DeoptimizeIfNotSmi(value, instr->environment());
}


void LCodeGen::DoCheckInstanceType(LCheckInstanceType* instr) {
  Register input = ToRegister(instr->value());
  Register scratch = ToRegister(instr->temp());

  __ Ldr(scratch, FieldMemOperand(input, HeapObject::kMapOffset));
  __ Ldrb(scratch, FieldMemOperand(scratch, Map::kInstanceTypeOffset));

  if (instr->hydrogen()->is_interval_check()) {
    InstanceType first, last;
    instr->hydrogen()->GetCheckInterval(&first, &last);

    __ Cmp(scratch, first);
    if (first == last) {
      // If there is only one type in the interval check for equality.
      DeoptimizeIf(ne, instr->environment());
    } else if (last == LAST_TYPE) {
      // We don't need to compare with the higher bound of the interval.
      DeoptimizeIf(lo, instr->environment());
    } else {
      // If we are below the lower bound, set the C flag and clear the Z flag
      // to force a deopt.
      __ Ccmp(scratch, last, CFlag, hs);
      DeoptimizeIf(hi, instr->environment());
    }
  } else {
    uint8_t mask;
    uint8_t tag;
    instr->hydrogen()->GetCheckMaskAndTag(&mask, &tag);

    if (IsPowerOf2(mask)) {
      ASSERT((tag == 0) || (tag == mask));
      // TODO(all): We might be able to use tbz/tbnz if we can guarantee that
      // the deopt handler is reachable by a tbz instruction.
      __ Tst(scratch, mask);
      DeoptimizeIf(tag == 0 ? ne : eq, instr->environment());
    } else {
      if (tag == 0) {
        __ Tst(scratch, mask);
      } else {
        __ And(scratch, scratch, mask);
        __ Cmp(scratch, tag);
      }
      DeoptimizeIf(ne, instr->environment());
    }
  }
}


void LCodeGen::DoClampDToUint8(LClampDToUint8* instr) {
  DoubleRegister input = ToDoubleRegister(instr->unclamped());
  Register result = ToRegister(instr->result());
  __ ClampDoubleToUint8(result, input, double_scratch());
}


void LCodeGen::DoClampIToUint8(LClampIToUint8* instr) {
  Register input = ToRegister32(instr->unclamped());
  Register result = ToRegister32(instr->result());
  __ ClampInt32ToUint8(result, input);
}


void LCodeGen::DoClampTToUint8(LClampTToUint8* instr) {
  Register input = ToRegister(instr->unclamped());
  Register result = ToRegister(instr->result());
  Register scratch = ToRegister(instr->temp1());
  Label done;

  // Both smi and heap number cases are handled.
  Label is_not_smi;
  __ JumpIfNotSmi(input, &is_not_smi);
  __ SmiUntag(result, input);
  __ ClampInt32ToUint8(result);
  __ B(&done);

  __ Bind(&is_not_smi);

  // Check for heap number.
  Label is_heap_number;
  __ Ldr(scratch, FieldMemOperand(input, HeapObject::kMapOffset));
  __ JumpIfRoot(scratch, Heap::kHeapNumberMapRootIndex, &is_heap_number);

  // Check for undefined. Undefined is coverted to zero for clamping conversion.
  DeoptimizeIfNotRoot(input, Heap::kUndefinedValueRootIndex,
                         instr->environment());
  __ Mov(result, 0);
  __ B(&done);

  // Heap number case.
  __ Bind(&is_heap_number);
  DoubleRegister dbl_scratch = double_scratch();
  DoubleRegister dbl_scratch2 = ToDoubleRegister(instr->temp2());
  __ Ldr(dbl_scratch, FieldMemOperand(input, HeapNumber::kValueOffset));
  __ ClampDoubleToUint8(result, dbl_scratch, dbl_scratch2);

  __ Bind(&done);
}


void LCodeGen::DoClassOfTestAndBranch(LClassOfTestAndBranch* instr) {
  Handle<String> class_name = instr->hydrogen()->class_name();
  Label* true_label = instr->TrueLabel(chunk_);
  Label* false_label = instr->FalseLabel(chunk_);
  Register input = ToRegister(instr->value());
  Register scratch1 = ToRegister(instr->temp1());
  Register scratch2 = ToRegister(instr->temp2());

  __ JumpIfSmi(input, false_label);

  Register map = scratch2;
  if (class_name->IsUtf8EqualTo(CStrVector("Function"))) {
    // Assuming the following assertions, we can use the same compares to test
    // for both being a function type and being in the object type range.
    STATIC_ASSERT(NUM_OF_CALLABLE_SPEC_OBJECT_TYPES == 2);
    STATIC_ASSERT(FIRST_NONCALLABLE_SPEC_OBJECT_TYPE ==
                  FIRST_SPEC_OBJECT_TYPE + 1);
    STATIC_ASSERT(LAST_NONCALLABLE_SPEC_OBJECT_TYPE ==
                  LAST_SPEC_OBJECT_TYPE - 1);
    STATIC_ASSERT(LAST_SPEC_OBJECT_TYPE == LAST_TYPE);

    // We expect CompareObjectType to load the object instance type in scratch1.
    __ CompareObjectType(input, map, scratch1, FIRST_SPEC_OBJECT_TYPE);
    __ B(lt, false_label);
    __ B(eq, true_label);
    __ Cmp(scratch1, LAST_SPEC_OBJECT_TYPE);
    __ B(eq, true_label);
  } else {
    __ IsObjectJSObjectType(input, map, scratch1, false_label);
  }

  // Now we are in the FIRST-LAST_NONCALLABLE_SPEC_OBJECT_TYPE range.
  // Check if the constructor in the map is a function.
  __ Ldr(scratch1, FieldMemOperand(map, Map::kConstructorOffset));

  // Objects with a non-function constructor have class 'Object'.
  if (class_name->IsUtf8EqualTo(CStrVector("Object"))) {
    __ JumpIfNotObjectType(
        scratch1, scratch2, scratch2, JS_FUNCTION_TYPE, true_label);
  } else {
    __ JumpIfNotObjectType(
        scratch1, scratch2, scratch2, JS_FUNCTION_TYPE, false_label);
  }

  // The constructor function is in scratch1. Get its instance class name.
  __ Ldr(scratch1,
         FieldMemOperand(scratch1, JSFunction::kSharedFunctionInfoOffset));
  __ Ldr(scratch1,
         FieldMemOperand(scratch1,
                         SharedFunctionInfo::kInstanceClassNameOffset));

  // The class name we are testing against is internalized since it's a literal.
  // The name in the constructor is internalized because of the way the context
  // is booted. This routine isn't expected to work for random API-created
  // classes and it doesn't have to because you can't access it with natives
  // syntax. Since both sides are internalized it is sufficient to use an
  // identity comparison.
  EmitCompareAndBranch(instr, eq, scratch1, Operand(class_name));
}


void LCodeGen::DoCmpMapAndBranch(LCmpMapAndBranch* instr) {
  Register value = ToRegister(instr->value());
  Register map = ToRegister(instr->temp());

  __ Ldr(map, FieldMemOperand(value, HeapObject::kMapOffset));
  EmitCompareAndBranch(instr, eq, map, Operand(instr->map()));
}


void LCodeGen::DoCmpIDAndBranch(LCmpIDAndBranch* instr) {
  LOperand* left = instr->left();
  LOperand* right = instr->right();
  Condition cond = TokenToCondition(instr->op(), false);

  if (left->IsConstantOperand() && right->IsConstantOperand()) {
    // We can statically evaluate the comparison.
    double left_val = ToDouble(LConstantOperand::cast(left));
    double right_val = ToDouble(LConstantOperand::cast(right));
    int next_block = EvalComparison(instr->op(), left_val, right_val) ?
        instr->TrueDestination(chunk_) : instr->FalseDestination(chunk_);
    EmitGoto(next_block);
  } else {
    if (instr->is_double()) {
      if (right->IsConstantOperand()) {
        __ Fcmp(ToDoubleRegister(left),
                ToDouble(LConstantOperand::cast(right)));
      } else if (left->IsConstantOperand()) {
        // Transpose the operands and reverse the condition.
        __ Fcmp(ToDoubleRegister(right),
                ToDouble(LConstantOperand::cast(left)));
        cond = ReverseConditionForCmp(cond);
      } else {
        __ Fcmp(ToDoubleRegister(left), ToDoubleRegister(right));
      }

      // If a NaN is involved, i.e. the result is unordered (V set),
      // jump to false block label.
      __ B(vs, instr->FalseLabel(chunk_));
      EmitBranch(instr, cond);
    } else {
      if (instr->hydrogen_value()->representation().IsInteger32()) {
        if (right->IsConstantOperand()) {
          EmitCompareAndBranch(instr,
                               cond,
                               ToRegister32(left),
                               ToOperand32(right));
        } else {
          // Transpose the operands and reverse the condition.
          EmitCompareAndBranch(instr,
                               ReverseConditionForCmp(cond),
                               ToRegister32(right),
                               ToOperand32(left));
        }
      } else {
        ASSERT(instr->hydrogen_value()->representation().IsSmi());
        if (right->IsConstantOperand()) {
          int32_t value = ToInteger32(LConstantOperand::cast(right));
          EmitCompareAndBranch(instr,
                               cond,
                               ToRegister(left),
                               Operand(Smi::FromInt(value)));
        } else if (left->IsConstantOperand()) {
          // Transpose the operands and reverse the condition.
          int32_t value = ToInteger32(LConstantOperand::cast(left));
          EmitCompareAndBranch(instr,
                               ReverseConditionForCmp(cond),
                               ToRegister(right),
                               Operand(Smi::FromInt(value)));
        } else {
          EmitCompareAndBranch(instr,
                               cond,
                               ToRegister(left),
                               ToRegister(right));
        }
      }
    }
  }
}


void LCodeGen::DoCmpObjectEqAndBranch(LCmpObjectEqAndBranch* instr) {
  Register left = ToRegister(instr->left());
  Register right = ToRegister(instr->right());
  EmitCompareAndBranch(instr, eq, left, right);
}


void LCodeGen::DoCmpT(LCmpT* instr) {
  Token::Value op = instr->op();
  Condition cond = TokenToCondition(op, false);

  ASSERT(ToRegister(instr->left()).Is(x1));
  ASSERT(ToRegister(instr->right()).Is(x0));
  Handle<Code> ic = CompareIC::GetUninitialized(isolate(), op);
  CallCode(ic, RelocInfo::CODE_TARGET, instr);
  // Signal that we don't inline smi code before this stub.
  InlineSmiCheckInfo::EmitNotInlined(masm());

  // Return true or false depending on CompareIC result.
  // This instruction is marked as call. We can clobber any register.
  ASSERT(instr->IsMarkedAsCall());
  __ LoadTrueFalseRoots(x1, x2);
  __ Cmp(x0, 0);
  __ Csel(ToRegister(instr->result()), x1, x2, cond);
}


void LCodeGen::DoConstantD(LConstantD* instr) {
  ASSERT(instr->result()->IsDoubleRegister());
  DoubleRegister result = ToDoubleRegister(instr->result());
  __ Fmov(result, instr->value());
}


void LCodeGen::DoConstantI(LConstantI* instr) {
  __ Mov(ToRegister(instr->result()), instr->value());
}


void LCodeGen::DoConstantS(LConstantS* instr) {
  __ Mov(ToRegister(instr->result()), Operand(instr->value()));
}


void LCodeGen::DoConstantT(LConstantT* instr) {
  Handle<Object> value = instr->value();
  AllowDeferredHandleDereference smi_check;
  if (value->IsSmi()) {
    __ Mov(ToRegister(instr->result()), Operand(value));
  } else {
    __ LoadHeapObject(ToRegister(instr->result()),
                      Handle<HeapObject>::cast(value));
  }
}


void LCodeGen::DoContext(LContext* instr) {
  // If there is a non-return use, the context must be moved to a register.
  Register result = ToRegister(instr->result());
  // TODO(jbramley): LContext is only generated if it meets this condition, so
  // why not move cp unconditionally?
  for (HUseIterator it(instr->hydrogen()->uses()); !it.Done(); it.Advance()) {
    if (!it.value()->IsReturn()) {
      __ Mov(result, cp);
      return;
    }
  }
}


void LCodeGen::DoCheckFunction(LCheckFunction* instr) {
  Register reg = ToRegister(instr->value());
  Handle<JSFunction> target = instr->hydrogen()->target();
  AllowDeferredHandleDereference smi_check;
  if (isolate()->heap()->InNewSpace(*target)) {
    Register temp = ToRegister(instr->temp());
    Handle<Cell> cell = isolate()->factory()->NewPropertyCell(target);
    __ Mov(temp, Operand(Handle<Object>(cell)));
    __ Ldr(temp, FieldMemOperand(temp, Cell::kValueOffset));
    __ Cmp(reg, temp);
  } else {
    __ Cmp(reg, Operand(target));
  }
  DeoptimizeIf(ne, instr->environment());
}


void LCodeGen::DoLazyBailout(LLazyBailout* instr) {
  EnsureSpaceForLazyDeopt();
  ASSERT(instr->HasEnvironment());
  LEnvironment* env = instr->environment();
  RegisterEnvironmentForDeoptimization(env, Safepoint::kLazyDeopt);
  safepoints_.RecordLazyDeoptimizationIndex(env->deoptimization_index());
}


void LCodeGen::DoDateField(LDateField* instr) {
  Register object = ToRegister(instr->date());
  Register result = ToRegister(instr->result());
  Register temp1 = x10;
  Register temp2 = x11;
  Smi* index = instr->index();
  Label runtime, done, deopt, obj_ok;

  ASSERT(object.is(result) && object.Is(x0));
  ASSERT(instr->IsMarkedAsCall());

  __ JumpIfSmi(object, &deopt);
  __ CompareObjectType(object, temp1, temp1, JS_DATE_TYPE);
  __ B(eq, &obj_ok);

  __ Bind(&deopt);
  Deoptimize(instr->environment());

  __ Bind(&obj_ok);
  if (index->value() == 0) {
    __ Ldr(result, FieldMemOperand(object, JSDate::kValueOffset));
  } else {
    if (index->value() < JSDate::kFirstUncachedField) {
      ExternalReference stamp = ExternalReference::date_cache_stamp(isolate());
      __ Mov(temp1, Operand(stamp));
      __ Ldr(temp1, MemOperand(temp1));
      __ Ldr(temp2, FieldMemOperand(object, JSDate::kCacheStampOffset));
      __ Cmp(temp1, temp2);
      __ B(ne, &runtime);
      __ Ldr(result, FieldMemOperand(object, JSDate::kValueOffset +
                                             kPointerSize * index->value()));
      __ B(&done);
    }

    __ Bind(&runtime);
    __ Mov(x1, Operand(index));
    __ CallCFunction(ExternalReference::get_date_field_function(isolate()), 2);
  }

  __ Bind(&done);
}


void LCodeGen::DoDeoptimize(LDeoptimize* instr) {
  if (instr->hydrogen_value()->IsSoftDeoptimize()) {
    SoftDeoptimize(instr->environment());
  } else {
    Deoptimize(instr->environment());
  }
}


void LCodeGen::DoDivI(LDivI* instr) {
  Register dividend = ToRegister32(instr->left());
  Register result = ToRegister32(instr->result());

  bool has_power_of_2_divisor = instr->hydrogen()->HasPowerOf2Divisor();
  bool can_overflow = instr->hydrogen()->CheckFlag(HValue::kCanOverflow);
  bool bailout_on_minus_zero =
      instr->hydrogen()->CheckFlag(HValue::kBailoutOnMinusZero);
  bool can_be_div_by_zero =
      instr->hydrogen()->CheckFlag(HValue::kCanBeDivByZero);
  bool all_uses_truncating_to_int32 =
      instr->hydrogen()->CheckFlag(HInstruction::kAllUsesTruncatingToInt32);

  if (has_power_of_2_divisor) {
    ASSERT(instr->temp() == NULL);
    int32_t divisor = ToInteger32(LConstantOperand::cast(instr->right()));
    int32_t power;
    int32_t power_mask;
    Label deopt, done;

    ASSERT(divisor != 0);
    if (divisor > 0) {
      power = WhichPowerOf2(divisor);
      power_mask = divisor - 1;
    } else {
      // Check for (0 / -x) as that will produce negative zero.
      if (bailout_on_minus_zero) {
        if (all_uses_truncating_to_int32) {
         // If all uses truncate, and the dividend is zero, the truncated
         // result is zero.
         __ Mov(result, 0);
         __ Cbz(dividend, &done);
        } else {
          __ Cbz(dividend, &deopt);
        }
      }
      // Check for (kMinInt / -1).
      if ((divisor == -1) && can_overflow && !all_uses_truncating_to_int32) {
        // Check for kMinInt by subtracting one and checking for overflow.
        __ Cmp(dividend, 1);
        __ B(vs, &deopt);
      }
      power = WhichPowerOf2(-divisor);
      power_mask = -divisor - 1;
    }

    if (power_mask != 0) {
      if (all_uses_truncating_to_int32) {
        __ Cmp(dividend, 0);
        __ Cneg(result, dividend, lt);
        __ Asr(result, result, power);
        if (divisor > 0) __ Cneg(result, result, lt);
        if (divisor < 0) __ Cneg(result, result, gt);
        return;  // Don't fall through to negation below.
      } else {
        // Deoptimize if remainder is not 0. If the least-significant
        // power bits aren't 0, it's not a multiple of 2^power, and
        // therefore, there will be a remainder.
        __ TestAndBranchIfAnySet(dividend, power_mask, &deopt);
        __ Asr(result, dividend, power);
        if (divisor < 0) __ Neg(result, result);
      }
    } else {
      ASSERT((divisor == 1) || (divisor == -1));
      if (divisor < 0) {
        __ Neg(result, dividend);
      } else {
        __ Mov(result, dividend);
      }
    }
    __ B(&done);
    __ Bind(&deopt);
    Deoptimize(instr->environment());
    __ Bind(&done);
  } else {
    Register divisor = ToRegister32(instr->right());

    // Issue the division first, and then check for any deopt cases whilst the
    // result is computed.
    __ Sdiv(result, dividend, divisor);

    if (!all_uses_truncating_to_int32) {
      Label deopt;
      // Check for x / 0.
      if (can_be_div_by_zero) {
        __ Cbz(divisor, &deopt);
      }

      // Check for (0 / -x) as that will produce negative zero.
      if (bailout_on_minus_zero) {
        __ Cmp(divisor, 0);

        // If the divisor < 0 (mi), compare the dividend, and deopt if it is
        // zero, ie. zero dividend with negative divisor deopts.
        // If the divisor >= 0 (pl, the opposite of mi) set the flags to
        // condition ne, so we don't deopt, ie. positive divisor doesn't deopt.
        __ Ccmp(dividend, 0, NoFlag, mi);
        __ B(eq, &deopt);
      }

      // Check for (kMinInt / -1).
      if (can_overflow) {
        // Test dividend for kMinInt by subtracting one (cmp) and checking for
        // overflow.
        __ Cmp(dividend, 1);
        // If overflow is set, ie. dividend = kMinInt, compare the divisor with
        // -1. If overflow is clear, set the flags for condition ne, as the
        // dividend isn't -1, and thus we shouldn't deopt.
        __ Ccmp(divisor, -1, NoFlag, vs);
        __ B(eq, &deopt);
      }

      // Compute remainder and deopt if it's not zero.
      Register remainder = ToRegister32(instr->temp());
      __ Msub(remainder, result, divisor, dividend);
      __ Cbnz(remainder, &deopt);

      Label div_ok;
      __ B(&div_ok);
      __ Bind(&deopt);
      Deoptimize(instr->environment());
      __ Bind(&div_ok);
    } else {
      ASSERT(instr->temp() == NULL);
    }
  }
}


void LCodeGen::DoDoubleToI(LDoubleToI* instr) {
  DoubleRegister input = ToDoubleRegister(instr->value());

  if (instr->truncating()) {
    Register result = ToRegister(instr->result());
    Register scratch1 = ToRegister(instr->temp1());
    Register scratch2 = ToRegister(instr->temp2());
    __ ECMA262ToInt32(result, input, scratch1, scratch2);
  } else {
    Register result = ToRegister32(instr->result());
    ASSERT((instr->temp1() == NULL) && (instr->temp2() == NULL));
    Label done, deopt;

    if (instr->hydrogen()->CheckFlag(HValue::kBailoutOnMinusZero)) {
      // Check for an input of -0.0, using the result register as a scratch.
      __ Fmov(result, input);
      __ Cmp(result, 1);
      __ B(&deopt, vs);
    }

    __ TryConvertDoubleToInt32(result, input, double_scratch(), &done);
    __ Bind(&deopt);
    Deoptimize(instr->environment());
    __ Bind(&done);
  }
}


// TODO(jbramley): This is almost the same as DoDoubleToI. Can we merge them?
void LCodeGen::DoDoubleToSmi(LDoubleToSmi* instr) {
  DoubleRegister input = ToDoubleRegister(instr->value());

  if (instr->truncating()) {
    Register result = ToRegister(instr->result());
    Register scratch1 = ToRegister(instr->temp1());
    Register scratch2 = ToRegister(instr->temp2());
    __ ECMA262ToInt32(result, input, scratch1, scratch2);
  } else {
    Register result = ToRegister32(instr->result());
    ASSERT((instr->temp1() == NULL) && (instr->temp2() == NULL));
    Label done, deopt;

    if (instr->hydrogen()->CheckFlag(HValue::kBailoutOnMinusZero)) {
      // Check for an input of -0.0, using the result register as a scratch.
      __ Fmov(result, input);
      __ Cmp(result, 1);
      __ B(&deopt, vs);
    }

    __ TryConvertDoubleToInt32(result, input, double_scratch(), &done);
    __ Bind(&deopt);
    Deoptimize(instr->environment());
    __ Bind(&done);
  }
  __ SmiTag(ToRegister(instr->result()));
}


void LCodeGen::DoDrop(LDrop* instr) {
  TODO_UNIMPLEMENTED("DoDrop is untested.");
  __ Drop(instr->count());
}


void LCodeGen::DoDummyUse(LDummyUse* instr) {
  // Nothing to see here, move on!
}


void LCodeGen::DoElementsKind(LElementsKind* instr) {
  Register result = ToRegister(instr->result());
  Register input = ToRegister(instr->value());

  // Load map into result.
  __ Ldr(result, FieldMemOperand(input, HeapObject::kMapOffset));

  // Load the map's "bit field 2" into result.
  ASSERT((Map::kElementsKindBitCount + Map::kElementsKindShift) <= kByteSize);
  __ Ldrb(result.W(), FieldMemOperand(result, Map::kBitField2Offset));

  // Retrieve elements_kind from bit field 2.
  __ Ubfx(result.W(), result.W(), Map::kElementsKindShift,
          Map::kElementsKindBitCount);
}


void LCodeGen::DoFixedArrayBaseLength(LFixedArrayBaseLength* instr) {
  Register result = ToRegister(instr->result());
  Register array = ToRegister(instr->value());
  __ Ldr(result, FieldMemOperand(array, FixedArrayBase::kLengthOffset));
}


void LCodeGen::DoFunctionLiteral(LFunctionLiteral* instr) {
  // FunctionLiteral instruction is marked as call, we can trash any register.
  ASSERT(instr->IsMarkedAsCall());

  // Use the fast case closure allocation code that allocates in new
  // space for nested functions that don't need literals cloning.
  bool pretenure = instr->hydrogen()->pretenure();
  if (!pretenure && instr->hydrogen()->has_no_literals()) {
    FastNewClosureStub stub(instr->hydrogen()->language_mode(),
                            instr->hydrogen()->is_generator());
    __ Mov(x1, Operand(instr->hydrogen()->shared_info()));
    __ Push(x1);
    CallCode(stub.GetCode(isolate()), RelocInfo::CODE_TARGET, instr);
  } else {
    __ Mov(x2, Operand(instr->hydrogen()->shared_info()));
    __ Mov(x1, Operand(pretenure ? factory()->true_value()
                                 : factory()->false_value()));
    __ Push(cp, x2, x1);
    CallRuntime(Runtime::kNewClosure, 3, instr);
  }
}


void LCodeGen::DoForInCacheArray(LForInCacheArray* instr) {
  Register map = ToRegister(instr->map());
  Register result = ToRegister(instr->result());
  Label load_cache, done;

  __ EnumLengthUntagged(result, map);
  __ Cbnz(result, &load_cache);

  __ Mov(result, Operand(isolate()->factory()->empty_fixed_array()));
  __ B(&done);

  __ Bind(&load_cache);
  __ LoadInstanceDescriptors(map, result);
  __ Ldr(result, FieldMemOperand(result, DescriptorArray::kEnumCacheOffset));
  __ Ldr(result, FieldMemOperand(result, FixedArray::SizeFor(instr->idx())));
  DeoptimizeIfZero(result, instr->environment());

  __ Bind(&done);
}


void LCodeGen::DoForInPrepareMap(LForInPrepareMap* instr) {
  Register object = ToRegister(instr->object());
  Register null_value = x5;

  ASSERT(instr->IsMarkedAsCall());
  ASSERT(object.Is(x0));

  Label deopt;

  __ JumpIfRoot(object, Heap::kUndefinedValueRootIndex, &deopt);

  __ LoadRoot(null_value, Heap::kNullValueRootIndex);
  __ Cmp(object, null_value);
  __ B(eq, &deopt);

  __ JumpIfSmi(object, &deopt);

  STATIC_ASSERT(FIRST_JS_PROXY_TYPE == FIRST_SPEC_OBJECT_TYPE);
  __ CompareObjectType(object, x1, x1, LAST_JS_PROXY_TYPE);
  __ B(le, &deopt);

  Label use_cache, call_runtime;
  __ CheckEnumCache(object, null_value, x1, x2, x3, x4, &call_runtime);

  __ Ldr(object, FieldMemOperand(object, HeapObject::kMapOffset));
  __ B(&use_cache);

  __ Bind(&deopt);
  Deoptimize(instr->environment());

  // Get the set of properties to enumerate.
  __ Bind(&call_runtime);
  __ Push(object);
  CallRuntime(Runtime::kGetPropertyNamesFast, 1, instr);

  __ Ldr(x1, FieldMemOperand(object, HeapObject::kMapOffset));
  __ JumpIfNotRoot(x1, Heap::kMetaMapRootIndex, &deopt);

  __ Bind(&use_cache);
}


void LCodeGen::DoGetCachedArrayIndex(LGetCachedArrayIndex* instr) {
  Register input = ToRegister(instr->value());
  Register result = ToRegister(instr->result());

  __ AssertString(input);

  // Assert that we can use a W register load to get the hash.
  ASSERT((String::kHashShift + String::kArrayIndexValueBits) < kWRegSize);
  __ Ldr(result.W(), FieldMemOperand(input, String::kHashFieldOffset));
  __ IndexFromHash(result, result);
}


void LCodeGen::DoGlobalObject(LGlobalObject* instr) {
  Register result = ToRegister(instr->result());
  __ Ldr(result, GlobalObjectMemOperand());
}


void LCodeGen::DoGlobalReceiver(LGlobalReceiver* instr) {
  Register global = ToRegister(instr->global_object());
  Register result = ToRegister(instr->result());
  __ Ldr(result, FieldMemOperand(global, GlobalObject::kGlobalReceiverOffset));
}


int LCodeGen::GetNextEmittedBlock() const {
  for (int i = current_block_ + 1; i < graph()->blocks()->length(); ++i) {
    if (!chunk_->GetLabel(i)->HasReplacement()) return i;
  }
  return -1;
}


void LCodeGen::EmitGoto(int block) {
  // Do not emit jump if we are emitting a goto to the next block.
  if (!IsNextEmittedBlock(block)) {
    __ B(chunk_->GetAssemblyLabel(LookupDestination(block)));
  }
}


void LCodeGen::DoGoto(LGoto* instr) {
  EmitGoto(instr->block_id());
}


void LCodeGen::DoHasCachedArrayIndexAndBranch(
    LHasCachedArrayIndexAndBranch* instr) {
  Register input = ToRegister(instr->value());
  Register temp = ToRegister32(instr->temp());

  // Assert that the cache status bits fit in a W register.
  ASSERT(is_uint32(String::kContainsCachedArrayIndexMask));
  __ Ldr(temp, FieldMemOperand(input, String::kHashFieldOffset));
  __ Tst(temp, String::kContainsCachedArrayIndexMask);
  EmitBranch(instr, eq);
}


// HHasInstanceTypeAndBranch instruction is built with an interval of type
// to test but is only used in very restricted ways. The only possible kinds
// of intervals are:
//  - [ FIRST_TYPE, instr->to() ]
//  - [ instr->form(), LAST_TYPE ]
//  - instr->from() == instr->to()
//
// These kinds of intervals can be check with only one compare instruction
// providing the correct value and test condition are used.
//
// TestType() will return the value to use in the compare instruction and
// BranchCondition() will return the condition to use depending on the kind
// of interval actually specified in the instruction.
static InstanceType TestType(HHasInstanceTypeAndBranch* instr) {
  InstanceType from = instr->from();
  InstanceType to = instr->to();
  if (from == FIRST_TYPE) return to;
  ASSERT((from == to) || (to == LAST_TYPE));
  return from;
}


// See comment above TestType function for what this function does.
static Condition BranchCondition(HHasInstanceTypeAndBranch* instr) {
  InstanceType from = instr->from();
  InstanceType to = instr->to();
  if (from == to) return eq;
  if (to == LAST_TYPE) return hs;
  if (from == FIRST_TYPE) return ls;
  UNREACHABLE();
  return eq;
}


void LCodeGen::DoHasInstanceTypeAndBranch(LHasInstanceTypeAndBranch* instr) {
  Register input = ToRegister(instr->value());
  Register scratch = ToRegister(instr->temp());

  if (!instr->hydrogen()->value()->IsHeapObject()) {
    __ JumpIfSmi(input, instr->FalseLabel(chunk_));
  }
  __ CompareObjectType(input, scratch, scratch, TestType(instr->hydrogen()));
  EmitBranch(instr, BranchCondition(instr->hydrogen()));
}


void LCodeGen::DoIn(LIn* instr) {
  Register obj = ToRegister(instr->object());
  Register key = ToRegister(instr->key());
  __ Push(key, obj);
  ASSERT(instr->HasPointerMap());
  LPointerMap* pointers = instr->pointer_map();
  RecordPosition(pointers->position());
  SafepointGenerator safepoint_generator(this, pointers, Safepoint::kLazyDeopt);
  __ InvokeBuiltin(Builtins::IN, CALL_FUNCTION, safepoint_generator);
}


void LCodeGen::DoInnerAllocatedObject(LInnerAllocatedObject* instr) {
  Register result = ToRegister(instr->result());
  Register base = ToRegister(instr->base_object());
  __ Add(result, base, instr->offset());
}


void LCodeGen::DoInstanceOf(LInstanceOf* instr) {
  // Assert that the arguments are in the registers expected by InstanceofStub.
  ASSERT(ToRegister(instr->left()).Is(InstanceofStub::left()));
  ASSERT(ToRegister(instr->right()).Is(InstanceofStub::right()));

  InstanceofStub stub(InstanceofStub::kArgsInRegisters);
  CallCode(stub.GetCode(isolate()), RelocInfo::CODE_TARGET, instr);

  // InstanceofStub returns a result in x0:
  //   0     => not an instance
  //   smi 1 => instance.
  __ Cmp(x0, 0);
  __ LoadTrueFalseRoots(x0, x1);
  __ Csel(x0, x0, x1, eq);
}


void LCodeGen::DoInstanceOfKnownGlobal(LInstanceOfKnownGlobal* instr) {
  class DeferredInstanceOfKnownGlobal: public LDeferredCode {
   public:
    DeferredInstanceOfKnownGlobal(LCodeGen* codegen,
                                  LInstanceOfKnownGlobal* instr)
        : LDeferredCode(codegen), instr_(instr) { }
    virtual void Generate() {
      codegen()->DoDeferredInstanceOfKnownGlobal(instr_, &map_check_);
    }
    virtual LInstruction* instr() { return instr_; }
    Label* map_check() { return &map_check_; }
   private:
    LInstanceOfKnownGlobal* instr_;
    Label map_check_;
  };

  DeferredInstanceOfKnownGlobal* deferred =
      new(zone()) DeferredInstanceOfKnownGlobal(this, instr);

  Label return_false, cache_miss;
  Register object = ToRegister(instr->value());
  Register result = ToRegister(instr->result());

  // This instruction is marked as call. We can clobber any register.
  ASSERT(instr->IsMarkedAsCall());

  // We must take into account that object is in x11.
  ASSERT(object.Is(x11));
  Register scratch = x10;

  // A Smi is not instance of anything.
  __ JumpIfSmi(object, &return_false);

  TODO_UNIMPLEMENTED("patchable inline check");

  // The inlined call site cache did not match.
  // Check null and string before calling the deferred code.
  __ Bind(&cache_miss);
  // Null is not instance of anything.
  __ JumpIfRoot(object, Heap::kNullValueRootIndex, &return_false);

  // String values are not instances of anything.
  // Return false if the object is a string. Otherwise, jump to the deferred
  // code.
  // Note that we can't jump directly to deferred code from
  // IsObjectJSStringType, because it uses tbz for the jump and the deferred
  // code can be out of range.
  __ IsObjectJSStringType(object, scratch, NULL, &return_false);
  __ B(deferred->entry());

  __ Bind(&return_false);
  __ LoadRoot(result, Heap::kFalseValueRootIndex);

  // Here result is either true or false.
  __ Bind(deferred->exit());
}


void LCodeGen::DoInstanceSize(LInstanceSize* instr) {
  Register object = ToRegister(instr->object());
  Register result = ToRegister(instr->result());
  __ Ldr(result, FieldMemOperand(object, HeapObject::kMapOffset));
  __ Ldrb(result, FieldMemOperand(result, Map::kInstanceSizeOffset));
}


void LCodeGen::DoDeferredInstanceOfKnownGlobal(LInstanceOfKnownGlobal* instr,
                                               Label* map_check) {
  Register result = ToRegister(instr->result());
  ASSERT(result.Is(x0));  // InstanceofStub returns its result in x0.
  InstanceofStub::Flags flags = InstanceofStub::kArgsInRegisters;

  PushSafepointRegistersScope scope(this, Safepoint::kWithRegisters);

  // Prepare InstanceofStub arguments.
  ASSERT(ToRegister(instr->value()).Is(InstanceofStub::left()));
  __ LoadHeapObject(InstanceofStub::right(), instr->function());

  InstanceofStub stub(flags);
  CallCodeGeneric(stub.GetCode(isolate()),
                  RelocInfo::CODE_TARGET,
                  instr,
                  RECORD_SAFEPOINT_WITH_REGISTERS_AND_NO_ARGUMENTS);
  LEnvironment* env = instr->GetDeferredLazyDeoptimizationEnvironment();
  safepoints_.RecordLazyDeoptimizationIndex(env->deoptimization_index());

  // TODO(all): This could be integrated into InstanceofStub.
  __ LoadTrueFalseRoots(x1, x2);
  ASSERT(Smi::FromInt(0) == 0);
  __ Cmp(result, 0);
  __ Csel(result, x1, x2, eq);

  // Put the result value into the result register slot.
  __ StoreToSafepointRegisterSlot(result, result);
}


void LCodeGen::DoInstructionGap(LInstructionGap* instr) {
  DoGap(instr);
}


void LCodeGen::DoInteger32ToDouble(LInteger32ToDouble* instr) {
  Register value = ToRegister32(instr->value());
  DoubleRegister result = ToDoubleRegister(instr->result());
  __ Scvtf(result, value);
}


void LCodeGen::DoInteger32ToSmi(LInteger32ToSmi* instr) {
  // A64 smis can represent all Integer32 values, so this cannot deoptimize.
  ASSERT(!instr->hydrogen()->value()->HasRange() ||
         instr->hydrogen()->value()->range()->IsInSmiRange());

  Register value = ToRegister(instr->value());
  Register result = ToRegister(instr->result());
  __ SmiTag(result, value);
}


void LCodeGen::DoInvokeFunction(LInvokeFunction* instr) {
  // The function is required to be in x1.
  ASSERT(ToRegister(instr->function()).is(x1));
  ASSERT(instr->HasPointerMap());

  Handle<JSFunction> known_function = instr->hydrogen()->known_function();
  if (known_function.is_null()) {
    LPointerMap* pointers = instr->pointer_map();
    RecordPosition(pointers->position());
    SafepointGenerator generator(this, pointers, Safepoint::kLazyDeopt);
    ParameterCount count(instr->arity());
    __ InvokeFunction(x1, count, CALL_FUNCTION, generator, CALL_AS_METHOD);
    __ Ldr(cp, MemOperand(fp, StandardFrameConstants::kContextOffset));
  } else {
    CallKnownFunction(known_function,
                      instr->hydrogen()->formal_parameter_count(),
                      instr->arity(),
                      instr,
                      CALL_AS_METHOD,
                      x1);
  }
}


void LCodeGen::DoIsConstructCallAndBranch(LIsConstructCallAndBranch* instr) {
  Register temp1 = ToRegister(instr->temp1());
  Register temp2 = ToRegister(instr->temp2());

  // Get the frame pointer for the calling frame.
  __ Ldr(temp1, MemOperand(fp, StandardFrameConstants::kCallerFPOffset));

  // Skip the arguments adaptor frame if it exists.
  Label check_frame_marker;
  __ Ldr(temp2, MemOperand(temp1, StandardFrameConstants::kContextOffset));
  __ Cmp(temp2, Operand(Smi::FromInt(StackFrame::ARGUMENTS_ADAPTOR)));
  __ B(ne, &check_frame_marker);
  __ Ldr(temp1, MemOperand(temp1, StandardFrameConstants::kCallerFPOffset));

  // Check the marker in the calling frame.
  __ Bind(&check_frame_marker);
  __ Ldr(temp1, MemOperand(temp1, StandardFrameConstants::kMarkerOffset));

  EmitCompareAndBranch(
      instr, eq, temp1, Operand(Smi::FromInt(StackFrame::CONSTRUCT)));
}


void LCodeGen::DoIsObjectAndBranch(LIsObjectAndBranch* instr) {
  Label* is_object = instr->TrueLabel(chunk_);
  Label* is_not_object = instr->FalseLabel(chunk_);
  Register value = ToRegister(instr->value());
  Register map = ToRegister(instr->temp1());
  Register scratch = ToRegister(instr->temp2());

  __ JumpIfSmi(value, is_not_object);
  __ JumpIfRoot(value, Heap::kNullValueRootIndex, is_object);

  __ Ldr(map, FieldMemOperand(value, HeapObject::kMapOffset));

  // Check for undetectable objects.
  __ Ldrb(scratch, FieldMemOperand(map, Map::kBitFieldOffset));
  __ TestAndBranchIfAnySet(scratch, 1 << Map::kIsUndetectable, is_not_object);

  // Check that instance type is in object type range.
  __ IsInstanceJSObjectType(map, scratch, NULL);
  // Flags have been updated by IsInstanceJSObjectType. We can now test the
  // flags for "le" condition to check if the object's type is a valid
  // JS object type.
  EmitBranch(instr, le);
}


Condition LCodeGen::EmitIsString(Register input,
                                 Register temp1,
                                 Label* is_not_string,
                                 SmiCheck check_needed = INLINE_SMI_CHECK) {
  if (check_needed == INLINE_SMI_CHECK) {
    __ JumpIfSmi(input, is_not_string);
  }
  __ CompareObjectType(input, temp1, temp1, FIRST_NONSTRING_TYPE);

  return lt;
}


void LCodeGen::DoIsStringAndBranch(LIsStringAndBranch* instr) {
  Register val = ToRegister(instr->value());
  Register scratch = ToRegister(instr->temp());

  SmiCheck check_needed =
      instr->hydrogen()->value()->IsHeapObject()
          ? OMIT_SMI_CHECK : INLINE_SMI_CHECK;
  Condition true_cond =
      EmitIsString(val, scratch, instr->FalseLabel(chunk_), check_needed);

  EmitBranch(instr, true_cond);
}


void LCodeGen::DoIsSmiAndBranch(LIsSmiAndBranch* instr) {
  Register value = ToRegister(instr->value());
  STATIC_ASSERT(kSmiTag == 0);
  EmitTestAndBranch(instr, eq, value, kSmiTagMask);
}


void LCodeGen::DoIsUndetectableAndBranch(LIsUndetectableAndBranch* instr) {
  Register input = ToRegister(instr->value());
  Register temp = ToRegister(instr->temp());

  if (!instr->hydrogen()->value()->IsHeapObject()) {
    __ JumpIfSmi(input, instr->FalseLabel(chunk_));
  }
  __ Ldr(temp, FieldMemOperand(input, HeapObject::kMapOffset));
  __ Ldrb(temp, FieldMemOperand(temp, Map::kBitFieldOffset));

  EmitTestAndBranch(instr, ne, temp, 1 << Map::kIsUndetectable);
}


static const char* LabelType(LLabel* label) {
  if (label->is_loop_header()) return " (loop header)";
  if (label->is_osr_entry()) return " (OSR entry)";
  return "";
}


void LCodeGen::DoLabel(LLabel* label) {
  Comment(";;; <@%d,#%d> -------------------- B%d%s --------------------",
          current_instruction_,
          label->hydrogen_value()->id(),
          label->block_id(),
          LabelType(label));

  __ Bind(label->label());
  current_block_ = label->block_id();
  DoGap(label);
}


void LCodeGen::DoLoadContextSlot(LLoadContextSlot* instr) {
  Register context = ToRegister(instr->context());
  Register result = ToRegister(instr->result());
  __ Ldr(result, ContextMemOperand(context, instr->slot_index()));
  if (instr->hydrogen()->RequiresHoleCheck()) {
    if (instr->hydrogen()->DeoptimizesOnHole()) {
      DeoptimizeIfRoot(result, Heap::kTheHoleValueRootIndex,
                       instr->environment());
    } else {
      Label not_the_hole;
      __ JumpIfNotRoot(result, Heap::kTheHoleValueRootIndex, &not_the_hole);
      __ LoadRoot(result, Heap::kUndefinedValueRootIndex);
      __ Bind(&not_the_hole);
    }
  }
}


void LCodeGen::DoLoadExternalArrayPointer(LLoadExternalArrayPointer* instr) {
  Register to_reg = ToRegister(instr->result());
  Register from_reg = ToRegister(instr->object());
  __ Ldr(to_reg, FieldMemOperand(from_reg,
                                 ExternalArray::kExternalPointerOffset));
}


void LCodeGen::DoLoadFunctionPrototype(LLoadFunctionPrototype* instr) {
  Register function = ToRegister(instr->function());
  Register result = ToRegister(instr->result());
  Register temp = ToRegister(instr->temp());
  Label deopt;

  // Check that the function really is a function. Leaves map in the result
  // register.
  __ JumpIfNotObjectType(function, result, temp, JS_FUNCTION_TYPE, &deopt);

  // Make sure that the function has an instance prototype.
  Label non_instance;
  __ Ldrb(temp, FieldMemOperand(result, Map::kBitFieldOffset));
  __ Tbnz(temp, Map::kHasNonInstancePrototype, &non_instance);

  // Get the prototype or initial map from the function.
  __ Ldr(result, FieldMemOperand(function,
                                 JSFunction::kPrototypeOrInitialMapOffset));

  // Check that the function has a prototype or an initial map.
  __ JumpIfRoot(result, Heap::kTheHoleValueRootIndex, &deopt);

  // If the function does not have an initial map, we're done.
  Label done;
  __ CompareObjectType(result, temp, temp, MAP_TYPE);
  __ B(ne, &done);

  // Get the prototype from the initial map.
  __ Ldr(result, FieldMemOperand(result, Map::kPrototypeOffset));
  __ B(&done);

  // Non-instance prototype: fetch prototype from constructor field in initial
  // map.
  __ Bind(&non_instance);
  __ Ldr(result, FieldMemOperand(result, Map::kConstructorOffset));
  __ B(&done);

  // Deoptimize case.
  __ Bind(&deopt);
  Deoptimize(instr->environment());

  // All done.
  __ Bind(&done);
}


void LCodeGen::DoLoadGlobalCell(LLoadGlobalCell* instr) {
  Register result = ToRegister(instr->result());
  __ Mov(result, Operand(Handle<Object>(instr->hydrogen()->cell())));
  __ Ldr(result, FieldMemOperand(result, Cell::kValueOffset));
  if (instr->hydrogen()->RequiresHoleCheck()) {
    DeoptimizeIfRoot(
        result, Heap::kTheHoleValueRootIndex, instr->environment());
  }
}


void LCodeGen::DoLoadGlobalGeneric(LLoadGlobalGeneric* instr) {
  ASSERT(ToRegister(instr->global_object()).Is(x0));
  ASSERT(ToRegister(instr->result()).Is(x0));
  __ Mov(x2, Operand(instr->name()));
  RelocInfo::Mode mode = instr->for_typeof() ? RelocInfo::CODE_TARGET
                                             : RelocInfo::CODE_TARGET_CONTEXT;
  Handle<Code> ic = isolate()->builtins()->LoadIC_Initialize();
  CallCode(ic, mode, instr);
}


MemOperand LCodeGen::PrepareKeyedExternalArrayOperand(Register key,
                                                      Register base,
                                                      Register scratch,
                                                      bool key_is_smi,
                                                      bool key_is_constant,
                                                      int constant_key,
                                                      int element_size_shift,
                                                      int additional_index) {
  if (key_is_constant) {
    return MemOperand(base, (constant_key + additional_index) <<
                            element_size_shift);
  }

  if (additional_index == 0) {
    if (key_is_smi) {
      // Key is smi: untag, and scale by element size.
      __ Add(scratch, base, Operand::UntagSmiAndScale(key, element_size_shift));
      return MemOperand(scratch);
    } else {
      // Key is not smi, and element size is not byte: scale by element size.
      return MemOperand(base, key, LSL, element_size_shift);
    }
  } else {
    if (key_is_smi) {
      __ SmiUntag(scratch, key);
      __ Add(scratch, scratch, additional_index);
    } else {
      __ Add(scratch, key, additional_index);
    }
    return MemOperand(base, scratch, LSL, element_size_shift);
  }
}


void LCodeGen::DoLoadKeyedExternal(LLoadKeyedExternal* instr) {
  Register ext_ptr = ToRegister(instr->elements());
  Register scratch;
  ElementsKind elements_kind = instr->elements_kind();

  bool key_is_smi = instr->hydrogen()->key()->representation().IsSmi();
  bool key_is_constant = instr->key()->IsConstantOperand();
  Register key = no_reg;
  int constant_key = 0;
  if (key_is_constant) {
    ASSERT(instr->temp() == NULL);
    constant_key = ToInteger32(LConstantOperand::cast(instr->key()));
    if (constant_key & 0xf0000000) {
      Abort("Array index constant value too big.");
    }
  } else {
    scratch = ToRegister(instr->temp());
    key = ToRegister(instr->key());
  }

  int element_size_shift = ElementsKindToShiftSize(elements_kind);
  MemOperand mem_op =
      PrepareKeyedExternalArrayOperand(key, ext_ptr, scratch, key_is_smi,
                                       key_is_constant, constant_key,
                                       element_size_shift,
                                       instr->additional_index());

  if (elements_kind == EXTERNAL_FLOAT_ELEMENTS) {
    DoubleRegister result = ToDoubleRegister(instr->result());
    __ Ldr(result.S(), mem_op);
    __ Fcvt(result, result.S());
  } else if (elements_kind == EXTERNAL_DOUBLE_ELEMENTS) {
    DoubleRegister result = ToDoubleRegister(instr->result());
    __ Ldr(result, mem_op);
  } else {
    Register result = ToRegister(instr->result());

    switch (elements_kind) {
      case EXTERNAL_BYTE_ELEMENTS:            __ Ldrsb(result, mem_op); break;
      case EXTERNAL_PIXEL_ELEMENTS:           // Fall through.
      case EXTERNAL_UNSIGNED_BYTE_ELEMENTS:   __ Ldrb(result, mem_op); break;
      case EXTERNAL_SHORT_ELEMENTS:           __ Ldrsh(result, mem_op); break;
      case EXTERNAL_UNSIGNED_SHORT_ELEMENTS:  __ Ldrh(result, mem_op); break;
      case EXTERNAL_INT_ELEMENTS:             __ Ldrsw(result, mem_op); break;
      case EXTERNAL_UNSIGNED_INT_ELEMENTS:
        __ Ldr(result.W(), mem_op);
        if (!instr->hydrogen()->CheckFlag(HInstruction::kUint32)) {
          // Deopt if value > 0x80000000.
          __ Tst(result, 0xFFFFFFFF80000000);
          DeoptimizeIf(ne, instr->environment());
        }
        break;
      case EXTERNAL_FLOAT_ELEMENTS:
      case EXTERNAL_DOUBLE_ELEMENTS:
      case FAST_HOLEY_DOUBLE_ELEMENTS:
      case FAST_HOLEY_ELEMENTS:
      case FAST_HOLEY_SMI_ELEMENTS:
      case FAST_DOUBLE_ELEMENTS:
      case FAST_ELEMENTS:
      case FAST_SMI_ELEMENTS:
      case DICTIONARY_ELEMENTS:
      case NON_STRICT_ARGUMENTS_ELEMENTS:
        UNREACHABLE();
        break;
    }
  }
}


void LCodeGen::CalcKeyedArrayBaseRegister(Register base,
                                          Register elements,
                                          Register key,
                                          bool key_is_tagged,
                                          ElementsKind elements_kind) {
  int element_size_shift = ElementsKindToShiftSize(elements_kind);

  // Even though the HLoad/StoreKeyed instructions force the input
  // representation for the key to be an integer, the input gets replaced during
  // bounds check elimination with the index argument to the bounds check, which
  // can be tagged, so that case must be handled here, too.
  if (key_is_tagged) {
    __ Add(base, elements, Operand::UntagSmiAndScale(key, element_size_shift));
  } else {
    // Sign extend key because it could be a 32-bit negative value and the
    // address computation happens in 64-bit.
    ASSERT((element_size_shift >= 0) && (element_size_shift <= 4));
    __ Add(base, elements, Operand(key, SXTW, element_size_shift));
  }
}


void LCodeGen::DoLoadKeyedFixedDouble(LLoadKeyedFixedDouble* instr) {
  Register elements = ToRegister(instr->elements());
  DoubleRegister result = ToDoubleRegister(instr->result());
  Register load_base;
  int offset = 0;

  if (instr->key()->IsConstantOperand()) {
    ASSERT(instr->hydrogen()->RequiresHoleCheck() ||
           (instr->temp() == NULL));

    int constant_key = ToInteger32(LConstantOperand::cast(instr->key()));
    if (constant_key & 0xf0000000) {
      Abort("Array index constant value too big.");
    }
    offset = FixedDoubleArray::OffsetOfElementAt(constant_key +
                                                 instr->additional_index());
    load_base = elements;
  } else {
    load_base = ToRegister(instr->temp());
    Register key = ToRegister(instr->key());
    bool key_is_tagged = instr->hydrogen()->key()->representation().IsSmi();
    CalcKeyedArrayBaseRegister(load_base, elements, key, key_is_tagged,
                               instr->hydrogen()->elements_kind());
    offset = FixedDoubleArray::OffsetOfElementAt(instr->additional_index());
  }
  __ Ldr(result, FieldMemOperand(load_base, offset));

  if (instr->hydrogen()->RequiresHoleCheck()) {
    Register scratch = ToRegister(instr->temp());

    // TODO(all): Is it faster to reload this value to an integer register, or
    // move from fp to integer?
    __ Fmov(scratch, result);
    __ Cmp(scratch, kHoleNanInt64);
    DeoptimizeIf(eq, instr->environment());
  }
}


void LCodeGen::DoLoadKeyedFixed(LLoadKeyedFixed* instr) {
  Register elements = ToRegister(instr->elements());
  Register result = ToRegister(instr->result());
  Register load_base;
  int offset = 0;

  if (instr->key()->IsConstantOperand()) {
    ASSERT(instr->temp() == NULL);
    LConstantOperand* const_operand = LConstantOperand::cast(instr->key());
    offset = FixedArray::OffsetOfElementAt(ToInteger32(const_operand) +
                                           instr->additional_index());
    load_base = elements;
  } else {
    load_base = ToRegister(instr->temp());
    Register key = ToRegister(instr->key());
    bool key_is_tagged = instr->hydrogen()->key()->representation().IsSmi();
    CalcKeyedArrayBaseRegister(load_base, elements, key, key_is_tagged,
                               instr->hydrogen()->elements_kind());
    offset = FixedArray::OffsetOfElementAt(instr->additional_index());
  }
  __ Ldr(result, FieldMemOperand(load_base, offset));

  if (instr->hydrogen()->RequiresHoleCheck()) {
    if (IsFastSmiElementsKind(instr->hydrogen()->elements_kind())) {
      DeoptimizeIfNotSmi(result, instr->environment());
    } else {
      DeoptimizeIfRoot(result, Heap::kTheHoleValueRootIndex,
                       instr->environment());
    }
  }
}


void LCodeGen::DoLoadKeyedGeneric(LLoadKeyedGeneric* instr) {
  ASSERT(ToRegister(instr->object()).Is(x1));
  ASSERT(ToRegister(instr->key()).Is(x0));

  Handle<Code> ic = isolate()->builtins()->KeyedLoadIC_Initialize();
  CallCode(ic, RelocInfo::CODE_TARGET, instr);

  ASSERT(ToRegister(instr->result()).Is(x0));
}


void LCodeGen::DoLoadNamedField(LLoadNamedField* instr) {
  HObjectAccess access = instr->hydrogen()->access();
  int offset = access.offset();
  Register object = ToRegister(instr->object());

  if (instr->hydrogen()->representation().IsDouble()) {
    FPRegister result = ToDoubleRegister(instr->result());
    __ Ldr(result, FieldMemOperand(object, offset));
  } else {
    Register result = ToRegister(instr->result());
    if (access.IsInobject()) {
      __ Ldr(result, FieldMemOperand(object, offset));
    } else {
      __ Ldr(result, FieldMemOperand(object, JSObject::kPropertiesOffset));
      __ Ldr(result, FieldMemOperand(result, offset));
    }
  }
}


void LCodeGen::EmitLoadFieldOrConstantFunction(Register result,
                                               Register object,
                                               Handle<Map> type,
                                               Handle<String> name,
                                               LEnvironment* env) {
  LookupResult lookup(isolate());
  type->LookupDescriptor(NULL, *name, &lookup);
  ASSERT(lookup.IsFound() || lookup.IsCacheable());

  if (lookup.IsField()) {
    int index = lookup.GetLocalFieldIndexFromMap(*type);
    int offset = index * kPointerSize;
    if (index < 0) {
      // Negative property indices are in-object properties, indexed from the
      // end of the fixed part of the object.
      __ Ldr(result, FieldMemOperand(object, offset + type->instance_size()));
    } else {
      // Non-negative property indices are in the properties array.
      __ Ldr(result, FieldMemOperand(object, JSObject::kPropertiesOffset));
      __ Ldr(result, FieldMemOperand(result, offset + FixedArray::kHeaderSize));
    }
  } else if (lookup.IsConstantFunction()) {
    Handle<JSFunction> function(lookup.GetConstantFunctionFromMap(*type));
    __ LoadHeapObject(result, function);
  } else {
    // Negative lookup. Check prototypes.
    Handle<HeapObject> current(HeapObject::cast((*type)->prototype()));
    Heap* heap = type->GetHeap();
    while (*current != heap->null_value()) {
      __ LoadHeapObject(result, current);
      __ CompareMap(result, result, Handle<Map>(current->map()));
      DeoptimizeIf(ne, env);
      current =
          Handle<HeapObject>(HeapObject::cast(current->map()->prototype()));
    }
    __ LoadRoot(result, Heap::kUndefinedValueRootIndex);
  }
}


void LCodeGen::DoLoadNamedFieldPolymorphic(LLoadNamedFieldPolymorphic* instr) {
  Register object = ToRegister(instr->object());
  Register result = ToRegister(instr->result());
  // The result register is loaded with its value when the object's map has been
  // found. At this point we don't need to hold the map in object_map anymore,
  // so both values can share the same register.
  // However when we need to go through the generic code path, the instruction
  // is MarkedAsCall and both object and result registers will be allocated to
  // x0. Object should not be clobbered until the call to LoadIC. We choose a
  // different arbitrary register for object_map in this case.
  Register object_map = instr->IsMarkedAsCall()
      ? x10
      : result;

  int map_count = instr->hydrogen()->types()->length();
  bool need_generic = instr->hydrogen()->need_generic();

  if ((map_count == 0) && !need_generic) {
    Deoptimize(instr->environment());
    return;
  }

  Handle<String> name = instr->hydrogen()->name();
  Label done;
  __ Ldr(object_map, FieldMemOperand(object, HeapObject::kMapOffset));
  for (int i = 0; i < map_count; i++) {
    bool last = (i == (map_count - 1));
    Handle<Map> map = instr->hydrogen()->types()->at(i);
    Label check_passed;
    __ CompareMap(object_map, map, &check_passed);
    if (last && !need_generic) {
      DeoptimizeIf(ne, instr->environment());
      __ Bind(&check_passed);
      EmitLoadFieldOrConstantFunction(result, object, map, name,
                                      instr->environment());
    } else {
      Label next;
      __ B(ne, &next);
      __ Bind(&check_passed);
      EmitLoadFieldOrConstantFunction(result, object, map, name,
                                      instr->environment());
      __ B(&done);
      __ Bind(&next);
    }
  }
  if (need_generic) {
    ASSERT(instr->IsMarkedAsCall());
    // LoadIC expects x2 to hold the name, and x0 to hold the receiver.
    ASSERT(object.Is(x0));
    __ Mov(x2, Operand(name));
    Handle<Code> ic = isolate()->builtins()->LoadIC_Initialize();
    CallCode(ic, RelocInfo::CODE_TARGET, instr);
  }
  __ Bind(&done);
}


void LCodeGen::DoLoadNamedGeneric(LLoadNamedGeneric* instr) {
  // LoadIC expects x2 to hold the name, and x0 to hold the receiver.
  ASSERT(ToRegister(instr->object()).is(x0));
  __ Mov(x2, Operand(instr->name()));

  Handle<Code> ic = isolate()->builtins()->LoadIC_Initialize();
  CallCode(ic, RelocInfo::CODE_TARGET, instr);

  ASSERT(ToRegister(instr->result()).is(x0));
}


void LCodeGen::DoMapEnumLength(LMapEnumLength* instr) {
  Register result = ToRegister(instr->result());
  Register map = ToRegister(instr->value());
  __ EnumLengthSmi(result, map);
}


void LCodeGen::DoMathAbs(LMathAbs* instr) {
  Representation r = instr->hydrogen()->value()->representation();
  if (r.IsDouble()) {
    DoubleRegister input = ToDoubleRegister(instr->value());
    DoubleRegister result = ToDoubleRegister(instr->result());
    __ Fabs(result, input);
  } else {
    ASSERT(r.IsInteger32());
    Register input = ToRegister32(instr->value());
    Register result = ToRegister32(instr->result());
    Label done;
    __ Abs(result, input, NULL, &done);
    Deoptimize(instr->environment());
    __ Bind(&done);
  }
}


void LCodeGen::DoDeferredMathAbsTagged(LMathAbsTagged* instr,
                                       Label* exit,
                                       Label* allocation_entry) {
  // Handle the tricky cases of MathAbsTagged:
  //  - HeapNumber inputs.
  //    - Negative inputs produce a positive result, so a new HeapNumber is
  //      allocated to hold it.
  //    - Positive inputs are returned as-is, since there is no need to allocate
  //      a new HeapNumber for the result.
  //  - The (smi) input -0x80000000, produces +0x80000000, which does not fit
  //    a smi. In this case, the inline code sets the result and jumps directly
  //    to the allocation_entry label.
  Register input = ToRegister(instr->value());
  Register temp1 = ToRegister(instr->temp1());
  Register temp2 = ToRegister(instr->temp2());
  Register result_bits = ToRegister(instr->temp3());
  Register result = ToRegister(instr->result());

  Label runtime_allocation;

  // Deoptimize if the input is not a HeapNumber.
  __ Ldr(temp1, FieldMemOperand(input, HeapObject::kMapOffset));
  DeoptimizeIfNotRoot(temp1, Heap::kHeapNumberMapRootIndex,
                      instr->environment());

  // If the argument is positive, we can return it as-is, without any need to
  // allocate a new HeapNumber for the result. We have to do this in integer
  // registers (rather than with fabs) because we need to be able to distinguish
  // the two zeroes.
  __ Ldr(result_bits, FieldMemOperand(input, HeapNumber::kValueOffset));
  __ Mov(result, input);
  __ Tbz(result_bits, kXSignBit, exit);

  // Calculate abs(input) by clearing the sign bit.
  __ Bic(result_bits, result_bits, kXSignMask);

  // Allocate a new HeapNumber to hold the result.
  //  result_bits   The bit representation of the (double) result.
  __ Bind(allocation_entry);
  __ AllocateHeapNumber(result, &runtime_allocation, temp1, temp2);
  // The inline (non-deferred) code will store result_bits into result.
  __ B(exit);

  __ Bind(&runtime_allocation);
  if (FLAG_debug_code) {
    // Because result is in the pointer map, we need to make sure it has a valid
    // tagged value before we call the runtime. We speculatively set it to the
    // input (for abs(+x)) or to a smi (for abs(-SMI_MIN)), so it should already
    // be valid.
    Label result_ok;
    Register input = ToRegister(instr->value());
    __ JumpIfSmi(result, &result_ok);
    __ Cmp(input, result);
    DeoptimizeIf(ne, instr->environment());
    __ Bind(&result_ok);
  }

  { PushSafepointRegistersScope scope(this, Safepoint::kWithRegisters);
    CallRuntimeFromDeferred(Runtime::kAllocateHeapNumber, 0, instr);
    __ StoreToSafepointRegisterSlot(x0, result);
  }
  // The inline (non-deferred) code will store result_bits into result.
}


void LCodeGen::DoMathAbsTagged(LMathAbsTagged* instr) {
  // Class for deferred case.
  class DeferredMathAbsTagged: public LDeferredCode {
   public:
    DeferredMathAbsTagged(LCodeGen* codegen, LMathAbsTagged* instr)
        : LDeferredCode(codegen), instr_(instr) { }
    virtual void Generate() {
      codegen()->DoDeferredMathAbsTagged(instr_, exit(),
                                         allocation_entry());
    }
    virtual LInstruction* instr() { return instr_; }
    Label* allocation_entry() { return &allocation; }
   private:
    LMathAbsTagged* instr_;
    Label allocation;
  };

  // TODO(jbramley): The early-exit mechanism would skip the new frame handling
  // in GenerateDeferredCode. Tidy this up.
  ASSERT(!NeedsDeferredFrame());

  DeferredMathAbsTagged* deferred =
      new(zone()) DeferredMathAbsTagged(this, instr);

  ASSERT(instr->hydrogen()->value()->representation().IsTagged());
  Register input = ToRegister(instr->value());
  Register result_bits = ToRegister(instr->temp3());
  Register result = ToRegister(instr->result());
  Label done;

  // Handle smis inline.
  // We can treat smis as 64-bit integers, since the (low-order) tag bits will
  // never get set by the negation. This is therefore the same as the Integer32
  // case in DoMathAbs, except that it operates on 64-bit values.
  STATIC_ASSERT((kSmiValueSize == 32) && (kSmiShift == 32) && (kSmiTag == 0));

  // TODO(jbramley): We can't use JumpIfNotSmi here because the tbz it uses
  // doesn't always have enough range. Consider making a variant of it, or a
  // TestIsSmi helper.
  STATIC_ASSERT(kSmiTag == 0);
  __ Tst(input, kSmiTagMask);
  __ B(ne, deferred->entry());

  __ Abs(result, input, NULL, &done);

  // The result is the magnitude (abs) of the smallest value a smi can
  // represent, encoded as a double.
  __ Mov(result_bits, double_to_rawbits(0x80000000));
  __ B(deferred->allocation_entry());

  __ Bind(deferred->exit());
  __ Str(result_bits, FieldMemOperand(result, HeapNumber::kValueOffset));

  __ Bind(&done);
}


void LCodeGen::DoMathCos(LMathCos* instr) {
  ASSERT(ToDoubleRegister(instr->result()).is(d0));
  TranscendentalCacheStub stub(TranscendentalCache::COS,
                               TranscendentalCacheStub::UNTAGGED);
  CallCode(stub.GetCode(isolate()), RelocInfo::CODE_TARGET, instr);
  ASSERT(ToDoubleRegister(instr->result()).Is(d0));
}


void LCodeGen::DoRandom(LRandom* instr) {
  class DeferredDoRandom: public LDeferredCode {
   public:
    DeferredDoRandom(LCodeGen* codegen, LRandom* instr)
        : LDeferredCode(codegen), instr_(instr) { }
    virtual void Generate() { codegen()->DoDeferredRandom(instr_); }
    virtual LInstruction* instr() { return instr_; }

   private:
    LRandom* instr_;
  };

  DeferredDoRandom* deferred = new(zone()) DeferredDoRandom(this, instr);

  // Having marked this instruction as a call we can use any registers.
  ASSERT(instr->IsMarkedAsCall());
  ASSERT(ToDoubleRegister(instr->result()).is(d7));
  ASSERT(ToRegister(instr->global_object()).is(x0));

  static const int kSeedSize = sizeof(uint32_t);
  STATIC_ASSERT(kPointerSize == 2 * kSeedSize);

  Register global_object = x0;
  __ Ldr(global_object,
         FieldMemOperand(global_object, GlobalObject::kNativeContextOffset));
  static const int kRandomSeedOffset =
      FixedArray::kHeaderSize + Context::RANDOM_SEED_INDEX * kPointerSize;
  __ Ldr(x1, FieldMemOperand(global_object, kRandomSeedOffset));
  // x1: FixedArray of the native context's random seeds

  // Load state[0].
  __ Ldr(w2, FieldMemOperand(x1, ByteArray::kHeaderSize));
  // If state[0] == 0, call runtime to initialize seeds.
  __ Cbz(w2, deferred->entry());
  // Load state[1].
  __ Ldr(w3, FieldMemOperand(x1, ByteArray::kHeaderSize + kSeedSize));

  // state[0] = 18273 * (state[0] & 0xFFFF) + (state[0] >> 16)
  __ And(w4, w2, 0xFFFF);
  __ Mov(w5, 18273);
  __ Mul(w5, w5, w4);
  __ Add(w2, w5, Operand(w2, LSR, 16));
  // Save state[0].
  __ Str(w2, FieldMemOperand(x1, ByteArray::kHeaderSize));

  // state[1] = 36969 * (state[1] & 0xFFFF) + (state[1] >> 16)
  __ And(w4, w3, 0xFFFF);
  __ Mov(w5, 36969);
  __ Mul(w5, w5, w4);
  __ Add(w3, w5, Operand(w3, LSR, 16));
  // Save state[1].
  __ Str(w3, FieldMemOperand(x1, ByteArray::kHeaderSize + kSeedSize));

  // Random bit pattern = (state[0] << 14) + (state[1] & 0x3FFFF)
  __ And(w3, w3, 0x3FFFF);
  __ Add(w0, w3, Operand(w2, LSL, 14));

  __ Bind(deferred->exit());
  // Interpret the 32 random bits as a 0.32 fixed point number, and convert to
  // a double in the range 0.0 <= number < 1.0.
  __ Ucvtf(d7, w0, 32);
}


void LCodeGen::DoDeferredRandom(LRandom* instr) {
  __ CallCFunction(ExternalReference::random_uint32_function(isolate()), 1);
  // Return value is in x0.
}


void LCodeGen::DoMathExp(LMathExp* instr) {
  DoubleRegister input = ToDoubleRegister(instr->value());
  DoubleRegister result = ToDoubleRegister(instr->result());
  DoubleRegister double_temp1 = ToDoubleRegister(instr->double_temp1());
  DoubleRegister double_temp2 = double_scratch();
  Register temp1 = ToRegister(instr->temp1());
  Register temp2 = ToRegister(instr->temp2());
  Register temp3 = ToRegister(instr->temp3());

  MathExpGenerator::EmitMathExp(masm(), input, result,
                                double_temp1, double_temp2,
                                temp1, temp2, temp3);
}


void LCodeGen::DoMathFloor(LMathFloor* instr) {
  // TODO(jbramley): If we could provide a double result, we could use frintm
  // and produce a valid double result in a single instruction.
  DoubleRegister input = ToDoubleRegister(instr->value());
  Register result = ToRegister(instr->result());
  Label deopt;
  Label done;

  if (instr->hydrogen()->CheckFlag(HValue::kBailoutOnMinusZero)) {
    // Check for an input of -0.0, using the result register as a scratch.
    __ Fmov(result, input);
    __ Cmp(result, 1);
    __ B(&deopt, vs);
  }

  __ Fcvtms(result, input);

  // Check that the result fits into a 32-bit integer.
  //  - The result did not overflow.
  __ Cmp(result, Operand(result, SXTW));
  //  - The input was not NaN.
  __ Fccmp(input, input, NoFlag, eq);
  __ B(&done, eq);

  __ Bind(&deopt);
  Deoptimize(instr->environment());

  __ Bind(&done);
}


void LCodeGen::DoMathFloorOfDiv(LMathFloorOfDiv* instr) {
  Register result = ToRegister32(instr->result());
  Register left = ToRegister32(instr->left());
  Register right = ToRegister32(instr->right());
  Register remainder = ToRegister32(instr->temp());

  // This can't cause an exception on ARM, so we can speculatively
  // execute it already now.
  __ Sdiv(result, left, right);

  // Check for x / 0.
  DeoptimizeIfZero(right, instr->environment());

  // Check for (kMinInt / -1).
  if (instr->hydrogen()->CheckFlag(HValue::kCanOverflow)) {
    // The V flag will be set iff left == kMinInt.
    __ Cmp(left, 1);
    __ Ccmp(right, -1, NoFlag, vs);
    DeoptimizeIf(eq, instr->environment());
  }

  // Check for (0 / -x) that will produce negative zero.
  if (instr->hydrogen()->CheckFlag(HValue::kBailoutOnMinusZero)) {
    __ Cmp(right, 0);
    __ Ccmp(left, 0, ZFlag, mi);
    // "right" can't be null because the code would have already been
    // deoptimized. The Z flag is set only if (right < 0) and (left == 0).
    // In this case we need to deoptimize to produce a -0.
    DeoptimizeIf(eq, instr->environment());
  }

  Label done;
  // If both operands have the same sign then we are done.
  __ Eor(remainder, left, right);
  __ Tbz(remainder, kWSignBit, &done);

  // Check if the result needs to be corrected.
  __ Msub(remainder, result, right, left);
  __ Cbz(remainder, &done);
  __ Sub(result, result, 1);

  __ Bind(&done);
}


void LCodeGen::DoMathLog(LMathLog* instr) {
  ASSERT(ToDoubleRegister(instr->result()).is(d0));
  TranscendentalCacheStub stub(TranscendentalCache::LOG,
                               TranscendentalCacheStub::UNTAGGED);
  CallCode(stub.GetCode(isolate()), RelocInfo::CODE_TARGET, instr);
  ASSERT(ToDoubleRegister(instr->result()).Is(d0));
}


void LCodeGen::DoMathPowHalf(LMathPowHalf* instr) {
  DoubleRegister input = ToDoubleRegister(instr->value());
  DoubleRegister result = ToDoubleRegister(instr->result());
  Label done;

  // Math.pow(x, 0.5) differs from fsqrt(x) in the following cases:
  //  Math.pow(-Infinity, 0.5) == +Infinity
  //  Math.pow(-0.0, 0.5) == +0.0

  // Catch -infinity inputs first.
  // TODO(jbramley): A constant infinity register would be helpful here.
  __ Fmov(double_scratch(), kFP64NegativeInfinity);
  __ Fcmp(double_scratch(), input);
  __ Fabs(result, input);
  __ B(&done, eq);

  // Add +0.0 to convert -0.0 to +0.0.
  // TODO(jbramley): A constant zero register would be helpful here.
  __ Fmov(double_scratch(), 0.0);
  __ Fadd(double_scratch(), input, double_scratch());
  __ Fsqrt(result, double_scratch());

  __ Bind(&done);
}


void LCodeGen::DoPower(LPower* instr) {
  Representation exponent_type = instr->hydrogen()->right()->representation();
  // Having marked this as a call, we can use any registers.
  // Just make sure that the input/output registers are the expected ones.
  ASSERT(!instr->right()->IsDoubleRegister() ||
         ToDoubleRegister(instr->right()).is(d1));
  ASSERT(!instr->right()->IsRegister() ||
         ToRegister(instr->right()).is(x11));
  ASSERT(ToDoubleRegister(instr->left()).is(d0));
  ASSERT(ToDoubleRegister(instr->result()).is(d0));

  if (exponent_type.IsSmi()) {
    MathPowStub stub(MathPowStub::TAGGED);
    __ CallStub(&stub);
  } else if (exponent_type.IsTagged()) {
    Label no_deopt;
    __ JumpIfSmi(x11, &no_deopt);
    __ Ldr(x0, FieldMemOperand(x11, HeapObject::kMapOffset));
    DeoptimizeIfNotRoot(x0, Heap::kHeapNumberMapRootIndex,
                        instr->environment());
    __ Bind(&no_deopt);
    MathPowStub stub(MathPowStub::TAGGED);
    __ CallStub(&stub);
  } else if (exponent_type.IsInteger32()) {
    MathPowStub stub(MathPowStub::INTEGER);
    __ CallStub(&stub);
  } else {
    ASSERT(exponent_type.IsDouble());
    MathPowStub stub(MathPowStub::DOUBLE);
    __ CallStub(&stub);
  }
}


void LCodeGen::DoMathRound(LMathRound* instr) {
  // TODO(jbramley): We could provide a double result here using frint.
  DoubleRegister input = ToDoubleRegister(instr->value());
  DoubleRegister temp1 = ToDoubleRegister(instr->temp1());
  Register result = ToRegister(instr->result());
  Label try_rounding;
  Label deopt;
  Label done;

  // Math.round() rounds to the nearest integer, with ties going towards
  // +infinity. This does not match any IEEE-754 rounding mode.
  //  - Infinities and NaNs are propagated unchanged, but cause deopts because
  //    they can't be represented as integers.
  //  - The sign of the result is the same as the sign of the input. This means
  //    that -0.0 rounds to itself, and values -0.5 <= input < 0 also produce a
  //    result of -0.0.

  DoubleRegister dot_five = double_scratch();
  __ Fmov(dot_five, 0.5);
  __ Fabs(temp1, input);
  __ Fcmp(temp1, dot_five);
  // If input is in [-0.5, -0], the result is -0.
  // If input is in [+0, +0.5[, the result is +0.
  // If the input is +0.5, the result is 1.
  __ B(hi, &try_rounding);  // hi so NaN will also branch.

  if (instr->hydrogen()->CheckFlag(HValue::kBailoutOnMinusZero)) {
    __ Fmov(result, input);
    __ Cmp(result, 0);
    DeoptimizeIf(mi, instr->environment());  // [-0.5, -0.0].
  }
  __ Fcmp(input, dot_five);
  __ Mov(result, 1);  // +0.5.
  // Remaining cases: [+0, +0.5[ or [-0.5, +0.5[, depending on
  // flag kBailoutOnMinusZero, will return 0 (xzr).
  __ Csel(result, result, xzr, eq);
  __ B(&done);

  __ Bind(&deopt);
  Deoptimize(instr->environment());

  __ Bind(&try_rounding);
  // Since we're providing a 32-bit result, we can implement ties-to-infinity by
  // adding 0.5 to the input, then taking the floor of the result. This does not
  // work for very large positive doubles because adding 0.5 would cause an
  // intermediate rounding stage, so a different approach will be necessary if a
  // double result is needed.
  __ Fadd(temp1, input, dot_five);
  __ Fcvtms(result, temp1);

  // Deopt if
  //  * the input was NaN
  //  * the result is not representable using a 32-bit integer.
  __ Fcmp(input, 0.0);
  __ Ccmp(result, Operand(result.W(), SXTW), NoFlag, vc);
  __ B(ne, &deopt);

  __ Bind(&done);
}


void LCodeGen::DoMathSin(LMathSin* instr) {
  ASSERT(ToDoubleRegister(instr->result()).is(d0));
  TranscendentalCacheStub stub(TranscendentalCache::SIN,
                               TranscendentalCacheStub::UNTAGGED);
  CallCode(stub.GetCode(isolate()), RelocInfo::CODE_TARGET, instr);
  ASSERT(ToDoubleRegister(instr->result()).Is(d0));
}


void LCodeGen::DoMathSqrt(LMathSqrt* instr) {
  DoubleRegister input = ToDoubleRegister(instr->value());
  DoubleRegister result = ToDoubleRegister(instr->result());
  __ Fsqrt(result, input);
}


void LCodeGen::DoMathTan(LMathTan* instr) {
  ASSERT(ToDoubleRegister(instr->result()).is(d0));
  TranscendentalCacheStub stub(TranscendentalCache::TAN,
                               TranscendentalCacheStub::UNTAGGED);
  CallCode(stub.GetCode(isolate()), RelocInfo::CODE_TARGET, instr);
  ASSERT(ToDoubleRegister(instr->result()).Is(d0));
}


void LCodeGen::DoMathMinMax(LMathMinMax* instr) {
  HMathMinMax::Operation op = instr->hydrogen()->operation();
  if (instr->hydrogen()->representation().IsInteger32()) {
    Register result = ToRegister32(instr->result());
    Register left = ToRegister32(instr->left());
    Operand right = ToOperand32(instr->right());

    __ Cmp(left, right);
    __ Csel(result, left, right, (op == HMathMinMax::kMathMax) ? ge : le);
  } else {
    ASSERT(instr->hydrogen()->representation().IsDouble());
    DoubleRegister result = ToDoubleRegister(instr->result());
    DoubleRegister left = ToDoubleRegister(instr->left());
    DoubleRegister right = ToDoubleRegister(instr->right());

    if (op == HMathMinMax::kMathMax) {
      __ Fmax(result, left, right);
    } else {
      ASSERT(op == HMathMinMax::kMathMin);
      __ Fmin(result, left, right);
    }
  }
}


void LCodeGen::DoModI(LModI* instr) {
  HMod* hmod = instr->hydrogen();
  HValue* hleft = hmod->left();
  HValue* hright = hmod->right();

  Label done;
  Register result = ToRegister32(instr->result());
  Register dividend = ToRegister32(instr->left());

  bool need_minus_zero_check = (hmod->CheckFlag(HValue::kBailoutOnMinusZero) &&
                                hleft->CanBeNegative() && hmod->CanBeZero());

  if (hmod->HasPowerOf2Divisor()) {
    // Note: The code below even works when right contains kMinInt.
    int32_t divisor = Abs(hright->GetInteger32Constant());

    if (hleft->CanBeNegative()) {
      __ Cmp(dividend, 0);
      __ Cneg(result, dividend, mi);
      __ And(result, result, divisor - 1);
      __ Cneg(result, result, mi);
      if (need_minus_zero_check) {
        __ Cbnz(result, &done);
        // The result is 0. Deoptimize if the dividend was negative.
        DeoptimizeIf(mi, instr->environment());
      }
    } else {
      __ And(result, dividend, divisor - 1);
    }

  } else {
    Label deopt;
    Register divisor = ToRegister32(instr->right());
    // Compute:
    //   modulo = dividend - quotient * divisor
    __ Sdiv(result, dividend, divisor);
    if (hright->CanBeZero()) {
      // Combine the deoptimization sites.
      Label ok;
      __ Cbnz(divisor, &ok);
      __ Bind(&deopt);
      Deoptimize(instr->environment());
      __ Bind(&ok);
    }
    __ Msub(result, result, divisor, dividend);
    if (need_minus_zero_check) {
      __ Cbnz(result, &done);
      if (deopt.is_bound()) {
        __ Tbnz(dividend, kWSignBit, &deopt);
      } else {
        DeoptimizeIfNegative(dividend, instr->environment());
      }
    }
  }
  __ Bind(&done);
}


void LCodeGen::DoMulConstI(LMulConstI* instr) {
  Register result = ToRegister32(instr->result());
  Register left = ToRegister32(instr->left());
  int32_t right = ToInteger32(instr->right());

  bool can_overflow = instr->hydrogen()->CheckFlag(HValue::kCanOverflow);
  bool bailout_on_minus_zero =
    instr->hydrogen()->CheckFlag(HValue::kBailoutOnMinusZero);

  if (bailout_on_minus_zero) {
    if (right < 0) {
      // The result is -0 if right is negative and left is zero.
      DeoptimizeIfZero(left, instr->environment());
    } else if (right == 0) {
      // The result is -0 if the right is zero and the left is negative.
      DeoptimizeIfNegative(left, instr->environment());
    }
  }

  switch (right) {
    // Cases which can detect overflow.
    case -1:
      if (can_overflow) {
        // Only 0x80000000 can overflow here.
        __ Negs(result, left);
        DeoptimizeIf(vs, instr->environment());
      } else {
        __ Neg(result, left);
      }
      break;
    case 0:
      // This case can never overflow.
      __ Mov(result, 0);
      break;
    case 1:
      // This case can never overflow.
      __ Mov(result, left, kDiscardForSameWReg);
      break;
    case 2:
      if (can_overflow) {
        __ Adds(result, left, left);
        DeoptimizeIf(vs, instr->environment());
      } else {
        __ Add(result, left, left);
      }
      break;

    // All other cases cannot detect overflow, because it would probably be no
    // faster than using the smull method in LMulI.
    // TODO(jbramley): Investigate this, and add overflow support if it would
    // be useful.
    default:
      ASSERT(!can_overflow);

      // Multiplication by constant powers of two (and some related values)
      // can be done efficiently with shifted operands.
      if (right >= 0) {
        if (IsPowerOf2(right)) {
          // result = left << log2(right)
          __ Lsl(result, left, WhichPowerOf2(right));
        } else if (IsPowerOf2(right - 1)) {
          // result = left + left << log2(right - 1)
          __ Add(result, left, Operand(left, LSL, WhichPowerOf2(right - 1)));
        } else if (IsPowerOf2(right + 1)) {
          // result = -left + left << log2(right + 1)
          __ Sub(result, left, Operand(left, LSL, WhichPowerOf2(right + 1)));
          __ Neg(result, result);
        } else {
          UNREACHABLE();
        }
      } else {
        if (IsPowerOf2(-right)) {
          // result = -left << log2(-right)
          __ Neg(result, Operand(left, LSL, WhichPowerOf2(-right)));
        } else if (IsPowerOf2(-right + 1)) {
          // result = left - left << log2(-right + 1)
          __ Sub(result, left, Operand(left, LSL, WhichPowerOf2(-right + 1)));
        } else if (IsPowerOf2(-right - 1)) {
          // result = -left - left << log2(-right - 1)
          __ Add(result, left, Operand(left, LSL, WhichPowerOf2(-right - 1)));
          __ Neg(result, result);
        } else {
          UNREACHABLE();
        }
      }
      break;
  }
}


void LCodeGen::DoMulI(LMulI* instr) {
  Register result = ToRegister32(instr->result());
  Register left = ToRegister32(instr->left());

  bool can_overflow = instr->hydrogen()->CheckFlag(HValue::kCanOverflow);
  bool bailout_on_minus_zero =
    instr->hydrogen()->CheckFlag(HValue::kBailoutOnMinusZero);

  Register right = ToRegister32(instr->right());
  if (bailout_on_minus_zero) {
    // If one operand is zero and the other is negative, the result is -0.
    //  - Set Z (eq) if either left or right, or both, are 0.
    __ Cmp(left, 0);
    __ Ccmp(right, 0, ZFlag, ne);
    //  - If so (eq), set N (mi) if left + right is negative.
    //  - Otherwise, clear N.
    __ Ccmn(left, right, NoFlag, eq);
    DeoptimizeIf(mi, instr->environment());
  }

  if (can_overflow) {
    __ Smull(result.X(), left, right);
    __ Cmp(result.X(), Operand(result, SXTW));
    DeoptimizeIf(ne, instr->environment());
  } else {
    __ Mul(result, left, right);
  }
}


void LCodeGen::DoDeferredNumberTagD(LNumberTagD* instr) {
  // TODO(3095996): Get rid of this. For now, we need to make the
  // result register contain a valid pointer because it is already
  // contained in the register pointer map.
  Register result = ToRegister(instr->result());
  __ Mov(result, 0);

  PushSafepointRegistersScope scope(this, Safepoint::kWithRegisters);
  CallRuntimeFromDeferred(Runtime::kAllocateHeapNumber, 0, instr);
  __ StoreToSafepointRegisterSlot(x0, result);
}


void LCodeGen::DoNumberTagD(LNumberTagD* instr) {
  class DeferredNumberTagD: public LDeferredCode {
   public:
    DeferredNumberTagD(LCodeGen* codegen, LNumberTagD* instr)
        : LDeferredCode(codegen), instr_(instr) { }
    virtual void Generate() { codegen()->DoDeferredNumberTagD(instr_); }
    virtual LInstruction* instr() { return instr_; }
   private:
    LNumberTagD* instr_;
  };

  DoubleRegister input = ToDoubleRegister(instr->value());
  Register result = ToRegister(instr->result());
  Register temp1 = ToRegister(instr->temp1());
  Register temp2 = ToRegister(instr->temp2());
  Label done;

  bool convert_hole = false;
  HValue* change_input = instr->hydrogen()->value();
  if (change_input->IsLoadKeyed()) {
    HLoadKeyed* load = HLoadKeyed::cast(change_input);
    convert_hole = load->UsesMustHandleHole();
  }

  if (convert_hole) {
    Label no_special_nan_handling, canonicalize;
    // TODO(jbramley): This special case does not exist in bleeding_edge.
    // * Non-NaN inputs are handled as usual.
    // * If the input is the hole, the output is the hole.
    // * If the input is any other NaN, the output is the canonical NaN.
    __ Fcmp(input, 0.0);
    __ B(vc, &no_special_nan_handling);
    __ Fmov(temp1, input);
    __ Cmp(temp1, kHoleNanInt64);
    __ B(ne, &canonicalize);
    __ Mov(result, Operand(factory()->the_hole_value()));
    __ B(&done);
    __ Bind(&canonicalize);
    // TODO(jbramley): Overwriting the input is probably a mistake, but this
    // code is removed in bleeding_edge anyway so it won't be here for long.
    TODO_UNIMPLEMENTED("DoNumberTagD: Fix NaN canonicalization logic.");
    __ Fmov(input, FixedDoubleArray::canonical_not_the_hole_nan_as_double());
    __ Bind(&no_special_nan_handling);
  }

  DeferredNumberTagD* deferred = new(zone()) DeferredNumberTagD(this, instr);
  if (FLAG_inline_new) {
    __ AllocateHeapNumber(result, deferred->entry(), temp1, temp2);
  } else {
    __ B(deferred->entry());
  }

  __ Bind(deferred->exit());
  __ Str(input, FieldMemOperand(result, HeapNumber::kValueOffset));
  __ Bind(&done);
}


void LCodeGen::DoDeferredNumberTagI(LInstruction* instr,
                                    LOperand* value,
                                    LOperand* temp1,
                                    LOperand* temp2,
                                    IntegerSignedness signedness) {
  Label slow, convert_and_store;
  Register src = ToRegister32(value);
  Register dst = ToRegister(instr->result());
  Register scratch1 = ToRegister(temp1);

  if (FLAG_inline_new) {
    Register scratch2 = ToRegister(temp2);
    __ AllocateHeapNumber(dst, &slow, scratch1, scratch2);
    __ B(&convert_and_store);
  }

  // Slow case: call the runtime system to do the number allocation.
  __ Bind(&slow);

  // Check that the dst register contains new space allocation top, which is a
  // valid address for the GC.
  if (FLAG_debug_code) {
    ExternalReference new_space_allocation_top =
        ExternalReference::new_space_allocation_top_address(isolate());
    __ Mov(scratch1, Operand(new_space_allocation_top));
    __ Ldr(scratch1, MemOperand(scratch1));
    __ Cmp(dst, scratch1);
    __ Check(eq, "Register dst does not contain allocation top.");
  }

  {
    // Preserve the value of all registers.
    PushSafepointRegistersScope scope(this, Safepoint::kWithRegisters);

    CallRuntimeFromDeferred(Runtime::kAllocateHeapNumber, 0, instr);
    __ StoreToSafepointRegisterSlot(x0, dst);
  }

  // Convert number to floating point and store in the newly allocated heap
  // number.
  __ Bind(&convert_and_store);
  DoubleRegister dbl_scratch = double_scratch();
  if (signedness == SIGNED_INT32) {
    ASM_UNIMPLEMENTED_BREAK("DeferredNumberTagI - signed int32 case.");
  } else {
    ASSERT(signedness == UNSIGNED_INT32);
    __ Ucvtf(dbl_scratch, src);
  }
  __ Str(dbl_scratch, FieldMemOperand(dst, HeapNumber::kValueOffset));
}


void LCodeGen::DoNumberTagU(LNumberTagU* instr) {
  class DeferredNumberTagU: public LDeferredCode {
   public:
    DeferredNumberTagU(LCodeGen* codegen, LNumberTagU* instr)
        : LDeferredCode(codegen), instr_(instr) { }
    virtual void Generate() {
      codegen()->DoDeferredNumberTagI(instr_,
                                      instr_->value(),
                                      instr_->temp1(),
                                      instr_->temp2(),
                                      UNSIGNED_INT32);
    }
    virtual LInstruction* instr() { return instr_; }
   private:
    LNumberTagU* instr_;
  };

  Register value = ToRegister(instr->value());
  Register result = ToRegister(instr->result());

  DeferredNumberTagU* deferred = new(zone()) DeferredNumberTagU(this, instr);
  __ Cmp(value, Smi::kMaxValue);
  __ B(hi, deferred->entry());
  __ SmiTag(result, value);
  __ Bind(deferred->exit());
}


void LCodeGen::DoNumberUntagD(LNumberUntagD* instr) {
  Register input = ToRegister(instr->value());
  Register scratch = ToRegister(instr->temp());
  DoubleRegister result = ToDoubleRegister(instr->result());
  bool allow_undefined_as_nan = instr->hydrogen()->allow_undefined_as_nan();

  Label done, load_smi;

  // Work out what untag mode we're working with.
  NumberUntagDMode mode = NUMBER_CANDIDATE_IS_ANY_TAGGED;
  HValue* value = instr->hydrogen()->value();
  if (value->type().IsSmi()) {
    mode = NUMBER_CANDIDATE_IS_SMI;
  } else if (value->IsLoadKeyed()) {
    HLoadKeyed* load = HLoadKeyed::cast(value);
    if (load->UsesMustHandleHole()) {
      if (load->hole_mode() == ALLOW_RETURN_HOLE) {
        mode = NUMBER_CANDIDATE_IS_ANY_TAGGED_CONVERT_HOLE;
      }
    }
  }

  STATIC_ASSERT(NUMBER_CANDIDATE_IS_ANY_TAGGED_CONVERT_HOLE >
                NUMBER_CANDIDATE_IS_ANY_TAGGED);
  if (mode >= NUMBER_CANDIDATE_IS_ANY_TAGGED) {
    __ JumpIfSmi(input, &load_smi);

    Label convert_undefined, deopt;

    // Heap number map check.
    Label* not_heap_number = allow_undefined_as_nan ? &convert_undefined
                                                    : &deopt;
    __ Ldr(scratch, FieldMemOperand(input, HeapObject::kMapOffset));
    __ JumpIfNotRoot(scratch, Heap::kHeapNumberMapRootIndex, not_heap_number);

    // Load heap number.
    __ Ldr(result, FieldMemOperand(input, HeapNumber::kValueOffset));
    if (instr->hydrogen()->deoptimize_on_minus_zero()) {
      ASM_UNIMPLEMENTED_BREAK("NumberUntagD - deopt on minus zero");
    }
    __ B(&done);

    if (allow_undefined_as_nan) {
      Label load_nan;

      __ Bind(&convert_undefined);
      // Convert undefined (and hole) to NaN.
      if (mode == NUMBER_CANDIDATE_IS_ANY_TAGGED_CONVERT_HOLE) {
        __ JumpIfRoot(input, Heap::kUndefinedValueRootIndex, &load_nan);
        __ JumpIfNotRoot(input, Heap::kTheHoleValueRootIndex, &deopt);
      } else {
        ASSERT(mode == NUMBER_CANDIDATE_IS_ANY_TAGGED);
        __ JumpIfNotRoot(input, Heap::kUndefinedValueRootIndex, &deopt);
      }

      __ Bind(&load_nan);
      __ LoadRoot(scratch, Heap::kNanValueRootIndex);
      __ Ldr(result, FieldMemOperand(scratch, HeapNumber::kValueOffset));
      __ B(&done);
    }

    __ Bind(&deopt);
    Deoptimize(instr->environment());
  } else {
    ASSERT(mode == NUMBER_CANDIDATE_IS_SMI);
    // Fall through to load_smi.
  }

  // Smi to double register conversion.
  __ Bind(&load_smi);
  __ SmiUntagToDouble(result, input);

  __ Bind(&done);
}


void LCodeGen::DoOsrEntry(LOsrEntry* instr) {
  // This is a pseudo-instruction that ensures that the environment here is
  // properly registered for deoptimization and records the assembler's PC
  // offset.
  LEnvironment* environment = instr->environment();

  // If the environment were already registered, we would have no way of
  // backpatching it with the spill slot operands.
  ASSERT(!environment->HasBeenRegistered());
  RegisterEnvironmentForDeoptimization(environment, Safepoint::kNoLazyDeopt);

  // Normally we record the first unknown OSR value as the entrypoint to the OSR
  // code, but if there were none, record the entrypoint here.
  if (osr_pc_offset_ == -1) osr_pc_offset_ = masm()->pc_offset();
}


void LCodeGen::DoOuterContext(LOuterContext* instr) {
  Register context = ToRegister(instr->context());
  Register result = ToRegister(instr->result());
  __ Ldr(result, ContextMemOperand(context, Context::PREVIOUS_INDEX));
}


void LCodeGen::DoParameter(LParameter* instr) {
  // Nothing to do.
}


void LCodeGen::DoPushArgument(LPushArgument* instr) {
  LOperand* argument = instr->value();
  if (argument->IsDoubleRegister() || argument->IsDoubleStackSlot()) {
    Abort("DoPushArgument not implemented for double types.");
  } else {
    __ Push(ToRegister(argument));
  }
}


void LCodeGen::DoReturn(LReturn* instr) {
  if (FLAG_trace && info()->IsOptimizing()) {
    // Push the return value on the stack as the parameter.
    // Runtime::TraceExit returns its parameter in x0.
    __ Push(x0);
    __ CallRuntime(Runtime::kTraceExit, 1);
  }

  if (info()->saves_caller_doubles()) {
    ASSERT(NeedsEagerFrame());
    BitVector* doubles = chunk()->allocated_double_registers();
    BitVector::Iterator iterator(doubles);
    int count = 0;
    while (!iterator.Done()) {
      FPRegister value = FPRegister::FromAllocationIndex(iterator.Current());
      // TODO(jbramley): Make Peek support FPRegisters.
      __ Ldr(value, MemOperand(__ StackPointer(), count * kDoubleSize));
      iterator.Advance();
      count++;
    }
  }

  int no_frame_start = -1;
  if (NeedsEagerFrame()) {
    Register stack_pointer = masm()->StackPointer();
    __ Mov(stack_pointer, fp);
    no_frame_start = masm_->pc_offset();
    __ Pop(fp, lr);
  }

  if (instr->has_constant_parameter_count()) {
    int parameter_count = ToInteger32(instr->constant_parameter_count());
    __ Drop(parameter_count + 1);
  } else {
    Register parameter_count = ToRegister(instr->parameter_count());
    __ DropBySMI(parameter_count);
  }
  __ Ret();

  if (no_frame_start != -1) {
    info_->AddNoFrameRange(no_frame_start, masm_->pc_offset());
  }
}


void LCodeGen::DoSeqStringSetChar(LSeqStringSetChar* instr) {
  String::Encoding encoding = instr->encoding();
  Register string = ToRegister(instr->string());
  Register index = ToRegister(instr->index());
  Register value = ToRegister(instr->value());
  Register temp = ToRegister(instr->temp());

  if (FLAG_debug_code) {
    __ Ldr(temp, FieldMemOperand(string, HeapObject::kMapOffset));
    __ Ldrb(temp, FieldMemOperand(temp, Map::kInstanceTypeOffset));
    __ And(temp, temp, kStringRepresentationMask | kStringEncodingMask);

    if (encoding == String::ONE_BYTE_ENCODING) {
      __ Cmp(temp, kSeqStringTag | kOneByteStringTag);
      __ Check(eq, "Unexpected string type");
    } else {
      ASSERT(encoding == String::TWO_BYTE_ENCODING);
      __ Cmp(temp, kSeqStringTag | kTwoByteStringTag);
      __ Check(eq, "Unexpected string type");
    }
  }

  __ Add(temp, string, SeqString::kHeaderSize - kHeapObjectTag);
  if (encoding == String::ONE_BYTE_ENCODING) {
    __ Strb(value, MemOperand(temp, index));
  } else {
    __ Strh(value, MemOperand(temp, index, LSL, 1));
  }
}


void LCodeGen::DoSmiTag(LSmiTag* instr) {
  ASSERT(!instr->hydrogen_value()->CheckFlag(HValue::kCanOverflow));
  __ SmiTag(ToRegister(instr->result()), ToRegister(instr->value()));
}


void LCodeGen::DoSmiUntag(LSmiUntag* instr) {
  Register input = ToRegister(instr->value());
  Register result = ToRegister(instr->result());
  Label done, untag;

  if (instr->needs_check()) {
    DeoptimizeIfNotSmi(input, instr->environment());
  }

  __ Bind(&untag);
  __ SmiUntag(result, input);
  __ Bind(&done);
}


void LCodeGen::DoShiftI(LShiftI* instr) {
  LOperand* right_op = instr->right();
  Register left = ToRegister32(instr->left());
  Register result = ToRegister32(instr->result());

  if (right_op->IsRegister()) {
    Register right = ToRegister32(instr->right());
    switch (instr->op()) {
      case Token::ROR: __ Ror(result, left, right); break;
      case Token::SAR: __ Asr(result, left, right); break;
      case Token::SHL: __ Lsl(result, left, right); break;
      case Token::SHR:
        if (instr->can_deopt()) {
          // TODO(all): Using conditional compare may be faster here, eg.
          // Deopt if (right == 0) && (left < 0).
          // __ Cmp(right, 0);
          // __ Ccmp(left, 0, NoFlag, eq);
          Label right_not_zero;
          __ Cbnz(right, &right_not_zero);
          DeoptimizeIfNegative(left, instr->environment());
          __ Bind(&right_not_zero);
        }
        __ Lsr(result, left, right);
        break;
      default: UNREACHABLE();
    }
  } else {
    ASSERT(right_op->IsConstantOperand());
    int shift_count = ToInteger32(LConstantOperand::cast(right_op)) & 0x1f;
    if (shift_count == 0) {
      if ((instr->op() == Token::SHR) && instr->can_deopt()) {
        DeoptimizeIfNegative(left, instr->environment());
      }
      __ Mov(result, left, kDiscardForSameWReg);
    } else {
      switch (instr->op()) {
        case Token::ROR: __ Ror(result, left, shift_count); break;
        case Token::SAR: __ Asr(result, left, shift_count); break;
        case Token::SHL: __ Lsl(result, left, shift_count); break;
        case Token::SHR: __ Lsr(result, left, shift_count); break;
        default: UNREACHABLE();
      }
    }
  }
}


void LCodeGen::DoDebugBreak(LDebugBreak* instr) {
  __ Debug("LDebugBreak", 0, BREAK);
}


void LCodeGen::DoDeclareGlobals(LDeclareGlobals* instr) {
  Register scratch1 = x5;
  Register scratch2 = x6;
  ASSERT(instr->IsMarkedAsCall());

  ASM_UNIMPLEMENTED_BREAK("DoDeclareGlobals");
  __ LoadHeapObject(scratch1, instr->hydrogen()->pairs());
  __ Mov(scratch2, Operand(Smi::FromInt(instr->hydrogen()->flags())));
  __ Push(cp, scratch1, scratch2);  // The context is the first argument.
  CallRuntime(Runtime::kDeclareGlobals, 3, instr);
}


void LCodeGen::DoDeferredStackCheck(LStackCheck* instr) {
  PushSafepointRegistersScope scope(this, Safepoint::kWithRegisters);
  __ CallRuntimeSaveDoubles(Runtime::kStackGuard);
  RecordSafepointWithLazyDeopt(
      instr, RECORD_SAFEPOINT_WITH_REGISTERS_AND_NO_ARGUMENTS);
  ASSERT(instr->HasEnvironment());
  LEnvironment* env = instr->environment();
  safepoints_.RecordLazyDeoptimizationIndex(env->deoptimization_index());
}


void LCodeGen::DoStackCheck(LStackCheck* instr) {
  class DeferredStackCheck: public LDeferredCode {
   public:
    DeferredStackCheck(LCodeGen* codegen, LStackCheck* instr)
        : LDeferredCode(codegen), instr_(instr) { }
    virtual void Generate() { codegen()->DoDeferredStackCheck(instr_); }
    virtual LInstruction* instr() { return instr_; }
   private:
    LStackCheck* instr_;
  };

  ASSERT(instr->HasEnvironment());
  LEnvironment* env = instr->environment();
  // There is no LLazyBailout instruction for stack-checks. We have to
  // prepare for lazy deoptimization explicitly here.
  if (instr->hydrogen()->is_function_entry()) {
    // Perform stack overflow check.
    Label done;
    __ CompareRoot(masm()->StackPointer(), Heap::kStackLimitRootIndex);
    __ B(hs, &done);

    PredictableCodeSizeScope predictable(masm_,
                                         Assembler::kCallSizeWithRelocation);
    StackCheckStub stub;
    CallCode(stub.GetCode(isolate()), RelocInfo::CODE_TARGET, instr);
    EnsureSpaceForLazyDeopt();

    __ Bind(&done);
    RegisterEnvironmentForDeoptimization(env, Safepoint::kLazyDeopt);
    safepoints_.RecordLazyDeoptimizationIndex(env->deoptimization_index());
  } else {
    ASSERT(instr->hydrogen()->is_backwards_branch());
    // Perform stack overflow check if this goto needs it before jumping.
    DeferredStackCheck* deferred_stack_check =
        new(zone()) DeferredStackCheck(this, instr);
    __ CompareRoot(masm()->StackPointer(), Heap::kStackLimitRootIndex);
    __ B(lo, deferred_stack_check->entry());

    EnsureSpaceForLazyDeopt();
    __ Bind(instr->done_label());
    deferred_stack_check->SetExit(instr->done_label());
    RegisterEnvironmentForDeoptimization(env, Safepoint::kLazyDeopt);
    // Don't record a deoptimization index for the safepoint here.
    // This will be done explicitly when emitting call and the safepoint in
    // the deferred code.
  }
}


void LCodeGen::DoStoreContextSlot(LStoreContextSlot* instr) {
  Register context = ToRegister(instr->context());
  Register value = ToRegister(instr->value());
  Register scratch = ToRegister(instr->temp());
  MemOperand target = ContextMemOperand(context, instr->slot_index());

  Label skip_assignment;

  if (instr->hydrogen()->RequiresHoleCheck()) {
    __ Ldr(scratch, target);
    if (instr->hydrogen()->DeoptimizesOnHole()) {
      DeoptimizeIfRoot(scratch, Heap::kTheHoleValueRootIndex,
                       instr->environment());
    } else {
      __ JumpIfNotRoot(scratch, Heap::kTheHoleValueRootIndex, &skip_assignment);
    }
  }

  __ Str(value, target);
  if (instr->hydrogen()->NeedsWriteBarrier()) {
    SmiCheck check_needed =
        instr->hydrogen()->value()->IsHeapObject()
            ? OMIT_SMI_CHECK : INLINE_SMI_CHECK;
    __ RecordWriteContextSlot(context,
                              target.offset(),
                              value,
                              scratch,
                              GetLinkRegisterState(),
                              kSaveFPRegs,
                              EMIT_REMEMBERED_SET,
                              check_needed);
  }
  __ Bind(&skip_assignment);
}


void LCodeGen::DoStoreGlobalCell(LStoreGlobalCell* instr) {
  Register value = ToRegister(instr->value());
  Register cell = ToRegister(instr->temp1());

  // Load the cell.
  __ Mov(cell, Operand(instr->hydrogen()->cell()));

  // If the cell we are storing to contains the hole it could have
  // been deleted from the property dictionary. In that case, we need
  // to update the property details in the property dictionary to mark
  // it as no longer deleted. We deoptimize in that case.
  if (instr->hydrogen()->RequiresHoleCheck()) {
    Register payload = ToRegister(instr->temp2());
    __ Ldr(payload, FieldMemOperand(cell, Cell::kValueOffset));
    DeoptimizeIfRoot(
        payload, Heap::kTheHoleValueRootIndex, instr->environment());
  }

  // Store the value.
  __ Str(value, FieldMemOperand(cell, Cell::kValueOffset));
  // Cells are always rescanned, so no write barrier here.
}


void LCodeGen::DoStoreGlobalGeneric(LStoreGlobalGeneric* instr) {
  ASSERT(ToRegister(instr->global_object()).Is(x1));
  ASSERT(ToRegister(instr->value()).Is(x0));

  __ Mov(x2, Operand(instr->name()));
  Handle<Code> ic = (instr->strict_mode_flag() == kStrictMode)
      ? isolate()->builtins()->StoreIC_Initialize_Strict()
      : isolate()->builtins()->StoreIC_Initialize();
  CallCode(ic, RelocInfo::CODE_TARGET_CONTEXT, instr);
}


void LCodeGen::DoStoreKeyedExternal(LStoreKeyedExternal* instr) {
  Register ext_ptr = ToRegister(instr->elements());
  Register key = no_reg;
  Register scratch;
  ElementsKind elements_kind = instr->elements_kind();

  bool key_is_smi = instr->hydrogen()->key()->representation().IsSmi();
  bool key_is_constant = instr->key()->IsConstantOperand();
  int constant_key = 0;
  if (key_is_constant) {
    ASSERT(instr->temp() == NULL);
    constant_key = ToInteger32(LConstantOperand::cast(instr->key()));
    if (constant_key & 0xf0000000) {
      Abort("Array index constant value too big.");
    }
  } else {
    key = ToRegister(instr->key());
    scratch = ToRegister(instr->temp());
  }

  int element_size_shift = ElementsKindToShiftSize(elements_kind);
  MemOperand dst =
    PrepareKeyedExternalArrayOperand(key, ext_ptr, scratch, key_is_smi,
                                     key_is_constant, constant_key,
                                     element_size_shift,
                                     instr->additional_index());

  if (elements_kind == EXTERNAL_FLOAT_ELEMENTS) {
    DoubleRegister value = ToDoubleRegister(instr->value());
    DoubleRegister dbl_scratch = double_scratch();
    __ Fcvt(dbl_scratch.S(), value);
    __ Str(dbl_scratch.S(), dst);
  } else if (elements_kind == EXTERNAL_DOUBLE_ELEMENTS) {
    DoubleRegister value = ToDoubleRegister(instr->value());
    __ Str(value, dst);
  } else {
    Register value = ToRegister(instr->value());

    switch (elements_kind) {
      case EXTERNAL_PIXEL_ELEMENTS:
      case EXTERNAL_BYTE_ELEMENTS:
      case EXTERNAL_UNSIGNED_BYTE_ELEMENTS:   __ Strb(value, dst); break;
      case EXTERNAL_SHORT_ELEMENTS:
      case EXTERNAL_UNSIGNED_SHORT_ELEMENTS:  __ Strh(value, dst); break;
      case EXTERNAL_INT_ELEMENTS:
      case EXTERNAL_UNSIGNED_INT_ELEMENTS:    __ Str(value.W(), dst); break;
      case EXTERNAL_FLOAT_ELEMENTS:
      case EXTERNAL_DOUBLE_ELEMENTS:
      case FAST_DOUBLE_ELEMENTS:
      case FAST_ELEMENTS:
      case FAST_SMI_ELEMENTS:
      case FAST_HOLEY_DOUBLE_ELEMENTS:
      case FAST_HOLEY_ELEMENTS:
      case FAST_HOLEY_SMI_ELEMENTS:
      case DICTIONARY_ELEMENTS:
      case NON_STRICT_ARGUMENTS_ELEMENTS:
        UNREACHABLE();
        break;
    }
  }
}


void LCodeGen::DoStoreKeyedFixedDouble(LStoreKeyedFixedDouble* instr) {
  Register elements = ToRegister(instr->elements());
  DoubleRegister value = ToDoubleRegister(instr->value());
  Register store_base = ToRegister(instr->temp());
  int offset = 0;

  if (instr->key()->IsConstantOperand()) {
    int constant_key = ToInteger32(LConstantOperand::cast(instr->key()));
    if (constant_key & 0xf0000000) {
      Abort("Array index constant value too big.");
    }
    offset = FixedDoubleArray::OffsetOfElementAt(constant_key +
                                                 instr->additional_index());
    store_base = elements;
  } else {
    Register key = ToRegister(instr->key());
    bool key_is_tagged = instr->hydrogen()->key()->representation().IsSmi();
    CalcKeyedArrayBaseRegister(store_base, elements, key, key_is_tagged,
                               instr->hydrogen()->elements_kind());
    offset = FixedDoubleArray::OffsetOfElementAt(instr->additional_index());
  }

  if (instr->NeedsCanonicalization()) {
    DoubleRegister dbl_scratch = double_scratch();
    __ Fmov(dbl_scratch,
            FixedDoubleArray::canonical_not_the_hole_nan_as_double());
    __ Fmaxnm(dbl_scratch, dbl_scratch, value);
    __ Str(dbl_scratch, FieldMemOperand(store_base, offset));
  } else {
    __ Str(value, FieldMemOperand(store_base, offset));
  }
}


void LCodeGen::DoStoreKeyedFixed(LStoreKeyedFixed* instr) {
  Register value = ToRegister(instr->value());
  Register elements = ToRegister(instr->elements());
  Register store_base = ToRegister(instr->temp());
  Register key = no_reg;
  int offset = 0;

  if (instr->key()->IsConstantOperand()) {
    ASSERT(!instr->hydrogen()->NeedsWriteBarrier());
    LConstantOperand* const_operand = LConstantOperand::cast(instr->key());
    offset = FixedArray::OffsetOfElementAt(ToInteger32(const_operand) +
                                           instr->additional_index());
    store_base = elements;
  } else {
    key = ToRegister(instr->key());
    bool key_is_tagged = instr->hydrogen()->key()->representation().IsSmi();
    CalcKeyedArrayBaseRegister(store_base, elements, key, key_is_tagged,
                               instr->hydrogen()->elements_kind());
    offset = FixedArray::OffsetOfElementAt(instr->additional_index());
  }
  __ Str(value, FieldMemOperand(store_base, offset));

  if (instr->hydrogen()->NeedsWriteBarrier()) {
    SmiCheck check_needed =
        instr->hydrogen()->value()->IsHeapObject()
            ? OMIT_SMI_CHECK : INLINE_SMI_CHECK;
    // Compute address of modified element and store it into key register.
    __ Add(key, store_base, offset - kHeapObjectTag);
    __ RecordWrite(elements, key, value, GetLinkRegisterState(), kSaveFPRegs,
                   EMIT_REMEMBERED_SET, check_needed);
  }
}


void LCodeGen::DoStoreKeyedGeneric(LStoreKeyedGeneric* instr) {
  ASSERT(ToRegister(instr->object()).Is(x2));
  ASSERT(ToRegister(instr->key()).Is(x1));
  ASSERT(ToRegister(instr->value()).Is(x0));

  Handle<Code> ic = (instr->strict_mode_flag() == kStrictMode)
      ? isolate()->builtins()->KeyedStoreIC_Initialize_Strict()
      : isolate()->builtins()->KeyedStoreIC_Initialize();
  CallCode(ic, RelocInfo::CODE_TARGET, instr);
}


// TODO(jbramley): Once the merge is done and we're tracking bleeding_edge, try
// to tidy up this function.
void LCodeGen::DoStoreNamedField(LStoreNamedField* instr) {
  Representation representation = instr->representation();

  Register object = ToRegister(instr->object());
  Register temp0 = ToRegister(instr->temp0());
  Register temp1 = ToRegister(instr->temp1());
  HObjectAccess access = instr->hydrogen()->access();
  int offset = access.offset();

  Handle<Map> transition = instr->transition();

  if (FLAG_track_heap_object_fields && representation.IsHeapObject()) {
    Register value = ToRegister(instr->value());
    if (!instr->hydrogen()->value()->type().IsHeapObject()) {
      DeoptimizeIfSmi(value, instr->environment());
    }
  } else if (FLAG_track_double_fields && representation.IsDouble()) {
    ASSERT(transition.is_null());
    ASSERT(access.IsInobject());
    ASSERT(!instr->hydrogen()->NeedsWriteBarrier());
    FPRegister value = ToDoubleRegister(instr->value());
    __ Str(value, FieldMemOperand(object, offset));
    return;
  }

  if (!transition.is_null()) {
    // Store the new map value.
    Register new_map_value = temp0;
    __ Mov(new_map_value, Operand(transition));
    __ Str(new_map_value, FieldMemOperand(object, HeapObject::kMapOffset));
    if (instr->hydrogen()->NeedsWriteBarrierForMap()) {
      // Update the write barrier for the map field.
      __ RecordWriteField(object,
                          HeapObject::kMapOffset,
                          new_map_value,
                          temp1,
                          GetLinkRegisterState(),
                          kSaveFPRegs,
                          OMIT_REMEMBERED_SET,
                          OMIT_SMI_CHECK);
    }
  }

  // Do the store.
  Register value = ToRegister(instr->value());
  SmiCheck check_needed =
      instr->hydrogen()->value()->IsHeapObject()
          ? OMIT_SMI_CHECK : INLINE_SMI_CHECK;
  if (access.IsInobject()) {
    __ Str(value, FieldMemOperand(object, offset));
    if (instr->hydrogen()->NeedsWriteBarrier()) {
      // Update the write barrier for the object for in-object properties.
      __ RecordWriteField(object,
                          offset,
                          value,      // Clobbered.
                          temp0,      // Clobbered.
                          GetLinkRegisterState(),
                          kSaveFPRegs,
                          EMIT_REMEMBERED_SET,
                          check_needed);
    }
  } else {
    __ Ldr(temp0, FieldMemOperand(object, JSObject::kPropertiesOffset));
    __ Str(value, FieldMemOperand(temp0, offset));
    if (instr->hydrogen()->NeedsWriteBarrier()) {
      // Update the write barrier for the properties array.
      __ RecordWriteField(temp0,
                          offset,
                          value,      // Clobbered.
                          temp1,      // Clobbered.
                          GetLinkRegisterState(),
                          kSaveFPRegs,
                          EMIT_REMEMBERED_SET,
                          check_needed);
    }
  }
}


void LCodeGen::DoStoreNamedGeneric(LStoreNamedGeneric* instr) {
  ASSERT(ToRegister(instr->value()).is(x0));
  ASSERT(ToRegister(instr->object()).is(x1));

  // Name must be in x2.
  __ Mov(x2, Operand(instr->name()));
  Handle<Code> ic = (instr->strict_mode_flag() == kStrictMode)
      ? isolate()->builtins()->StoreIC_Initialize_Strict()
      : isolate()->builtins()->StoreIC_Initialize();
  CallCode(ic, RelocInfo::CODE_TARGET, instr);
}


void LCodeGen::DoStringAdd(LStringAdd* instr) {
  Register left = ToRegister(instr->left());
  Register right = ToRegister(instr->right());
  __ Push(left, right);
  // TODO(jbramley): Once we haved rebased, use instr->hydrogen->flags() to get
  // the flags for the stub.
  StringAddStub stub(NO_STRING_CHECK_IN_STUB);
  CallCode(stub.GetCode(isolate()), RelocInfo::CODE_TARGET, instr);
}


void LCodeGen::DoStringCharCodeAt(LStringCharCodeAt* instr) {
  class DeferredStringCharCodeAt: public LDeferredCode {
   public:
    DeferredStringCharCodeAt(LCodeGen* codegen, LStringCharCodeAt* instr)
        : LDeferredCode(codegen), instr_(instr) { }
    virtual void Generate() { codegen()->DoDeferredStringCharCodeAt(instr_); }
    virtual LInstruction* instr() { return instr_; }
   private:
    LStringCharCodeAt* instr_;
  };

  DeferredStringCharCodeAt* deferred =
      new(zone()) DeferredStringCharCodeAt(this, instr);

  StringCharLoadGenerator::Generate(masm(),
                                    ToRegister(instr->string()),
                                    ToRegister(instr->index()),
                                    ToRegister(instr->result()),
                                    deferred->entry());
  __ Bind(deferred->exit());
}


void LCodeGen::DoDeferredStringCharCodeAt(LStringCharCodeAt* instr) {
  Register string = ToRegister(instr->string());
  Register result = ToRegister(instr->result());

  // TODO(3095996): Get rid of this. For now, we need to make the
  // result register contain a valid pointer because it is already
  // contained in the register pointer map.
  __ Mov(result, 0);

  PushSafepointRegistersScope scope(this, Safepoint::kWithRegisters);
  __ Push(string);
  // Push the index as a smi. This is safe because of the checks in
  // DoStringCharCodeAt above.
  Register index = ToRegister(instr->index());
  __ SmiTag(index);
  __ Push(index);

  CallRuntimeFromDeferred(Runtime::kStringCharCodeAt, 2, instr);
  __ AssertSmi(x0);
  __ SmiUntag(x0);
  __ StoreToSafepointRegisterSlot(x0, result);
}


void LCodeGen::DoStringCharFromCode(LStringCharFromCode* instr) {
  class DeferredStringCharFromCode: public LDeferredCode {
   public:
    DeferredStringCharFromCode(LCodeGen* codegen, LStringCharFromCode* instr)
        : LDeferredCode(codegen), instr_(instr) { }
    virtual void Generate() { codegen()->DoDeferredStringCharFromCode(instr_); }
    virtual LInstruction* instr() { return instr_; }
   private:
    LStringCharFromCode* instr_;
  };

  DeferredStringCharFromCode* deferred =
      new(zone()) DeferredStringCharFromCode(this, instr);

  ASSERT(instr->hydrogen()->value()->representation().IsInteger32());
  Register char_code = ToRegister(instr->char_code());
  Register result = ToRegister(instr->result());

  __ Cmp(char_code, Operand(String::kMaxOneByteCharCode));
  __ B(hi, deferred->entry());
  __ LoadRoot(result, Heap::kSingleCharacterStringCacheRootIndex);
  __ Add(result, result, Operand(char_code, LSL, kPointerSizeLog2));
  __ Ldr(result, FieldMemOperand(result, FixedArray::kHeaderSize));
  __ CompareRoot(result, Heap::kUndefinedValueRootIndex);
  __ B(eq, deferred->entry());
  __ Bind(deferred->exit());
}


void LCodeGen::DoDeferredStringCharFromCode(LStringCharFromCode* instr) {
  Register char_code = ToRegister(instr->char_code());
  Register result = ToRegister(instr->result());

  // TODO(3095996): Get rid of this. For now, we need to make the
  // result register contain a valid pointer because it is already
  // contained in the register pointer map.
  __ Mov(result, 0);

  PushSafepointRegistersScope scope(this, Safepoint::kWithRegisters);
  __ SmiTag(char_code);
  __ Push(char_code);
  CallRuntimeFromDeferred(Runtime::kCharFromCode, 1, instr);
  __ StoreToSafepointRegisterSlot(x0, result);
}


void LCodeGen::DoStringCompareAndBranch(LStringCompareAndBranch* instr) {
  Token::Value op = instr->op();

  Handle<Code> ic = CompareIC::GetUninitialized(isolate(), op);
  CallCode(ic, RelocInfo::CODE_TARGET, instr);
  InlineSmiCheckInfo::EmitNotInlined(masm());

  Condition condition = TokenToCondition(op, false);

  EmitBranch(instr, condition);
}


void LCodeGen::DoStringLength(LStringLength* instr) {
  Register string = ToRegister(instr->string());
  Register result = ToRegister(instr->result());
  __ Ldr(result, FieldMemOperand(string, String::kLengthOffset));
}


void LCodeGen::DoSubI(LSubI* instr) {
  bool can_overflow = instr->hydrogen()->CheckFlag(HValue::kCanOverflow);
  Register result = ToRegister32(instr->result());
  Register left = ToRegister32(instr->left());
  Operand right = ToOperand32(instr->right());
  if (can_overflow) {
    __ Subs(result, left, right);
    DeoptimizeIf(vs, instr->environment());
  } else {
    __ Sub(result, left, right);
  }
}


void LCodeGen::DoDeferredTaggedToI(LTaggedToI* instr,
                                   LOperand* value,
                                   LOperand* temp1,
                                   LOperand* temp2) {
  Register input = ToRegister(value);
  Register scratch1 = ToRegister(temp1);
  DoubleRegister dbl_scratch1 = double_scratch();

  Label done;

  // Load heap object map.
  __ Ldr(scratch1, FieldMemOperand(input, HeapObject::kMapOffset));

  if (instr->truncating()) {
    Register output = ToRegister(instr->result());
    Register scratch2 = ToRegister(temp2);
    Label undefined;

    // If it's not a heap number, jump to undefined check.
    __ JumpIfNotRoot(scratch1, Heap::kHeapNumberMapRootIndex, &undefined);

    // A heap number: load value and convert to int32 using truncating function.
    __ Ldr(dbl_scratch1, FieldMemOperand(input, HeapNumber::kValueOffset));
    __ ECMA262ToInt32(output, dbl_scratch1, scratch1, scratch2);
    __ B(&done);

    // Check for undefined. Undefined is converted to zero for truncating
    // conversions.
    __ Bind(&undefined);

    DeoptimizeIfNotRoot(input, Heap::kUndefinedValueRootIndex,
                        instr->environment());
    __ Mov(output, 0);
  } else {
    Register output = ToRegister32(instr->result());

    DoubleRegister dbl_scratch2 = ToDoubleRegister(temp2);
    Label converted;

    // Deoptimized if it's not a heap number.
    DeoptimizeIfNotRoot(scratch1, Heap::kHeapNumberMapRootIndex,
                        instr->environment());

    // A heap number: load value and convert to int32 using non-truncating
    // function. If the result is out of range, branch to deoptimize.
    __ Ldr(dbl_scratch1, FieldMemOperand(input, HeapNumber::kValueOffset));
    __ TryConvertDoubleToInt32(output, dbl_scratch1, dbl_scratch2, &converted);
    Deoptimize(instr->environment());

    __ Bind(&converted);

    if (instr->hydrogen()->CheckFlag(HValue::kBailoutOnMinusZero)) {
      __ Cmp(output, 0);
      __ B(ne, &done);
      __ Fmov(scratch1, dbl_scratch1);
      DeoptimizeIfNegative(scratch1, instr->environment());
    }
  }
  __ Bind(&done);
}


void LCodeGen::DoTaggedToI(LTaggedToI* instr) {
  class DeferredTaggedToI: public LDeferredCode {
   public:
    DeferredTaggedToI(LCodeGen* codegen, LTaggedToI* instr)
        : LDeferredCode(codegen), instr_(instr) { }
    virtual void Generate() {
      codegen()->DoDeferredTaggedToI(instr_, instr_->value(), instr_->temp1(),
                                     instr_->temp2());
    }

    virtual LInstruction* instr() { return instr_; }
   private:
    LTaggedToI* instr_;
  };

  Register input = ToRegister(instr->value());
  Register output = ToRegister(instr->result());

  DeferredTaggedToI* deferred = new(zone()) DeferredTaggedToI(this, instr);

  // TODO(jbramley): We can't use JumpIfNotSmi here because the tbz it uses
  // doesn't always have enough range. Consider making a variant of it, or a
  // TestIsSmi helper.
  STATIC_ASSERT(kSmiTag == 0);
  __ Tst(input, kSmiTagMask);
  __ B(ne, deferred->entry());

  __ SmiUntag(output, input);
  __ Bind(deferred->exit());
}


void LCodeGen::DoThisFunction(LThisFunction* instr) {
  Register result = ToRegister(instr->result());
  __ Ldr(result, MemOperand(fp, JavaScriptFrameConstants::kFunctionOffset));
}


void LCodeGen::DoToFastProperties(LToFastProperties* instr) {
  ASSERT(ToRegister(instr->value()).Is(x0));
  ASSERT(ToRegister(instr->result()).Is(x0));
  ASM_UNIMPLEMENTED_BREAK("DoToFastProperties");
  __ Push(x0);
  CallRuntime(Runtime::kToFastProperties, 1, instr);
}


void LCodeGen::DoRegExpLiteral(LRegExpLiteral* instr) {
  Label materialized;
  // Registers will be used as follows:
  // x7 = literals array.
  // x1 = regexp literal.
  // x0 = regexp literal clone.
  // x10-x12 are used as temporaries.
  int literal_offset =
      FixedArray::OffsetOfElementAt(instr->hydrogen()->literal_index());
  __ LoadHeapObject(x7, instr->hydrogen()->literals());
  __ Ldr(x1, FieldMemOperand(x7, literal_offset));
  __ JumpIfNotRoot(x1, Heap::kUndefinedValueRootIndex, &materialized);

  // Create regexp literal using runtime function
  // Result will be in x0.
  __ Mov(x12, Operand(Smi::FromInt(instr->hydrogen()->literal_index())));
  __ Mov(x11, Operand(instr->hydrogen()->pattern()));
  __ Mov(x10, Operand(instr->hydrogen()->flags()));
  __ Push(x7, x12, x11, x10);
  CallRuntime(Runtime::kMaterializeRegExpLiteral, 4, instr);
  __ Mov(x1, x0);

  __ Bind(&materialized);
  int size = JSRegExp::kSize + JSRegExp::kInObjectFieldCount * kPointerSize;
  Label allocated, runtime_allocate;

  __ Allocate(size, x0, x10, x11, &runtime_allocate, TAG_OBJECT);
  __ B(&allocated);

  __ Bind(&runtime_allocate);
  __ Mov(x0, Operand(Smi::FromInt(size)));
  __ Push(x1, x0);
  CallRuntime(Runtime::kAllocateInNewSpace, 1, instr);
  __ Pop(x1);

  __ Bind(&allocated);
  // Copy the content into the newly allocated memory.
  __ CopyFields(x0, x1, CPURegList(x10, x11, x12), size / kPointerSize);
}


void LCodeGen::DoThrow(LThrow* instr) {
  Register value = ToRegister(instr->value());
  __ Push(value);
  CallRuntime(Runtime::kThrow, 1, instr);

  if (FLAG_debug_code) {
    __ Abort("Unreachable code in Throw.");
  }
}


void LCodeGen::DoTransitionElementsKind(LTransitionElementsKind* instr) {
  Register object = ToRegister(instr->object());

  Handle<Map> from_map = instr->original_map();
  Handle<Map> to_map = instr->transitioned_map();
  ElementsKind from_kind = instr->from_kind();
  ElementsKind to_kind = instr->to_kind();

  Register scratch;
  if (IsSimpleMapChangeTransition(from_kind, to_kind)) {
    scratch = ToRegister(instr->temp1());
  } else {
    ASSERT(FLAG_compiled_transitions || instr->IsMarkedAsCall());
    scratch = x10;
  }

  Label not_applicable;
  __ CompareMap(object, scratch, from_map);
  __ B(ne, &not_applicable);

  if (IsSimpleMapChangeTransition(from_kind, to_kind)) {
    Register new_map = ToRegister(instr->temp2());
    __ Mov(new_map, Operand(to_map));
    __ Str(new_map, FieldMemOperand(object, HeapObject::kMapOffset));
    // Write barrier.
    __ RecordWriteField(object, HeapObject::kMapOffset, new_map, scratch,
                        GetLinkRegisterState(), kDontSaveFPRegs);
  } else if (FLAG_compiled_transitions) {
    PushSafepointRegistersScope scope(this, Safepoint::kWithRegisters);
    __ Mov(x0, object);
    __ Mov(x1, Operand(to_map));
    TransitionElementsKindStub stub(from_kind, to_kind);
    __ CallStub(&stub);
    RecordSafepointWithRegisters(
        instr->pointer_map(), 0, Safepoint::kNoLazyDeopt);
  } else if ((IsFastSmiElementsKind(from_kind) &&
              IsFastDoubleElementsKind(to_kind)) ||
             (IsFastDoubleElementsKind(from_kind) &&
              IsFastObjectElementsKind(to_kind))) {
    ASSERT((instr->temp1() == NULL) && (instr->temp2() == NULL));
    __ Mov(x2, object);
    __ Mov(x3, Operand(to_map));
    if (IsFastSmiElementsKind(from_kind)) {
      CallCode(isolate()->builtins()->TransitionElementsSmiToDouble(),
               RelocInfo::CODE_TARGET, instr);
    } else if (IsFastDoubleElementsKind(from_kind)) {
      CallCode(isolate()->builtins()->TransitionElementsDoubleToObject(),
               RelocInfo::CODE_TARGET, instr);
    }
  } else {
    UNREACHABLE();
  }
  __ Bind(&not_applicable);
}


void LCodeGen::DoTrapAllocationMemento(LTrapAllocationMemento* instr) {
  Register object = ToRegister(instr->object());
  Register temp1 = ToRegister(instr->temp1());
  Register temp2 = ToRegister(instr->temp2());
  __ TestJSArrayForAllocationSiteInfo(object, temp1, temp2);
  DeoptimizeIf(eq, instr->environment());
}


void LCodeGen::DoTypeof(LTypeof* instr) {
  Register input = ToRegister(instr->value());
  __ Push(input);
  CallRuntime(Runtime::kTypeof, 1, instr);
}


void LCodeGen::DoTypeofIsAndBranch(LTypeofIsAndBranch* instr) {
  Handle<String> type_name = instr->type_literal();
  Label* true_label = instr->TrueLabel(chunk_);
  Label* false_label = instr->FalseLabel(chunk_);
  Register value = ToRegister(instr->value());

  if (type_name->Equals(heap()->number_string())) {
    ASSERT(instr->temp1() != NULL);
    Register map = ToRegister(instr->temp1());

    __ JumpIfSmi(value, true_label);
    __ Ldr(map, FieldMemOperand(value, HeapObject::kMapOffset));
    __ CompareRoot(map, Heap::kHeapNumberMapRootIndex);
    EmitBranch(instr, eq);

  } else if (type_name->Equals(heap()->string_string())) {
    ASSERT((instr->temp1() != NULL) && (instr->temp2() != NULL));
    Register map = ToRegister(instr->temp1());
    Register scratch = ToRegister(instr->temp2());

    __ JumpIfSmi(value, false_label);
    __ JumpIfObjectType(
        value, map, scratch, FIRST_NONSTRING_TYPE, false_label, ge);
    __ Ldrb(scratch, FieldMemOperand(map, Map::kBitFieldOffset));
    EmitTestAndBranch(instr, eq, scratch, 1 << Map::kIsUndetectable);

  } else if (type_name->Equals(heap()->symbol_string())) {
    ASSERT((instr->temp1() != NULL) && (instr->temp2() != NULL));
    Register map = ToRegister(instr->temp1());
    Register scratch = ToRegister(instr->temp2());

    __ JumpIfSmi(value, false_label);
    __ CompareObjectType(value, map, scratch, SYMBOL_TYPE);
    EmitBranch(instr, eq);

  } else if (type_name->Equals(heap()->boolean_string())) {
    __ JumpIfRoot(value, Heap::kTrueValueRootIndex, true_label);
    __ CompareRoot(value, Heap::kFalseValueRootIndex);
    EmitBranch(instr, eq);

  } else if (FLAG_harmony_typeof && type_name->Equals(heap()->null_string())) {
    __ CompareRoot(value, Heap::kNullValueRootIndex);
    EmitBranch(instr, eq);

  } else if (type_name->Equals(heap()->undefined_string())) {
    ASSERT(instr->temp1() != NULL);
    Register scratch = ToRegister(instr->temp1());

    __ JumpIfRoot(value, Heap::kUndefinedValueRootIndex, true_label);
    __ JumpIfSmi(value, false_label);
    // Check for undetectable objects and jump to the true branch in this case.
    __ Ldr(scratch, FieldMemOperand(value, HeapObject::kMapOffset));
    __ Ldrb(scratch, FieldMemOperand(scratch, Map::kBitFieldOffset));
    EmitTestAndBranch(instr, ne, scratch, 1 << Map::kIsUndetectable);

  } else if (type_name->Equals(heap()->function_string())) {
    STATIC_ASSERT(NUM_OF_CALLABLE_SPEC_OBJECT_TYPES == 2);
    ASSERT(instr->temp1() != NULL);
    Register type = ToRegister(instr->temp1());

    __ JumpIfSmi(value, false_label);
    __ JumpIfObjectType(value, type, type, JS_FUNCTION_TYPE, true_label);
    // HeapObject's type has been loaded into type register by JumpIfObjectType.
    EmitCompareAndBranch(instr, eq, type, JS_FUNCTION_PROXY_TYPE);

  } else if (type_name->Equals(heap()->object_string())) {
    ASSERT((instr->temp1() != NULL) && (instr->temp2() != NULL));
    Register map = ToRegister(instr->temp1());
    Register scratch = ToRegister(instr->temp2());

    __ JumpIfSmi(value, false_label);
    if (!FLAG_harmony_typeof) {
      __ JumpIfRoot(value, Heap::kNullValueRootIndex, true_label);
    }
    __ JumpIfObjectType(value, map, scratch,
                        FIRST_NONCALLABLE_SPEC_OBJECT_TYPE, false_label, lt);
    __ CompareInstanceType(map, scratch, LAST_NONCALLABLE_SPEC_OBJECT_TYPE);
    __ B(gt, false_label);
    // Check for undetectable objects => false.
    __ Ldrb(scratch, FieldMemOperand(value, Map::kBitFieldOffset));
    EmitTestAndBranch(instr, eq, scratch, 1 << Map::kIsUndetectable);

  } else {
    __ B(false_label);
  }
}


void LCodeGen::DoUint32ToDouble(LUint32ToDouble* instr) {
  __ Ucvtf(ToDoubleRegister(instr->result()), ToRegister32(instr->value()));
}


void LCodeGen::DoValueOf(LValueOf* instr) {
  Register input = ToRegister(instr->value());
  Register result = ToRegister(instr->result());
  Register scratch = ToRegister(instr->temp());
  Label done;

  ASSERT(input.Is(result));

  if (!instr->hydrogen()->value()->IsHeapObject()) {
    // If the object is a smi return it.
    __ JumpIfSmi(input, &done);
  }

  // If the object is not a value type, return the object, otherwise
  // return the value.
  __ JumpIfNotObjectType(input, scratch, scratch, JS_VALUE_TYPE, &done);
  __ Ldr(result, FieldMemOperand(input, JSValue::kValueOffset));

  __ Bind(&done);
}


void LCodeGen::DoCheckMapValue(LCheckMapValue* instr) {
  Register object = ToRegister(instr->value());
  Register map = ToRegister(instr->map());
  Register temp = ToRegister(instr->temp());
  __ Ldr(temp, FieldMemOperand(object, HeapObject::kMapOffset));
  __ Cmp(map, temp);
  DeoptimizeIf(ne, instr->environment());
}


void LCodeGen::DoWrapReceiver(LWrapReceiver* instr) {
  Register receiver = ToRegister(instr->receiver());
  Register function = ToRegister(instr->function());
  Register result = ToRegister(instr->result());
  Register temp = ToRegister(instr->temp());

  // If the receiver is null or undefined, we have to pass the global object as
  // a receiver to normal functions. Values have to be passed unchanged to
  // builtins and strict-mode functions.
  Label global_object, done, deopt;

  __ Ldr(temp, FieldMemOperand(function,
                               JSFunction::kSharedFunctionInfoOffset));

  // CompilerHints is an int32 field. See objects.h.
  __ Ldr(temp.W(),
         FieldMemOperand(temp, SharedFunctionInfo::kCompilerHintsOffset));

  // Do not transform the receiver to object for strict mode functions.
  __ Tbnz(temp, SharedFunctionInfo::kStrictModeFunction, &done);

  // Do not transform the receiver to object for builtins.
  __ Tbnz(temp, SharedFunctionInfo::kNative, &done);

  // Normal function. Replace undefined or null with global receiver.
  __ JumpIfRoot(receiver, Heap::kNullValueRootIndex, &global_object);
  __ JumpIfRoot(receiver, Heap::kUndefinedValueRootIndex, &global_object);

  // Deoptimize if the receiver is not a JS object.
  __ JumpIfSmi(receiver, &deopt);
  __ CompareObjectType(receiver, temp, temp, FIRST_SPEC_OBJECT_TYPE);
  __ B(ge, &done);
  // Otherwise, fall through to deopt.

  __ Bind(&deopt);
  Deoptimize(instr->environment());

  __ Bind(&global_object);
  // We could load directly into the result register here, but the additional
  // branches required are likely to be more time consuming than one additional
  // move.
  __ Ldr(receiver, GlobalObjectMemOperand());
  __ Ldr(receiver, FieldMemOperand(receiver,
                                   JSGlobalObject::kGlobalReceiverOffset));
  __ Bind(&done);

  __ Mov(result, receiver);
}


void LCodeGen::DoLoadFieldByIndex(LLoadFieldByIndex* instr) {
  Register object = ToRegister(instr->object());
  Register index = ToRegister(instr->index());
  Register result = ToRegister(instr->result());

  __ AssertSmi(index);

  Label out_of_object, done;
  __ Cmp(index, Operand(Smi::FromInt(0)));
  __ B(lt, &out_of_object);

  STATIC_ASSERT(kPointerSizeLog2 > kSmiTagSize);
  __ Add(result, object, Operand::UntagSmiAndScale(index, kPointerSizeLog2));
  __ Ldr(result, FieldMemOperand(result, JSObject::kHeaderSize));

  __ B(&done);

  __ Bind(&out_of_object);
  __ Ldr(result, FieldMemOperand(object, JSObject::kPropertiesOffset));
  // Index is equal to negated out of object property index plus 1.
  __ Sub(result, result, Operand::UntagSmiAndScale(index, kPointerSizeLog2));
  __ Ldr(result, FieldMemOperand(result,
                                 FixedArray::kHeaderSize - kPointerSize));
  __ Bind(&done);
}

} }  // namespace v8::internal
