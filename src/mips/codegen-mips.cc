// Copyright 2010 the V8 project authors. All rights reserved.
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

#include "bootstrapper.h"
#include "codegen-inl.h"
#include "compiler.h"
#include "debug.h"
#include "ic-inl.h"
#include "parser.h"
#include "register-allocator-inl.h"
#include "runtime.h"
#include "scopes.h"
#include "virtual-frame-inl.h"


namespace v8 {
namespace internal {

#define __ ACCESS_MASM(masm_)

static void EmitIdenticalObjectComparison(MacroAssembler* masm,
                                          Label* slow,
                                          Condition cc,
                                          bool never_nan_nan);
static void EmitSmiNonsmiComparison(MacroAssembler* masm,
                                    Label* rhs_not_nan,
                                    Label* slow,
                                    bool strict);
static void EmitTwoNonNanDoubleComparison(MacroAssembler* masm, Condition cc);
static void EmitStrictTwoHeapObjectCompare(MacroAssembler* masm);
static void MultiplyByKnownInt(MacroAssembler* masm,
                               Register source,
                               Register destination,
                               int known_int);
static bool IsEasyToMultiplyBy(int x);


// -----------------------------------------------------------------------------
// Platform-specific DeferredCode functions.


void DeferredCode::SaveRegisters() {
  for (int i = 0; i < RegisterAllocator::kNumRegisters; i++) {
    int action = registers_[i];
    if (action == kPush) {
      __ Push(RegisterAllocator::ToRegister(i));
    } else if (action != kIgnore && (action & kSyncedFlag) == 0) {
      __ sw(RegisterAllocator::ToRegister(i), MemOperand(fp, action));
    }
  }
}


void DeferredCode::RestoreRegisters() {
  // Restore registers in reverse order due to the stack.
  for (int i = RegisterAllocator::kNumRegisters - 1; i >= 0; i--) {
    int action = registers_[i];
    if (action == kPush) {
      __ Pop(RegisterAllocator::ToRegister(i));
    } else if (action != kIgnore) {
      action &= ~kSyncedFlag;
      __ lw(RegisterAllocator::ToRegister(i), MemOperand(fp, action));
    }
  }
}


// -----------------------------------------------------------------------------
// CodeGenState implementation.

CodeGenState::CodeGenState(CodeGenerator* owner)
    : owner_(owner),
      true_target_(NULL),
      false_target_(NULL),
      previous_(NULL) {
  owner_->set_state(this);
}


CodeGenState::CodeGenState(CodeGenerator* owner,
                           JumpTarget* true_target,
                           JumpTarget* false_target)
    : owner_(owner),
      true_target_(true_target),
      false_target_(false_target),
      previous_(owner->state()) {
  owner_->set_state(this);
}


CodeGenState::~CodeGenState() {
  ASSERT(owner_->state() == this);
  owner_->set_state(previous_);
}


// -----------------------------------------------------------------------------
// CodeGenerator implementation.

CodeGenerator::CodeGenerator(MacroAssembler* masm)
    : deferred_(8),
      masm_(masm),
      frame_(NULL),
      allocator_(NULL),
      cc_reg_(cc_always),
      state_(NULL),
      function_return_is_shadowed_(false) {
}


// Calling conventions:
// fp: caller's frame pointer
// sp: stack pointer
// a1: called JS function
// cp: callee's context

void CodeGenerator::Generate(CompilationInfo* info) {
  // Record the position for debugging purposes.
  CodeForFunctionPosition(info->function());

  // Initialize state.
  info_ = info;
  ASSERT(allocator_ == NULL);
  RegisterAllocator register_allocator(this);
  allocator_ = &register_allocator;
  ASSERT(frame_ == NULL);
  frame_ = new VirtualFrame();
  cc_reg_ = cc_always;

  {
    CodeGenState state(this);

    // Registers:
    // a1: called JS function
    // ra: return address
    // fp: caller's frame pointer
    // sp: stack pointer
    // cp: callee's context
    //
    // Stack:
    // arguments
    // receiver

#ifdef DEBUG
    if (strlen(FLAG_stop_at) > 0 &&
        info->function()->name()->IsEqualTo(CStrVector(FLAG_stop_at))) {
      frame_->SpillAll();
      __ stop("stop-at");
    }
#endif

    frame_->Enter();

    // Allocate space for locals and initialize them.
    frame_->AllocateStackSlots();

    VirtualFrame::SpilledScope spilled_scope;
    int heap_slots = scope()->num_heap_slots();
    if (heap_slots > 0) {
      // Allocate local context.
      // Get outer context and create a new context based on it.
      __ lw(a0, frame_->Function());
      frame_->EmitPush(a0);
      frame_->CallRuntime(Runtime::kNewContext, 1);  // v0 holds the result

#ifdef DEBUG
      JumpTarget verified_true;
      verified_true.Branch(eq, v0, Operand(cp), no_hint);
      __ stop("NewContext: v0 is expected to be the same as cp");
      verified_true.Bind();
#endif
      // Update context local.
      __ sw(cp, frame_->Context());
    }

    {
      Comment cmnt2(masm_, "[ copy context parameters into .context");

      // Note that iteration order is relevant here! If we have the same
      // parameter twice (e.g., function (x, y, x)), and that parameter
      // needs to be copied into the context, it must be the last argument
      // passed to the parameter that needs to be copied. This is a rare
      // case so we don't check for it, instead we rely on the copying
      // order: such a parameter is copied repeatedly into the same
      // context location and thus the last value is what is seen inside
      // the function.
      for (int i = 0; i < scope()->num_parameters(); i++) {
        Variable* par = scope()->parameter(i);
        Slot* slot = par->slot();
        if (slot != NULL && slot->type() == Slot::CONTEXT) {
          ASSERT(!scope()->is_global_scope());  // no parameters in global scope
          __ lw(a1, frame_->ParameterAt(i));
          // Loads a2 with context; used below in RecordWrite.
          __ sw(a1, SlotOperand(slot, a2));
          // Load the offset into a3.
          int slot_offset =
              FixedArray::kHeaderSize + slot->index() * kPointerSize;
          __ li(a3, Operand(slot_offset));
          __ RecordWrite(a2, a3, a1);
        }
      }
    }

    // Store the arguments object.  This must happen after context
    // initialization because the arguments object may be stored in the
    // context.
    if (scope()->arguments() != NULL) {
        Comment cmnt(masm_, "[ allocate arguments object");
        ASSERT(scope()->arguments_shadow() != NULL);
        Variable* arguments = scope()->arguments()->var();
        Variable* shadow = scope()->arguments_shadow()->var();
        ASSERT(arguments != NULL && arguments->slot() != NULL);
        ASSERT(shadow != NULL && shadow->slot() != NULL);
        ArgumentsAccessStub stub(ArgumentsAccessStub::NEW_OBJECT);
        __ lw(a2, frame_->Function());
        // The receiver is below the arguments, the return address, and the
        // frame pointer on the stack.
        const int kReceiverDisplacement = 2 + scope()->num_parameters();
        __ Add(a1, fp, Operand(kReceiverDisplacement * kPointerSize));
        __ li(a0, Operand(Smi::FromInt(scope()->num_parameters())));
        frame_->Adjust(3);
        __ MultiPush(a0.bit() | a1.bit() | a2.bit());
        frame_->CallStub(&stub, 3);
        frame_->EmitPush(v0);
        StoreToSlot(arguments->slot(), NOT_CONST_INIT);
        StoreToSlot(shadow->slot(), NOT_CONST_INIT);
        frame_->Drop();  // Value is no longer needed.
    }

    // Initialize ThisFunction reference if present.
    if (scope()->is_function_scope() && scope()->function() != NULL) {
      __ li(t0, Operand(Factory::the_hole_value()));
      frame_->EmitPush(t0);
      StoreToSlot(scope()->function()->slot(), NOT_CONST_INIT);
    }

    // Initialize the function return target after the locals are set
    // up, because it needs the expected frame height from the frame.
    function_return_.set_direction(JumpTarget::BIDIRECTIONAL);
    function_return_is_shadowed_ = false;

    // Generate code to 'execute' declarations and initialize functions
    // (source elements). In case of an illegal redeclaration we need to
    // handle that instead of processing the declarations.
    if (scope()->HasIllegalRedeclaration()) {
      Comment cmnt(masm_, "[ illegal redeclarations");
      scope()->VisitIllegalRedeclaration(this);
    } else {
      Comment cmnt(masm_, "[ declarations");
      ProcessDeclarations(scope()->declarations());
      // Bail out if a stack-overflow exception occurred when processing
      // declarations.
      if (HasStackOverflow()) return;
    }

    if (FLAG_trace) {
      frame_->CallRuntime(Runtime::kTraceEnter, 0);
      // Ignore the return value.
    }

    // Compile the body of the function in a vanilla state. Don't
    // bother compiling all the code if the scope has an illegal
    // redeclaration.
    if (!scope()->HasIllegalRedeclaration()) {
      Comment cmnt(masm_, "[ function body");
#ifdef DEBUG
      bool is_builtin = Bootstrapper::IsActive();
      bool should_trace =
          is_builtin ? FLAG_trace_builtin_calls : FLAG_trace_calls;
      if (should_trace) {
        frame_->CallRuntime(Runtime::kDebugTrace, 0);
        // Ignore the return value.
      }
#endif
      VisitStatementsAndSpill(info->function()->body());
    }
  }

  if (has_valid_frame() || function_return_.is_linked()) {
    if (!function_return_.is_linked()) {
      CodeForReturnPosition(info->function());
    }
    // Registers:
    // v0: result
    // sp: stack pointer
    // fp: frame pointer
    // cp: callee's context

    __ LoadRoot(v0, Heap::kUndefinedValueRootIndex);

    function_return_.Bind();
    if (FLAG_trace) {
      // Push the return value on the stack as the parameter.
      // Runtime::TraceExit returns the parameter as it is.
      frame_->EmitPush(v0);
      frame_->CallRuntime(Runtime::kTraceExit, 1);
    }

    // We don't check for the return code size. It may differ if the number of
    // arguments is too big.
    __ mov(sp, fp);
    __ lw(fp, MemOperand(sp, 0));
    __ lw(ra, MemOperand(sp, 4));
    __ addiu(sp, sp, 8);

    __ Addu(sp, sp, Operand((scope()->num_parameters() + 1) * kPointerSize));
    __ Ret();
  }

  // Code generation state must be reset.
  ASSERT(!has_cc());
  ASSERT(state_ == NULL);
  ASSERT(!function_return_is_shadowed_);
  function_return_.Unuse();
  DeleteFrame();

  // Process any deferred code using the register allocator.
  if (!HasStackOverflow()) {
    ProcessDeferred();
  }

  allocator_ = NULL;
}


void CodeGenerator::LoadReference(Reference* ref) {
  VirtualFrame::SpilledScope spilled_scope;
  Comment cmnt(masm_, "[ LoadReference");
  Expression* e = ref->expression();
  Property* property = e->AsProperty();
  Variable* var = e->AsVariableProxy()->AsVariable();

  if (property != NULL) {
    // The expression is either a property or a variable proxy that rewrites
    // to a property.
    LoadAndSpill(property->obj());
    if (property->key()->IsPropertyName()) {
      ref->set_type(Reference::NAMED);
    } else {
      LoadAndSpill(property->key());
      ref->set_type(Reference::KEYED);
    }
  } else if (var != NULL) {
    // The expression is a variable proxy that does not rewrite to a
    // property.  Global variables are treated as named property references.
    if (var->is_global()) {
      LoadGlobal();
      ref->set_type(Reference::NAMED);
    } else {
      ASSERT(var->slot() != NULL);
      ref->set_type(Reference::SLOT);
    }
  } else {
    UNIMPLEMENTED_MIPS();
      __ break_(__LINE__);
  }
}


void CodeGenerator::UnloadReference(Reference* ref) {
  VirtualFrame::SpilledScope spilled_scope;
  // Pop a reference from the stack while preserving TOS.
  Comment cmnt(masm_, "[ UnloadReference");
  int size = ref->size();
  if (size > 0) {
    frame_->EmitPop(a0);
    frame_->Drop(size);
    frame_->EmitPush(a0);
  }
  ref->set_unloaded();
}


MemOperand CodeGenerator::SlotOperand(Slot* slot, Register tmp) {
  // Currently, this assertion will fail if we try to assign to
  // a constant variable that is constant because it is read-only
  // (such as the variable referring to a named function expression).
  // We need to implement assignments to read-only variables.
  // Ideally, we should do this during AST generation (by converting
  // such assignments into expression statements); however, in general
  // we may not be able to make the decision until past AST generation,
  // that is when the entire program is known.
  ASSERT(slot != NULL);
  int index = slot->index();
  switch (slot->type()) {
    case Slot::PARAMETER:
      return frame_->ParameterAt(index);

    case Slot::LOCAL:
      return frame_->LocalAt(index);

    case Slot::CONTEXT: {
      ASSERT(!tmp.is(cp));  // Do not overwrite context register.
      Register context = cp;
      int chain_length = scope()->ContextChainLength(slot->var()->scope());
      for (int i = 0; i < chain_length; i++) {
        // Load the closure.
        // (All contexts, even 'with' contexts, have a closure,
        // and it is the same for all contexts inside a function.
        // There is no need to go to the function context first.)
        __ lw(tmp, ContextOperand(context, Context::CLOSURE_INDEX));
        // Load the function context (which is the incoming, outer context).
        __ lw(tmp, FieldMemOperand(tmp, JSFunction::kContextOffset));
        context = tmp;
      }
      // We may have a 'with' context now. Get the function context.
      // (In fact this mov may never be the needed, since the scope analysis
      // may not permit a direct context access in this case and thus we are
      // always at a function context. However it is safe to dereference be-
      // cause the function context of a function context is itself. Before
      // deleting this mov we should try to create a counter-example first,
      // though...)
      __ lw(tmp, ContextOperand(context, Context::FCONTEXT_INDEX));
      return ContextOperand(tmp, index);
    }

    default:
      UNREACHABLE();
      return MemOperand(no_reg, 0);
  }
}


// Loads a value on TOS. If it is a boolean value, the result may have been
// (partially) translated into branches, or it may have set the condition
// code register. If force_cc is set, the value is forced to set the
// condition code register and no value is pushed. If the condition code
// register was set, has_cc() is true and cc_reg_ contains the condition to
// test for 'true'.
void CodeGenerator::LoadCondition(Expression* x,
                                  JumpTarget* true_target,
                                  JumpTarget* false_target,
                                  bool force_cc) {
  ASSERT(!has_cc());
  int original_height = frame_->height();

  { CodeGenState new_state(this, true_target, false_target);
    Visit(x);

    // If we hit a stack overflow, we may not have actually visited
    // the expression. In that case, we ensure that we have a
    // valid-looking frame state because we will continue to generate
    // code as we unwind the C++ stack.
    //
    // It's possible to have both a stack overflow and a valid frame
    // state (eg, a subexpression overflowed, visiting it returned
    // with a dummied frame state, and visiting this expression
    // returned with a normal-looking state).
    if (HasStackOverflow() &&
        has_valid_frame() &&
        !has_cc() &&
        frame_->height() == original_height) {
      true_target->Jump();
    }
  }
  if (force_cc && frame_ != NULL && !has_cc()) {
    // Convert the TOS value to a boolean in the condition code register.
    ToBoolean(true_target, false_target);
  }
  ASSERT(!force_cc || !has_valid_frame() || has_cc());
  ASSERT(!has_valid_frame() ||
         (has_cc() && frame_->height() == original_height) ||
         (!has_cc() && frame_->height() == original_height + 1));
}


void CodeGenerator::Load(Expression* x) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  JumpTarget true_target;
  JumpTarget false_target;
  LoadCondition(x, &true_target, &false_target, false);

  if (has_cc()) {
    // Convert cc_reg_ into a boolean value.
    JumpTarget loaded;
    JumpTarget materialize_true;

    materialize_true.Branch(cc_reg_, condReg1, Operand(condReg2));
    __ LoadRoot(v0, Heap::kFalseValueRootIndex);
    frame_->EmitPush(v0);
    loaded.Jump();
    materialize_true.Bind();
    __ LoadRoot(v0, Heap::kTrueValueRootIndex);
    frame_->EmitPush(v0);
    loaded.Bind();
    cc_reg_ = cc_always;
  }

  if (true_target.is_linked() || false_target.is_linked()) {
    // We have at least one condition value that has been "translated"
    // into a branch, thus it needs to be loaded explicitly.
    JumpTarget loaded;
    if (frame_ != NULL) {
      loaded.Jump();  // Don't lose the current TOS.
    }
    bool both = true_target.is_linked() && false_target.is_linked();
    // Load "true" if necessary.
    if (true_target.is_linked()) {
      true_target.Bind();
      __ LoadRoot(v0, Heap::kTrueValueRootIndex);
      frame_->EmitPush(v0);
    }
    // If both "true" and "false" need to be loaded jump across the code for
    // "false".
    if (both) {
      loaded.Jump();
    }
    // Load "false" if necessary.
    if (false_target.is_linked()) {
      false_target.Bind();
      __ LoadRoot(v0, Heap::kFalseValueRootIndex);
      frame_->EmitPush(v0);
    }
    // A value is loaded on all paths reaching this point.
    loaded.Bind();
  }
  ASSERT(has_valid_frame());
  ASSERT(!has_cc());
  ASSERT(frame_->height() == original_height + 1);
}


void CodeGenerator::LoadGlobal() {
  VirtualFrame::SpilledScope spilled_scope;
  __ lw(a0, GlobalObject());
  frame_->EmitPush(a0);
}


void CodeGenerator::LoadGlobalReceiver(Register scratch) {
  VirtualFrame::SpilledScope spilled_scope;
  __ lw(scratch, ContextOperand(cp, Context::GLOBAL_INDEX));
  __ lw(scratch,
         FieldMemOperand(scratch, GlobalObject::kGlobalReceiverOffset));
  frame_->EmitPush(scratch);
}


void CodeGenerator::LoadTypeofExpression(Expression* x) {
  // Special handling of identifiers as subxessions of typeof.
  VirtualFrame::SpilledScope spilled_scope;
  Variable* variable = x->AsVariableProxy()->AsVariable();
  if (variable != NULL && !variable->is_this() && variable->is_global()) {
    // For a global variable we build the property reference
    // <global>.<variable> and perform a (regular non-contextual) property
    // load to make sure we do not get reference errors.
    Slot global(variable, Slot::CONTEXT, Context::GLOBAL_INDEX);
    Literal key(variable->name());
    Property property(&global, &key, RelocInfo::kNoPosition);
    Reference ref(this, &property);
    ref.GetValueAndSpill();
  } else if (variable != NULL && variable->slot() != NULL) {
    // For a variable that rewrites to a slot, we signal it is the immediate
    // subxession of a typeof.
    LoadFromSlot(variable->slot(), INSIDE_TYPEOF);
    frame_->SpillAll();
  } else {
    // Anything else can be handled normally.
    LoadAndSpill(x);
  }
}


void CodeGenerator::LoadFromSlot(Slot* slot, TypeofState typeof_state) {
  VirtualFrame::SpilledScope spilled_scope;
  if (slot->type() == Slot::LOOKUP) {
    ASSERT(slot->var()->is_dynamic());

    JumpTarget slow;
    JumpTarget done;

    // Generate fast-case code for variables that might be shadowed by
    // eval-introduced variables.  Eval is used a lot without
    // introducing variables.  In those cases, we do not want to
    // perform a runtime call for all variables in the scope
    // containing the eval.
    if (slot->var()->mode() == Variable::DYNAMIC_GLOBAL) {
      LoadFromGlobalSlotCheckExtensions(slot, typeof_state, a1, a2, &slow);
      // If there was no control flow to slow, we can exit early.
      if (!slow.is_linked()) {
        frame_->EmitPush(v0);
        return;
      }

      done.Jump();

    } else if (slot->var()->mode() == Variable::DYNAMIC_LOCAL) {
      UNIMPLEMENTED_MIPS();
      __ break_(__LINE__);
    }

    slow.Bind();
    frame_->EmitPush(cp);
    __ li(v0, Operand(slot->var()->name()));
    frame_->EmitPush(v0);

    if (typeof_state == INSIDE_TYPEOF) {
      frame_->CallRuntime(Runtime::kLoadContextSlotNoReferenceError, 2);
    } else {
      frame_->CallRuntime(Runtime::kLoadContextSlot, 2);
    }
    done.Bind();
    frame_->EmitPush(v0);

  } else {
    __ lw(v0, SlotOperand(slot, a2));
    frame_->EmitPush(v0);
    if (slot->var()->mode() == Variable::CONST) {
      UNIMPLEMENTED_MIPS();
      __ break_(__LINE__);
    }
  }
}


void CodeGenerator::LoadFromGlobalSlotCheckExtensions(Slot* slot,
                                                      TypeofState typeof_state,
                                                      Register tmp,
                                                      Register tmp2,
                                                      JumpTarget* slow) {
  // Check that no extension objects have been created by calls to
  // eval from the current scope to the global scope.
  Register context = cp;
  Scope* s = scope();
  while (s != NULL) {
    if (s->num_heap_slots() > 0) {
      if (s->calls_eval()) {
        // Check that extension is NULL.
        __ lw(tmp2, ContextOperand(context, Context::EXTENSION_INDEX));
        slow->Branch(ne, tmp2, Operand(zero_reg));
      }
      // Load next context in chain.
      __ lw(tmp, ContextOperand(context, Context::CLOSURE_INDEX));
      __ lw(tmp, FieldMemOperand(tmp, JSFunction::kContextOffset));
      context = tmp;
    }
    // If no outer scope calls eval, we do not need to check more
    // context extensions.
    if (!s->outer_scope_calls_eval() || s->is_eval_scope()) break;
    s = s->outer_scope();
  }

  if (s->is_eval_scope()) {
    Label next, fast;
    if (!context.is(tmp)) {
      __ li(tmp, Operand(context));
    }
    __ bind(&next);
    // Terminate at global context.
    __ lw(tmp2, FieldMemOperand(tmp, HeapObject::kMapOffset));
    __ LoadRoot(t8, Heap::kGlobalContextMapRootIndex);
    __ Branch(eq, &fast, tmp2, Operand(t8));
    // Check that extension is NULL.
    __ lw(tmp2, ContextOperand(tmp, Context::EXTENSION_INDEX));
    slow->Branch(ne, tmp2, Operand(zero_reg));
    // Load next context in chain.
    __ lw(tmp, ContextOperand(tmp, Context::CLOSURE_INDEX));
    __ lw(tmp, FieldMemOperand(tmp, JSFunction::kContextOffset));
    __ jmp(&next);
    __ bind(&fast);
  }

  // All extension objects were empty and it is safe to use a global
  // load IC call.
  Handle<Code> ic(Builtins::builtin(Builtins::LoadIC_Initialize));
  // Load the global object.
  LoadGlobal();
  // Setup the name register.
  __ li(a2, Operand(slot->var()->name()));
  // Call IC stub.
  if (typeof_state == INSIDE_TYPEOF) {
    frame_->CallCodeObject(ic, RelocInfo::CODE_TARGET, 0);
  } else {
    frame_->CallCodeObject(ic, RelocInfo::CODE_TARGET_CONTEXT, 0);
  }

  // Drop the global object. The result is in v0.
  frame_->Drop();
}


void CodeGenerator::StoreToSlot(Slot* slot, InitState init_state) {
  ASSERT(slot != NULL);
  if (slot->type() == Slot::LOOKUP) {
    ASSERT(slot->var()->is_dynamic());

    // For now, just do a runtime call.
    frame_->EmitPush(cp);
    __ li(a0, Operand(slot->var()->name()));
    frame_->EmitPush(a0);

    if (init_state == CONST_INIT) {
      // Same as the case for a normal store, but ignores attribute
      // (e.g. READ_ONLY) of context slot so that we can initialize
      // const properties (introduced via eval("const foo = (some
      // expr);")). Also, uses the current function context instead of
      // the top context.
      //
      // Note that we must declare the foo upon entry of eval(), via a
      // context slot declaration, but we cannot initialize it at the
      // same time, because the const declaration may be at the end of
      // the eval code (sigh...) and the const variable may have been
      // used before (where its value is 'undefined'). Thus, we can only
      // do the initialization when we actually encounter the expression
      // and when the expression operands are defined and valid, and
      // thus we need the split into 2 operations: declaration of the
      // context slot followed by initialization.
      frame_->CallRuntime(Runtime::kInitializeConstContextSlot, 3);
    } else {
      frame_->CallRuntime(Runtime::kStoreContextSlot, 3);
    }
    // Storing a variable must keep the (new) value on the expression
    // stack. This is necessary for compiling assignment expressions.
    frame_->EmitPush(v0);

  } else {
    ASSERT(!slot->var()->is_dynamic());

    JumpTarget exit;
    if (init_state == CONST_INIT) {
      UNIMPLEMENTED_MIPS();
      __ break_(__LINE__);
    }

    // We must execute the store. Storing a variable must keep the
    // (new) value on the stack. This is necessary for compiling
    // assignment expressions.
    //
    // Note: We will reach here even with slot->var()->mode() ==
    // Variable::CONST because of const declarations which will
    // initialize consts to 'the hole' value and by doing so, end up
    // calling this code. a2 may be loaded with context; used below in
    // RecordWrite.
    frame_->EmitPop(a0);
    __ sw(a0, SlotOperand(slot, a2));
    frame_->EmitPush(a0);
    if (slot->type() == Slot::CONTEXT) {
      // Skip write barrier if the written value is a smi.
      __ And(t0, a0, Operand(kSmiTagMask));
      exit.Branch(eq, t0, Operand(zero_reg));
      // a2 is loaded with context when calling SlotOperand above.
      int offset = FixedArray::kHeaderSize + slot->index() * kPointerSize;
      __ li(a3, Operand(offset));
      __ RecordWrite(a2, a3, a1);
    }
    // If we definitely did not jump over the assignment, we do not need
    // to bind the exit label. Doing so can defeat peephole
    // optimization.
    if (init_state == CONST_INIT || slot->type() == Slot::CONTEXT) {
      exit.Bind();
    }
  }
}


// ECMA-262, section 9.2, page 30: ToBoolean(). Convert the given
// register to a boolean in the condition code register. The code
// may jump to 'false_target' in case the register converts to 'false'.
void CodeGenerator::ToBoolean(JumpTarget* true_target,
                              JumpTarget* false_target) {
  VirtualFrame::SpilledScope spilled_scope;
  // Note: The generated code snippet does not change stack variables.
  //       Only the condition code should be set.
  frame_->EmitPop(t0);

  // Fast case checks

  // Check if the value is 'false'.
  __ LoadRoot(t1, Heap::kFalseValueRootIndex);
  false_target->Branch(eq, t0, Operand(t1), no_hint);

  // Check if the value is 'true'.
  __ LoadRoot(t2, Heap::kTrueValueRootIndex);
  true_target->Branch(eq, t0, Operand(t2), no_hint);

  // Check if the value is 'undefined'.
  __ LoadRoot(t3, Heap::kUndefinedValueRootIndex);
  false_target->Branch(eq, t0, Operand(t3), no_hint);

  // Check if the value is a smi.
  // __ cmp(r0, Operand(Smi::FromInt(0)));             // plind:
  false_target->Branch(eq, t0, Operand(Smi::FromInt(0)), no_hint);
  __ And(t4, t0, Operand(kSmiTagMask));
  true_target->Branch(eq, t4, Operand(zero_reg), no_hint);

  // Slow case: call the runtime.
  frame_->EmitPush(t0);
  frame_->CallRuntime(Runtime::kToBool, 1);
  // Convert the result (v0) to a condition code.
  //  __ cmp(r0, ip);                                 // plind
  // __ LoadRoot(s6, Heap::kFalseValueRootIndex);     // plind
  __ LoadRoot(condReg1, Heap::kFalseValueRootIndex);  // plind: highly suspect
  __ mov(condReg2, v0);

  cc_reg_ = ne;
}


void CodeGenerator::GenericBinaryOperation(Token::Value op,
                                           OverwriteMode overwrite_mode,
                                           int constant_rhs) {
  VirtualFrame::SpilledScope spilled_scope;
  // sp[0] : y
  // sp[1] : x
  // result : v0

  // Stub is entered with a call: 'return address' is in lr.
  switch (op) {
    case Token::ADD:  // Fall through.
    case Token::SUB:  // Fall through.
    case Token::MUL:
    case Token::DIV:
    case Token::MOD:
    case Token::BIT_OR:
    case Token::BIT_AND:
    case Token::BIT_XOR:
    case Token::SHL:
    case Token::SHR:
    case Token::SAR: {
      frame_->EmitPop(a0);  // a0 : y
      frame_->EmitPop(a1);  // a1 : x
      GenericBinaryOpStub stub(op, overwrite_mode, constant_rhs);
      frame_->CallStub(&stub, 0);
      break;
    }

    case Token::COMMA:
      frame_->EmitPop(v0);
      // Simply discard left value.
      frame_->Drop();
      break;

    default:
      // Other cases should have been handled before this point.
      UNREACHABLE();
      break;
  }
}


class DeferredInlineSmiOperation: public DeferredCode {
 public:
  DeferredInlineSmiOperation(Token::Value op,
                             int value,
                             bool reversed,
                             OverwriteMode overwrite_mode)
      : op_(op),
        value_(value),
        reversed_(reversed),
        overwrite_mode_(overwrite_mode) {
    set_comment("[ DeferredInlinedSmiOperation");
  }

  virtual void Generate();

 private:
  Token::Value op_;
  int value_;
  bool reversed_;
  OverwriteMode overwrite_mode_;
};


void DeferredInlineSmiOperation::Generate() {
  // In CodeGenerator::SmiOperation we used a1 instead of a0, and we left the
  // register untouched.
  // We just need to load value_ and switch if necessary.
  switch (op_) {
    case Token::ADD:
    case Token::SUB:
    case Token::MUL:
    case Token::MOD:
    case Token::BIT_OR:
    case Token::BIT_XOR:
    case Token::BIT_AND: {
      if (reversed_) {
        __ mov(a0, a1);
        __ li(a1, Operand(Smi::FromInt(value_)));
      } else {
        __ li(a0, Operand(Smi::FromInt(value_)));
      }
      break;
    }
    case Token::SHL:
    case Token::SHR:
    case Token::SAR: {
      if (!reversed_) {
        __ li(a0, Operand(Smi::FromInt(value_)));
      } else {
        UNREACHABLE();  // Should have been handled in SmiOperation.
      }
      break;
    }

    default:
      // Other cases should have been handled before this point.
      UNREACHABLE();
      break;
  }

  GenericBinaryOpStub stub(op_, overwrite_mode_, value_);
  __ CallStub(&stub);
}


static bool PopCountLessThanEqual2(unsigned int x) {
  x &= x - 1;
  return (x & (x - 1)) == 0;
}


// Returns the index of the lowest bit set.
static int BitPosition(unsigned x) {
  int bit_posn = 0;
  while ((x & 0xf) == 0) {
    bit_posn += 4;
    x >>= 4;
  }
  while ((x & 1) == 0) {
    bit_posn++;
    x >>= 1;
  }
  return bit_posn;
}


void CodeGenerator::SmiOperation(Token::Value op,
                                 Handle<Object> value,
                                 bool reversed,
                                 OverwriteMode mode) {
  VirtualFrame::SpilledScope spilled_scope;
  // NOTE: This is an attempt to inline (a bit) more of the code for
  // some possible smi operations (like + and -) when (at least) one
  // of the operands is a literal smi. With this optimization, the
  // performance of the system is increased by ~15%, and the generated
  // code size is increased by ~1% (measured on a combination of
  // different benchmarks).

  // We care about keeping a1 unchanged, as it spares the need to reverse the
  // optimistic operation if we need to jump to the deferred code.

  // sp[0] : operand

  int int_value = Smi::cast(*value)->value();

  JumpTarget exit;
  // We use a1 instead of a0 because in most cases we will need the value in a1
  // if we jump to the deferred code.
  frame_->EmitPop(a1);

  bool something_to_inline = true;
  switch (op) {
    case Token::ADD: {
      DeferredCode* deferred =
          new DeferredInlineSmiOperation(op, int_value, reversed, mode);

      __ Addu(v0, a1, Operand(value));
      // Check for overflow.
      __ xor_(t0, v0, a1);
      __ Xor(t1, v0, Operand(value));
      __ and_(t0, t0, t1);    // Overflow occurred if result is negative.
      deferred->Branch(lt, t0);
      __ And(t0, v0, Operand(kSmiTagMask));
      deferred->Branch(ne, t0, Operand(zero_reg));
      deferred->BindExit();
      break;
    }

    case Token::SUB: {
      DeferredCode* deferred =
          new DeferredInlineSmiOperation(op, int_value, reversed, mode);

      __ li(t0, Operand(value));
      if (reversed) {
        __ Subu(v0, t0, Operand(a1));
        __ xor_(t2, v0, t0);  // Check for overflow.
      } else {
        __ Subu(v0, a1, Operand(t0));
        __ xor_(t2, v0, a1);  // Check for overflow.
      }
      __ xor_(t1, t0, a1);
      __ and_(t2, t2, t1);    // Overflow occurred if result is negative.
      deferred->Branch(lt, t2);
      __ And(t0, v0, Operand(kSmiTagMask));
      deferred->Branch(ne, t0, Operand(zero_reg));
      deferred->BindExit();
      break;
    }


    case Token::BIT_OR:
    case Token::BIT_XOR:
    case Token::BIT_AND: {
      DeferredCode* deferred =
        new DeferredInlineSmiOperation(op, int_value, reversed, mode);
      __ And(t0, a1, Operand(kSmiTagMask));
      deferred->Branch(ne, t0, Operand(zero_reg));
      switch (op) {
        case Token::BIT_OR:  __ Or(v0, a1, Operand(value)); break;
        case Token::BIT_XOR: __ Xor(v0, a1, Operand(value)); break;
        case Token::BIT_AND: __ And(v0, a1, Operand(value)); break;
        default: UNREACHABLE();
      }
      deferred->BindExit();
      break;
    }

    case Token::SHL:
    case Token::SHR:
    case Token::SAR: {
      if (reversed) {
        something_to_inline = false;
        break;
      }
      int shift_value = int_value & 0x1f;  // Least significant 5 bits.
      DeferredCode* deferred =
        new DeferredInlineSmiOperation(op, shift_value, false, mode);
      __ And(t0, a1, Operand(kSmiTagMask));
      deferred->Branch(ne, t0, Operand(zero_reg));
      __ sra(a2, a1, kSmiTagSize);  // Remove tag.
      switch (op) {
        case Token::SHL: {
          if (shift_value != 0) {
            __ sll(v0, a2, shift_value);
          }
          // Check that the *unsigned* result fits in a Smi.
          __ Addu(t3, v0, Operand(0x40000000));
          __ And(t3, t3, Operand(0x80000000));
          deferred->Branch(ne, t3, Operand(zero_reg));
          break;
        }
        case Token::SHR: {
          // LSR by immediate 0 means shifting 32 bits.
          if (shift_value != 0) {
            __ srl(v0, a2, shift_value);
          }
          // Check that the *unsigned* result fits in a smi.
          // Neither of the two high-order bits can be set:
          // - 0x80000000: high bit would be lost when smi tagging
          // - 0x40000000: this number would convert to negative when
          // Smi tagging these two cases can only happen with shifts
          // by 0 or 1 when handed a valid smi.
          // Check that the result fits in a Smi.
          __ And(t3, v0, Operand(0xc0000000));
          deferred->Branch(ne, t3, Operand(zero_reg));
          break;
        }
        case Token::SAR: {
          if (shift_value != 0) {
            // ASR by immediate 0 means shifting 32 bits.
            __ sra(v0, a2, shift_value);
          }
          break;
        }
        default: UNREACHABLE();
      }
      __ sll(v0, v0, kSmiTagSize);  // Tag result.
      deferred->BindExit();
      break;
    }

    case Token::MOD: {
      if (reversed || int_value < 2 || !IsPowerOf2(int_value)) {
        something_to_inline = false;
        break;
      }
      DeferredCode* deferred =
        new DeferredInlineSmiOperation(op, int_value, reversed, mode);
      unsigned mask = (0x80000000u | kSmiTagMask);
      __ And(t0, a1, Operand(mask));
      // Go to deferred code on non-Smis and negative.
      deferred->Branch(ne, t0, Operand(zero_reg));
      mask = (int_value << kSmiTagSize) - 1;
      __ And(v0, a1, Operand(mask));
      deferred->BindExit();
      break;
    }

    case Token::MUL: {
      if (!IsEasyToMultiplyBy(int_value)) {
        something_to_inline = false;
        break;
      }
      DeferredCode* deferred =
        new DeferredInlineSmiOperation(op, int_value, reversed, mode);
      unsigned max_smi_that_wont_overflow = Smi::kMaxValue / int_value;
      max_smi_that_wont_overflow <<= kSmiTagSize;
      unsigned mask = 0x80000000u;
      while ((mask & max_smi_that_wont_overflow) == 0) {
        mask |= mask >> 1;
      }
      mask |= kSmiTagMask;
      // This does a single mask that checks for a too high value in a
      // conservative way and for a non-Smi.  It also filters out negative
      // numbers, unfortunately, but since this code is inline we prefer
      // brevity to comprehensiveness.
      __ And(t0, a1, Operand(mask));
      deferred->Branch(ne, t0, Operand(zero_reg));
      MultiplyByKnownInt(masm_, a1, v0, int_value);
      deferred->BindExit();
      break;
    }

    default:
      something_to_inline = false;
      break;
  }

  if (!something_to_inline) {
    // Smi operand in a1, load literal value in a0.
    if (!reversed) {
      __ li(a0, Operand(value));
      frame_->EmitMultiPush(a0.bit() | a1.bit());
      GenericBinaryOperation(op, mode, int_value);
    } else {
      __ li(a0, Operand(value));
      frame_->EmitMultiPushReversed(a1.bit() | a0.bit());
      GenericBinaryOperation(op, mode, kUnknownIntValue);
    }
  }

  exit.Bind();
}


// On MIPS we load registers condReg1 and condReg2 with the values which should
// be compared. With the CodeGenerator::cc_reg_ condition, functions will be
// able to evaluate correctly the condition. (eg CodeGenerator::Branch)
void CodeGenerator::Comparison(Condition cc,
                               Expression* left,
                               Expression* right,
                               bool strict) {
  if (left != NULL) LoadAndSpill(left);
  if (right != NULL) LoadAndSpill(right);

  VirtualFrame::SpilledScope spilled_scope;
  // sp[0] : y  (right)
  // sp[1] : x  (left)

  // Strict only makes sense for equality comparisons.
  ASSERT(!strict || cc == eq);

  JumpTarget exit;
  JumpTarget smi;
  // Implement '>' and '<=' by reversal to obtain ECMA-262 conversion order.
  if (cc == greater || cc == less_equal) {
    cc = ReverseCondition(cc);
    frame_->EmitPop(a0);
    frame_->EmitPop(a1);
  } else {
    frame_->EmitPop(a1);
    frame_->EmitPop(a0);
  }
  __ Or(t0, a0, a1);
  __ And(t1, t0, kSmiTagMask);
  smi.Branch(eq, t1, Operand(zero_reg), no_hint);

  // Perform non-smi comparison by stub.
  // CompareStub takes arguments in a0 and a1, returns <0, >0 or 0 in v0.
  // We call with 0 args because there are 0 on the stack.
  CompareStub stub(cc, strict);
  frame_->CallStub(&stub, 0);
  __ mov(condReg1, v0);
  __ li(condReg2, Operand(0));
  exit.Jump();

  // Do smi comparison.
  smi.Bind();
  __ mov(condReg1, a0);
  __ mov(condReg2, a1);

  exit.Bind();
  cc_reg_ = cc;
}


void CodeGenerator::VisitStatements(ZoneList<Statement*>* statements) {
  VirtualFrame::SpilledScope spilled_scope;
  for (int i = 0; frame_ != NULL && i < statements->length(); i++) {
    VisitAndSpill(statements->at(i));
  }
}


void CodeGenerator::CallWithArguments(ZoneList<Expression*>* args,
                                      CallFunctionFlags flags,
                                      int position) {
  VirtualFrame::SpilledScope spilled_scope;
  // Push the arguments ("left-to-right") on the stack.
  int arg_count = args->length();
  for (int i = 0; i < arg_count; i++) {
    LoadAndSpill(args->at(i));
  }

  // Record the position for debugging purposes.
  CodeForSourcePosition(position);

  // Use the shared code stub to call the function.
  InLoopFlag in_loop = loop_nesting() > 0 ? IN_LOOP : NOT_IN_LOOP;
  CallFunctionStub call_function(arg_count, in_loop, flags);
  frame_->CallStub(&call_function, arg_count + 1);

  // Restore context and pop function from the stack.
  __ lw(cp, frame_->Context());
  frame_->Drop();  // Discard the TOS.
}


void CodeGenerator::Branch(bool if_true, JumpTarget* target) {
  VirtualFrame::SpilledScope spilled_scope;
  ASSERT(has_cc());
  Condition cc = if_true ? cc_reg_ : NegateCondition(cc_reg_);
  target->Branch(cc, condReg1, Operand(condReg2), no_hint);
  cc_reg_ = cc_always;
}


void CodeGenerator::CheckStack() {
  VirtualFrame::SpilledScope spilled_scope;
  Comment cmnt(masm_, "[ check stack");

  __ LoadRoot(t0, Heap::kStackLimitRootIndex);
  StackCheckStub stub;
  // Call the stub if lower.
  __ Push(ra);
  __ Call(Operand(reinterpret_cast<intptr_t>(stub.GetCode().location()),
          RelocInfo::CODE_TARGET),
          Uless, sp, Operand(t0));
  __ Pop(ra);
}


void CodeGenerator::VisitBlock(Block* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope;
  Comment cmnt(masm_, "[ Block");
  CodeForStatementPosition(node);
  node->break_target()->set_direction(JumpTarget::FORWARD_ONLY);
  VisitStatementsAndSpill(node->statements());
  if (node->break_target()->is_linked()) {
    node->break_target()->Bind();
  }
  node->break_target()->Unuse();
  ASSERT(!has_valid_frame() || frame_->height() == original_height);
}


void CodeGenerator::DeclareGlobals(Handle<FixedArray> pairs) {
  VirtualFrame::SpilledScope spilled_scope;
  frame_->EmitPush(cp);
  __ li(t0, Operand(pairs));
  frame_->EmitPush(t0);
  __ li(t0, Operand(Smi::FromInt(is_eval() ? 1 : 0)));
  frame_->EmitPush(t0);
  frame_->CallRuntime(Runtime::kDeclareGlobals, 3);
  // The result is discarded.
}


void CodeGenerator::VisitDeclaration(Declaration* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope;
  Comment cmnt(masm_, "[ Declaration");
  Variable* var = node->proxy()->var();
  ASSERT(var != NULL);  // Must have been resolved.
  Slot* slot = var->slot();

  // If it was not possible to allocate the variable at compile time,
  // we need to "declare" it at runtime to make sure it actually
  // exists in the local context.
  if (slot != NULL && slot->type() == Slot::LOOKUP) {
    UNIMPLEMENTED_MIPS();
    __ break_(__LINE__);
    return;
  }

  ASSERT(!var->is_global());

  // If we have a function or a constant, we need to initialize the variable.
  Expression* val = NULL;
  if (node->mode() == Variable::CONST) {
    UNIMPLEMENTED_MIPS();
    __ break_(__LINE__);
  } else {
    val = node->fun();  // NULL if we don't have a function.
  }

  if (val != NULL) {
    {
      // Set initial value.
      Reference target(this, node->proxy());
      LoadAndSpill(val);
      target.SetValue(NOT_CONST_INIT);
      // The reference is removed from the stack (preserving TOS) when
      // it goes out of scope.
    }
    // Get rid of the assigned value (declarations are statements).
    frame_->Drop();
  }
  ASSERT(frame_->height() == original_height);
}


void CodeGenerator::VisitExpressionStatement(ExpressionStatement* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope;
  Comment cmnt(masm_, "[ ExpressionStatement");
  CodeForStatementPosition(node);
  Expression* expression = node->expression();
  expression->MarkAsStatement();
  LoadAndSpill(expression);
  frame_->Drop();
  ASSERT(frame_->height() == original_height);
}


void CodeGenerator::VisitEmptyStatement(EmptyStatement* node) {
  UNIMPLEMENTED_MIPS();
  __ break_(__LINE__);
}


void CodeGenerator::VisitIfStatement(IfStatement* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope;
  Comment cmnt(masm_, "[ IfStatement");
  // Generate different code depending on which parts of the if statement
  // are present or not.
  bool has_then_stm = node->HasThenStatement();
  bool has_else_stm = node->HasElseStatement();

  CodeForStatementPosition(node);

  JumpTarget exit;
  if (has_then_stm && has_else_stm) {
    Comment cmnt(masm_, "[ IfThenElse");
    JumpTarget then;
    JumpTarget else_;
    // if (cond)
    LoadConditionAndSpill(node->condition(),
                          &then, &else_, true);
    if (frame_ != NULL) {
      Branch(false, &else_);
    }
    // then
    if (frame_ != NULL || then.is_linked()) {
      then.Bind();
      VisitAndSpill(node->then_statement());
    }
    if (frame_ != NULL) {
      exit.Jump();
    }
    // else
    if (else_.is_linked()) {
      else_.Bind();
      VisitAndSpill(node->else_statement());
    }

  } else if (has_then_stm) {
    Comment cmnt(masm_, "[ IfThen");
    ASSERT(!has_else_stm);
    JumpTarget then;
    // if (cond)
    LoadConditionAndSpill(node->condition(),
                          &then, &exit, true);
    if (frame_ != NULL) {
      Branch(false, &exit);
    }
    // then
    if (frame_ != NULL || then.is_linked()) {
      then.Bind();
      VisitAndSpill(node->then_statement());
    }

  } else if (has_else_stm) {
    Comment cmnt(masm_, "[ IfElse");
    ASSERT(!has_then_stm);
    JumpTarget else_;
    // if (!cond)
    LoadConditionAndSpill(node->condition(),
                          &exit, &else_, true);
    if (frame_ != NULL) {
      Branch(true, &exit);
    }
    // else
    if (frame_ != NULL || else_.is_linked()) {
      else_.Bind();
      VisitAndSpill(node->else_statement());
    }

  } else {
    Comment cmnt(masm_, "[ If");
    ASSERT(!has_then_stm && !has_else_stm);
    // if (cond)
    LoadConditionAndSpill(node->condition(),
                          &exit, &exit, false);
    if (frame_ != NULL) {
      if (has_cc()) {
        cc_reg_ = cc_always;
      } else {
        frame_->Drop();
      }
    }
  }

  // end
  if (exit.is_linked()) {
    exit.Bind();
  }
  ASSERT(!has_valid_frame() || frame_->height() == original_height);
}


void CodeGenerator::VisitContinueStatement(ContinueStatement* node) {
  VirtualFrame::SpilledScope spilled_scope;
  Comment cmnt(masm_, "[ ContinueStatement");
  CodeForStatementPosition(node);
  node->target()->continue_target()->Jump();
}


void CodeGenerator::VisitBreakStatement(BreakStatement* node) {
  VirtualFrame::SpilledScope spilled_scope;
  Comment cmnt(masm_, "[ BreakStatement");
  CodeForStatementPosition(node);
  node->target()->break_target()->Jump();
}


void CodeGenerator::VisitReturnStatement(ReturnStatement* node) {
  VirtualFrame::SpilledScope spilled_scope;
  Comment cmnt(masm_, "[ ReturnStatement");

  CodeForStatementPosition(node);
  LoadAndSpill(node->expression());
  if (function_return_is_shadowed_) {
    frame_->EmitPop(v0);
    function_return_.Jump();
  } else {
    // Pop the result from the frame and prepare the frame for
    // returning thus making it easier to merge.
    frame_->EmitPop(v0);
    frame_->PrepareForReturn();

    function_return_.Jump();
  }
}


void CodeGenerator::VisitWithEnterStatement(WithEnterStatement* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope;
  Comment cmnt(masm_, "[ WithEnterStatement");
  CodeForStatementPosition(node);
  LoadAndSpill(node->expression());
  if (node->is_catch_block()) {
    frame_->CallRuntime(Runtime::kPushCatchContext, 1);
  } else {
    frame_->CallRuntime(Runtime::kPushContext, 1);
  }
#ifdef DEBUG
  JumpTarget verified_true;
  verified_true.Branch(eq, v0, Operand(cp), no_hint);
  __ stop("PushContext: v0 is expected to be the same as cp");
  verified_true.Bind();
#endif
  // Update context local.
  __ sw(cp, frame_->Context());
  ASSERT(frame_->height() == original_height);
}


void CodeGenerator::VisitWithExitStatement(WithExitStatement* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope;
  Comment cmnt(masm_, "[ WithExitStatement");
  CodeForStatementPosition(node);
  // Pop context.
  __ lw(cp, ContextOperand(cp, Context::PREVIOUS_INDEX));
  // Update context local.
  __ sw(cp, frame_->Context());
  ASSERT(frame_->height() == original_height);
}


void CodeGenerator::VisitSwitchStatement(SwitchStatement* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope;
  Comment cmnt(masm_, "[ SwitchStatement");
  CodeForStatementPosition(node);
  node->break_target()->set_direction(JumpTarget::FORWARD_ONLY);

  LoadAndSpill(node->tag());

  JumpTarget next_test;
  JumpTarget fall_through;
  JumpTarget default_entry;
  JumpTarget default_exit(JumpTarget::BIDIRECTIONAL);
  ZoneList<CaseClause*>* cases = node->cases();
  int length = cases->length();
  CaseClause* default_clause = NULL;

  for (int i = 0; i < length; i++) {
    CaseClause* clause = cases->at(i);
    if (clause->is_default()) {
      // Remember the default clause and compile it at the end.
      default_clause = clause;
      continue;
    }

    Comment cmnt(masm_, "[ Case clause");
    // Compile the test.
    next_test.Bind();
    next_test.Unuse();
    // Duplicate TOS.
    __ lw(t0, frame_->Top());
    frame_->EmitPush(t0);
    Comparison(eq, NULL, clause->label(), true);
    Branch(false, &next_test);

    // Before entering the body from the test, remove the switch value from
    // the stack.
    frame_->Drop();

    // Label the body so that fall through is enabled.
    if (i > 0 && cases->at(i - 1)->is_default()) {
      default_exit.Bind();
    } else {
      fall_through.Bind();
      fall_through.Unuse();
    }
    VisitStatementsAndSpill(clause->statements());

    // If control flow can fall through from the body, jump to the next body
    // or the end of the statement.
    if (frame_ != NULL) {
      if (i < length - 1 && cases->at(i + 1)->is_default()) {
        default_entry.Jump();
      } else {
        fall_through.Jump();
      }
    }
  }

  // The final "test" removes the switch value.
  next_test.Bind();
  frame_->Drop();

  // If there is a default clause, compile it.
  if (default_clause != NULL) {
    Comment cmnt(masm_, "[ Default clause");
    default_entry.Bind();
    VisitStatementsAndSpill(default_clause->statements());
    // If control flow can fall out of the default and there is a case after
    // it, jup to that case's body.
    if (frame_ != NULL && default_exit.is_bound()) {
      default_exit.Jump();
    }
  }

  if (fall_through.is_linked()) {
    fall_through.Bind();
  }

  if (node->break_target()->is_linked()) {
    node->break_target()->Bind();
  }
  node->break_target()->Unuse();
  ASSERT(!has_valid_frame() || frame_->height() == original_height);
}


void CodeGenerator::VisitDoWhileStatement(DoWhileStatement* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope;
  Comment cmnt(masm_, "[ DoWhileStatement");
  CodeForStatementPosition(node);
  node->break_target()->set_direction(JumpTarget::FORWARD_ONLY);
  JumpTarget body(JumpTarget::BIDIRECTIONAL);

  // Label the top of the loop for the backward CFG edge.  If the test
  // is always true we can use the continue target, and if the test is
  // always false there is no need.
  ConditionAnalysis info = AnalyzeCondition(node->cond());
  switch (info) {
    case ALWAYS_TRUE:
      node->continue_target()->set_direction(JumpTarget::BIDIRECTIONAL);
      node->continue_target()->Bind();
      break;
    case ALWAYS_FALSE:
      node->continue_target()->set_direction(JumpTarget::FORWARD_ONLY);
      break;
    case DONT_KNOW:
      node->continue_target()->set_direction(JumpTarget::FORWARD_ONLY);
      body.Bind();
      break;
  }

  CheckStack();  // TODO(1222600): ignore if body contains calls.
  VisitAndSpill(node->body());

      // Compile the test.
  switch (info) {
    case ALWAYS_TRUE:
      // If control can fall off the end of the body, jump back to the
      // top.
      if (has_valid_frame()) {
        node->continue_target()->Jump();
      }
      break;
    case ALWAYS_FALSE:
      // If we have a continue in the body, we only have to bind its
      // jump target.
      if (node->continue_target()->is_linked()) {
        node->continue_target()->Bind();
      }
      break;
    case DONT_KNOW:
      // We have to compile the test expression if it can be reached by
      // control flow falling out of the body or via continue.
      if (node->continue_target()->is_linked()) {
        node->continue_target()->Bind();
      }
      if (has_valid_frame()) {
        LoadConditionAndSpill(node->cond(), &body, node->break_target(), true);
        if (has_valid_frame()) {
          // A invalid frame here indicates that control did not
          // fall out of the test expression.
          Branch(true, &body);
        }
      }
      break;
  }

  if (node->break_target()->is_linked()) {
    node->break_target()->Bind();
  }
  ASSERT(!has_valid_frame() || frame_->height() == original_height);
}


void CodeGenerator::VisitWhileStatement(WhileStatement* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope;
  Comment cmnt(masm_, "[ WhileStatement");
  CodeForStatementPosition(node);

  // If the test is never true and has no side effects there is no need
  // to compile the test or body.
  ConditionAnalysis info = AnalyzeCondition(node->cond());
  if (info == ALWAYS_FALSE) return;

  node->break_target()->set_direction(JumpTarget::FORWARD_ONLY);

  // Label the top of the loop with the continue target for the backward
  // CFG edge.
  node->continue_target()->set_direction(JumpTarget::BIDIRECTIONAL);
  node->continue_target()->Bind();


  if (info == DONT_KNOW) {
    JumpTarget body;
    LoadConditionAndSpill(node->cond(), &body, node->break_target(), true);
    if (has_valid_frame()) {
      // A NULL frame indicates that control did not fall out of the
      // test expression.
      Branch(false, node->break_target());
    }
    if (has_valid_frame() || body.is_linked()) {
      body.Bind();
    }
  }

  if (has_valid_frame()) {
    CheckStack();  // TODO(1222600): Ignore if body contains calls.
    VisitAndSpill(node->body());

    // If control flow can fall out of the body, jump back to the top.
    if (has_valid_frame()) {
      node->continue_target()->Jump();
    }
  }
  if (node->break_target()->is_linked()) {
    node->break_target()->Bind();
  }
  ASSERT(!has_valid_frame() || frame_->height() == original_height);
}


void CodeGenerator::VisitForStatement(ForStatement* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope;
  Comment cmnt(masm_, "[ ForStatement");
  CodeForStatementPosition(node);
  if (node->init() != NULL) {
    VisitAndSpill(node->init());
  }

  // If the test is never true there is no need to compile the test or
  // body.
  ConditionAnalysis info = AnalyzeCondition(node->cond());
  if (info == ALWAYS_FALSE) return;

  node->break_target()->set_direction(JumpTarget::FORWARD_ONLY);

  // If there is no update statement, label the top of the loop with the
  // continue target, otherwise with the loop target.
  JumpTarget loop(JumpTarget::BIDIRECTIONAL);
  if (node->next() == NULL) {
    node->continue_target()->set_direction(JumpTarget::BIDIRECTIONAL);
    node->continue_target()->Bind();
  } else {
    node->continue_target()->set_direction(JumpTarget::FORWARD_ONLY);
    loop.Bind();
  }

  // If the test is always true, there is no need to compile it.
  if (info == DONT_KNOW) {
    JumpTarget body;
    LoadConditionAndSpill(node->cond(), &body, node->break_target(), true);
    if (has_valid_frame()) {
      Branch(false, node->break_target());
    }
    if (has_valid_frame() || body.is_linked()) {
      body.Bind();
    }
  }

  if (has_valid_frame()) {
    CheckStack();  // TODO(1222600): ignore if body contains calls.
    VisitAndSpill(node->body());

    if (node->next() == NULL) {
      // If there is no update statement and control flow can fall out
      // of the loop, jump directly to the continue label.
      if (has_valid_frame()) {
        node->continue_target()->Jump();
      }
    } else {
      // If there is an update statement and control flow can reach it
      // via falling out of the body of the loop or continuing, we
      // compile the update statement.
      if (node->continue_target()->is_linked()) {
        node->continue_target()->Bind();
      }
      if (has_valid_frame()) {
        // Record source position of the statement as this code which is
        // after the code for the body actually belongs to the loop
        // statement and not the body.
        CodeForStatementPosition(node);
        VisitAndSpill(node->next());
        loop.Jump();
      }
    }
  }
  if (node->break_target()->is_linked()) {
    node->break_target()->Bind();
  }
  ASSERT(!has_valid_frame() || frame_->height() == original_height);
}


void CodeGenerator::VisitForInStatement(ForInStatement* node) {
  UNIMPLEMENTED_MIPS();
  __ break_(__LINE__);
}


void CodeGenerator::VisitTryCatchStatement(TryCatchStatement* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope;
  Comment cmnt(masm_, "[ TryCatchStatement");
  CodeForStatementPosition(node);

  JumpTarget try_block;
  JumpTarget exit;

  try_block.Call();
  // --- Catch block ---
  frame_->EmitPush(v0);

  // Store the caught exception in the catch variable.
  Variable* catch_var = node->catch_var()->var();
  ASSERT(catch_var != NULL && catch_var->slot() != NULL);
  StoreToSlot(catch_var->slot(), NOT_CONST_INIT);

  // Remove the exception from the stack.
  frame_->Drop();

  VisitStatementsAndSpill(node->catch_block()->statements());
  if (frame_ != NULL) {
    exit.Jump();
  }


  // --- Try block ---
  try_block.Bind();

  frame_->PushTryHandler(TRY_CATCH_HANDLER);
  int handler_height = frame_->height();

  // Shadow the labels for all escapes from the try block, including
  // returns. During shadowing, the original label is hidden as the
  // LabelShadow and operations on the original actually affect the
  // shadowing label.
  //
  // We should probably try to unify the escaping labels and the return
  // label.
  int nof_escapes = node->escaping_targets()->length();
  List<ShadowTarget*> shadows(1 + nof_escapes);

  // Add the shadow target for the function return.
  static const int kReturnShadowIndex = 0;
  shadows.Add(new ShadowTarget(&function_return_));
  bool function_return_was_shadowed = function_return_is_shadowed_;
  function_return_is_shadowed_ = true;
  ASSERT(shadows[kReturnShadowIndex]->other_target() == &function_return_);

  // Add the remaining shadow targets.
  for (int i = 0; i < nof_escapes; i++) {
    shadows.Add(new ShadowTarget(node->escaping_targets()->at(i)));
  }

  // Generate code for the statements in the try block.

  VisitStatementsAndSpill(node->try_block()->statements());

  // Stop the introduced shadowing and count the number of required unlinks.
  // After shadowing stops, the original labels are unshadowed and the
  // LabelShadows represent the formerly shadowing labels.
  bool has_unlinks = false;
  for (int i = 0; i < shadows.length(); i++) {
    shadows[i]->StopShadowing();
    has_unlinks = has_unlinks || shadows[i]->is_linked();
  }
  function_return_is_shadowed_ = function_return_was_shadowed;

  // Get an external reference to the handler address.
  ExternalReference handler_address(Top::k_handler_address);

  // If we can fall off the end of the try block, unlink from try chain.
  if (has_valid_frame()) {
    // The next handler address is on top of the frame. Unlink from
    // the handler list and drop the rest of this handler from the
    // frame.
    ASSERT(StackHandlerConstants::kNextOffset == 0);
    frame_->EmitPop(a1);
    __ li(a3, Operand(handler_address));
    __ sw(a1, MemOperand(a3));
    frame_->Drop(StackHandlerConstants::kSize / kPointerSize - 1);
    if (has_unlinks) {
      exit.Jump();
    }
  }

  // Generate unlink code for the (formerly) shadowing labels that have been
  // jumped to.  Deallocate each shadow target.
  for (int i = 0; i < shadows.length(); i++) {
    if (shadows[i]->is_linked()) {
      // Unlink from try chain;
      shadows[i]->Bind();
      // Because we can be jumping here (to spilled code) from unspilled
      // code, we need to reestablish a spilled frame at this block.
      frame_->SpillAll();

      // Reload sp from the top handler, because some statements that we
      // break from (eg, for...in) may have left stuff on the stack.
      __ li(a3, Operand(handler_address));
      __ lw(sp, MemOperand(a3));
      frame_->Forget(frame_->height() - handler_height);

      ASSERT(StackHandlerConstants::kNextOffset == 0);
      frame_->EmitPop(a1);
      __ sw(a1, MemOperand(a3));
      frame_->Drop(StackHandlerConstants::kSize / kPointerSize - 1);

      if (!function_return_is_shadowed_ && i == kReturnShadowIndex) {
        frame_->PrepareForReturn();
      }
      shadows[i]->other_target()->Jump();
    }
  }

  exit.Bind();
  ASSERT(!has_valid_frame() || frame_->height() == original_height);
}


void CodeGenerator::VisitTryFinallyStatement(TryFinallyStatement* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope;
  Comment cmnt(masm_, "[ TryFinallyStatement");
  CodeForStatementPosition(node);

  // State: Used to keep track of reason for entering the finally
  // block. Should probably be extended to hold information for
  // break/continue from within the try block.
  enum { FALLING, THROWING, JUMPING };

  JumpTarget try_block;
  JumpTarget finally_block;

  try_block.Call();

  frame_->EmitPush(v0);  // Save exception object on the stack.
  // In case of thrown exceptions, this is where we continue.
  __ li(a2, Operand(Smi::FromInt(THROWING)));
  finally_block.Jump();

  // --- Try block ---
  try_block.Bind();

  frame_->PushTryHandler(TRY_FINALLY_HANDLER);
  int handler_height = frame_->height();

  // Shadow the labels for all escapes from the try block, including
  // returns. Shadowing hides the original label as the LabelShadow and
  // operations on the original actually affect the shadowing label.

  // We should probably try to unify the escaping labels and the return
  // label.
  int nof_escapes = node->escaping_targets()->length();
  List<ShadowTarget*> shadows(1 + nof_escapes);

  // Add the shadow target for the function return.
  static const int kReturnShadowIndex = 0;
  shadows.Add(new ShadowTarget(&function_return_));
  bool function_return_was_shadowed = function_return_is_shadowed_;
  function_return_is_shadowed_ = true;
  ASSERT(shadows[kReturnShadowIndex]->other_target() == &function_return_);

  // Add the remaining shadow targets.
  for (int i = 0; i < nof_escapes; i++) {
    shadows.Add(new ShadowTarget(node->escaping_targets()->at(i)));
  }

  // Generate code for the statements in the try block.
  VisitStatementsAndSpill(node->try_block()->statements());

  // Stop the introduced shadowing and count the number of required unlinks.
  // After shadowing stops, the original labels are unshadowed and the
  // LabelShadows represent the formerly shadowing labels.
  int nof_unlinks = 0;
  for (int i = 0; i < shadows.length(); i++) {
    shadows[i]->StopShadowing();
    if (shadows[i]->is_linked()) nof_unlinks++;
  }
  function_return_is_shadowed_ = function_return_was_shadowed;

  // Get an external reference to the handler address.
  ExternalReference handler_address(Top::k_handler_address);

  // If we can fall off the end of the try block, unlink from the try
  // chain and set the state on the frame to FALLING.
  if (has_valid_frame()) {
    // The next handler address is on top of the frame.
    ASSERT(StackHandlerConstants::kNextOffset == 0);
    frame_->EmitPop(a1);
    __ li(a3, Operand(handler_address));
    __ sw(a1, MemOperand(a3));
    frame_->Drop(StackHandlerConstants::kSize / kPointerSize - 1);

    // Fake a top of stack value (unneeded when FALLING) and set the
    // state in a2, then jump around the unlink blocks if any.
    __ LoadRoot(v0, Heap::kUndefinedValueRootIndex);
    frame_->EmitPush(v0);
    __ li(a2, Operand(Smi::FromInt(FALLING)));
    if (nof_unlinks > 0) {
      finally_block.Jump();
    }
  }

  // Generate code to unlink and set the state for the (formerly)
  // shadowing targets that have been jumped to.
  for (int i = 0; i < shadows.length(); i++) {
    if (shadows[i]->is_linked()) {
      // If we have come from the shadowed return, the return value is
      // in (a non-refcounted reference to) r0.  We must preserve it
      // until it is pushed.
      //
      // Because we can be jumping here (to spilled code) from
      // unspilled code, we need to reestablish a spilled frame at
      // this block.
      shadows[i]->Bind();
      frame_->SpillAll();

      // Reload sp from the top handler, because some statements that
      // we break from (eg, for...in) may have left stuff on the
      // stack.
      __ li(a3, Operand(handler_address));
      __ lw(sp, MemOperand(a3));
      frame_->Forget(frame_->height() - handler_height);

      // Unlink this handler and drop it from the frame.  The next
      // handler address is currently on top of the frame.
      ASSERT(StackHandlerConstants::kNextOffset == 0);
      frame_->EmitPop(a1);
      __ sw(a1, MemOperand(a3));
      frame_->Drop(StackHandlerConstants::kSize / kPointerSize - 1);

      if (i == kReturnShadowIndex) {
        // If this label shadowed the function return, materialize the
        // return value on the stack.
        frame_->EmitPush(v0);
      } else {
        // Fake TOS for targets that shadowed breaks and continues.
        __ LoadRoot(v0, Heap::kUndefinedValueRootIndex);
        frame_->EmitPush(v0);
      }
      __ li(a2, Operand(Smi::FromInt(JUMPING + i)));
      if (--nof_unlinks > 0) {
        // If this is not the last unlink block, jump around the next.
        finally_block.Jump();
      }
    }
  }

  // --- Finally block ---
  finally_block.Bind();

  // Push the state on the stack.
  frame_->EmitPush(a2);

  // We keep two elements on the stack - the (possibly faked) result
  // and the state - while evaluating the finally block.
  //
  // Generate code for the statements in the finally block.
  VisitStatementsAndSpill(node->finally_block()->statements());

  if (has_valid_frame()) {
    // Restore state and return value or faked TOS.
    frame_->EmitPop(a2);
    frame_->EmitPop(a0);
  }

  // Generate code to jump to the right destination for all used
  // formerly shadowing targets.  Deallocate each shadow target.
  for (int i = 0; i < shadows.length(); i++) {
    if (has_valid_frame() && shadows[i]->is_bound()) {
      JumpTarget* original = shadows[i]->other_target();
      if (!function_return_is_shadowed_ && i == kReturnShadowIndex) {
        JumpTarget skip;
        skip.Branch(ne, a2, Operand(Smi::FromInt(JUMPING + i)), no_hint);
        frame_->PrepareForReturn();
        original->Jump();
        skip.Bind();
      } else {
        original->Branch(eq, a2, Operand(Smi::FromInt(JUMPING + i)), no_hint);
      }
    }
  }

  if (has_valid_frame()) {
    // Check if we need to rethrow the exception.
    JumpTarget exit;
    exit.Branch(ne, a2, Operand(Smi::FromInt(THROWING)), no_hint);

    // Rethrow exception.
    __ break_(__LINE__);
    frame_->EmitPush(v0);
    frame_->CallRuntime(Runtime::kReThrow, 1);

    // Done.
    exit.Bind();
  }
  ASSERT(!has_valid_frame() || frame_->height() == original_height);
}


void CodeGenerator::VisitDebuggerStatement(DebuggerStatement* node) {
  UNIMPLEMENTED_MIPS();
  __ break_(__LINE__);
}


void CodeGenerator::InstantiateFunction(
    Handle<SharedFunctionInfo> function_info) {
  VirtualFrame::SpilledScope spilled_scope;
  __ li(a0, Operand(function_info));
  // Use the fast case closure allocation code that allocates in new
  // space for nested functions that don't need literals cloning.
//  if (scope()->is_function_scope() && function_info->num_literals() == 0) {
//    FastNewClosureStub stub;
//    frame_->EmitPush(a0);
//    frame_->CallStub(&stub, 1);
//    frame_->EmitPush(v0);
//  } else {
    // Create a new closure.
    frame_->EmitPush(cp);
    frame_->EmitPush(a0);
    frame_->CallRuntime(Runtime::kNewClosure, 2);
    frame_->EmitPush(v0);
//  }
}


void CodeGenerator::VisitFunctionLiteral(FunctionLiteral* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope;
  Comment cmnt(masm_, "[ FunctionLiteral");

  // Build the function info and instantiate it.
  Handle<SharedFunctionInfo> function_info =
      Compiler::BuildFunctionInfo(node, script(), this);
  // Check for stack-overflow exception.
  if (HasStackOverflow()) {
    ASSERT(frame_->height() == original_height);
    return;
  }
  InstantiateFunction(function_info);
  ASSERT(frame_->height() == original_height + 1);
}


void CodeGenerator::VisitSharedFunctionInfoLiteral(
    SharedFunctionInfoLiteral* node) {
  UNIMPLEMENTED_MIPS();
  __ break_(__LINE__);
}


void CodeGenerator::VisitConditional(Conditional* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope;
  Comment cmnt(masm_, "[ Conditional");
  JumpTarget then;
  JumpTarget else_;
  LoadConditionAndSpill(node->condition(), &then, &else_, true);
  if (has_valid_frame()) {
    Branch(false, &else_);
  }
  if (has_valid_frame() || then.is_linked()) {
    then.Bind();
    LoadAndSpill(node->then_expression());
  }
  if (else_.is_linked()) {
    JumpTarget exit;
    if (has_valid_frame()) {
      exit.Jump();
    }
    else_.Bind();
    LoadAndSpill(node->else_expression());
    if (exit.is_linked()) exit.Bind();
  }
  ASSERT(frame_->height() == original_height + 1);
}


void CodeGenerator::VisitSlot(Slot* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope;
  Comment cmnt(masm_, "[ Slot");
  LoadFromSlot(node, typeof_state());
  ASSERT(frame_->height() == original_height + 1);
}


void CodeGenerator::VisitVariableProxy(VariableProxy* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope;
  Comment cmnt(masm_, "[ VariableProxy");

  Variable* var = node->var();
  Expression* expr = var->rewrite();
  if (expr != NULL) {
    Visit(expr);
  } else {
    ASSERT(var->is_global());
    Reference ref(this, node);
    ref.GetValueAndSpill();
  }
  ASSERT(frame_->height() == original_height + 1);
}


void CodeGenerator::VisitLiteral(Literal* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope;
  Comment cmnt(masm_, "[ Literal");
  __ li(t0, Operand(node->handle()));
  frame_->EmitPush(t0);
  ASSERT(frame_->height() == original_height + 1);
}


void CodeGenerator::VisitRegExpLiteral(RegExpLiteral* node) {
  UNIMPLEMENTED_MIPS();
  __ break_(__LINE__);
}


void CodeGenerator::VisitObjectLiteral(ObjectLiteral* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope;
  Comment cmnt(masm_, "[ ObjectLiteral");

  // Load the function of this activation.
  __ lw(a3, frame_->Function());
  // Literal array.
  __ lw(t3, FieldMemOperand(a3, JSFunction::kLiteralsOffset));
  // Literal index.
  __ li(t2, Operand(Smi::FromInt(node->literal_index())));
  // Constant properties.
  __ li(t1, Operand(node->constant_properties()));
  // Should the object literal have fast elements?
  __ li(t0, Operand(Smi::FromInt(node->fast_elements() ? 1 : 0)));
  frame_->EmitMultiPush(t3.bit() | t2.bit() | t1.bit() | t0.bit());

  if (node->depth() > 1) {
    frame_->CallRuntime(Runtime::kCreateObjectLiteral, 4);
  } else {
    frame_->CallRuntime(Runtime::kCreateObjectLiteralShallow, 4);
  }
  frame_->EmitPush(v0);  // Save the result.

  for (int i = 0; i < node->properties()->length(); i++) {
    // At the start of each iteration, the top of stack contains
    // the newly created object literal.
    ObjectLiteral::Property* property = node->properties()->at(i);
    Literal* key = property->key();
    Expression* value = property->value();
    switch (property->kind()) {
      case ObjectLiteral::Property::CONSTANT:
        break;
      case ObjectLiteral::Property::MATERIALIZED_LITERAL:
        if (CompileTimeValue::IsCompileTimeValue(property->value())) break;
        // Else fall through
      case ObjectLiteral::Property::COMPUTED:
        if (key->handle()->IsSymbol()) {
          Handle<Code> ic(Builtins::builtin(Builtins::StoreIC_Initialize));
          LoadAndSpill(value);
          frame_->EmitPop(a0);
          __ li(a2, Operand(key->handle()));
          __ lw(a1, frame_->Top());  // Load the receiver.
          frame_->CallCodeObject(ic, RelocInfo::CODE_TARGET, 0);
          break;
        }
        // Else fall through.
      case ObjectLiteral::Property::PROTOTYPE: {
        __ lw(a0, frame_->Top());
        frame_->EmitPush(a0);  // Dup the result.
        LoadAndSpill(key);
        LoadAndSpill(value);
        frame_->CallRuntime(Runtime::kSetProperty, 3);
        break;
      }
      case ObjectLiteral::Property::SETTER: {
        __ lw(a0, frame_->Top());
        frame_->EmitPush(v0);
        LoadAndSpill(key);
        __ li(a0, Operand(Smi::FromInt(1)));
        frame_->EmitPush(a0);
        LoadAndSpill(value);
        frame_->CallRuntime(Runtime::kDefineAccessor, 4);
        break;
      }
      case ObjectLiteral::Property::GETTER: {
        __ lw(a0, frame_->Top());
        frame_->EmitPush(v0);
        LoadAndSpill(key);
        __ li(a0, Operand(Smi::FromInt(0)));
        frame_->EmitPush(a0);
        LoadAndSpill(value);
        frame_->CallRuntime(Runtime::kDefineAccessor, 4);
        break;
      }
    }
  }
  ASSERT(frame_->height() == original_height + 1);
}


void CodeGenerator::VisitArrayLiteral(ArrayLiteral* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope;
  Comment cmnt(masm_, "[ ArrayLiteral");

  // Load the function of this activation.
  __ lw(a2, frame_->Function());
  // Load the literals array of the function.
  __ lw(a2, FieldMemOperand(a2, JSFunction::kLiteralsOffset));
  __ li(a1, Operand(Smi::FromInt(node->literal_index())));
  __ li(a0, Operand(node->constant_elements()));
  frame_->EmitMultiPush(a2.bit() | a1.bit() | a0.bit());
  int length = node->values()->length();
  if (node->depth() > 1) {
    frame_->CallRuntime(Runtime::kCreateArrayLiteral, 3);
  } else if (length > FastCloneShallowArrayStub::kMaximumLength) {
    frame_->CallRuntime(Runtime::kCreateArrayLiteralShallow, 3);
  } else {
    FastCloneShallowArrayStub stub(length);
    frame_->CallStub(&stub, 3);
  }
  frame_->EmitPush(v0);  // Save the result.
  // v0: created object literal

  // Generate code to set the elements in the array that are not
  // literals.
  for (int i = 0; i < node->values()->length(); i++) {
    Expression* value = node->values()->at(i);

    // If value is a literal the property value is already set in the
    // boilerplate object.
    if (value->AsLiteral() != NULL) continue;
    // If value is a materialized literal the property value is already set
    // in the boilerplate object if it is simple.
    if (CompileTimeValue::IsCompileTimeValue(value)) continue;

    // The property must be set by generated code.
    LoadAndSpill(value);
    frame_->EmitPop(a0);

    // Fetch the object literal.
    __ lw(a1, frame_->Top());
    // Get the elements array.
    __ lw(a1, FieldMemOperand(a1, JSObject::kElementsOffset));

    // Write to the indexed properties array.
    int offset = i * kPointerSize + FixedArray::kHeaderSize;
    __ sw(a0, FieldMemOperand(a1, offset));

    // Update the write barrier for the array address.
    __ li(a3, Operand(offset));
    __ RecordWrite(a1, a3, a2);
  }
  ASSERT(frame_->height() == original_height + 1);
}


void CodeGenerator::VisitCatchExtensionObject(CatchExtensionObject* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope;
  // Call runtime routine to allocate the catch extension object and
  // assign the exception value to the catch variable.
  Comment cmnt(masm_, "[ CatchExtensionObject");
  LoadAndSpill(node->key());
  LoadAndSpill(node->value());
  frame_->CallRuntime(Runtime::kCreateCatchExtensionObject, 2);
  frame_->EmitPush(v0);
  ASSERT(frame_->height() == original_height + 1);
}


void CodeGenerator::VisitAssignment(Assignment* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope;
  Comment cmnt(masm_, "[ Assignment");

  { Reference target(this, node->target(), node->is_compound());
    if (target.is_illegal()) {
      // Fool the virtual frame into thinking that we left the assignment's
      // value on the frame.
      frame_->EmitPush(zero_reg);
      ASSERT(frame_->height() == original_height + 1);
      return;
    }

    if (node->op() == Token::ASSIGN ||
        node->op() == Token::INIT_VAR ||
        node->op() == Token::INIT_CONST) {
      LoadAndSpill(node->value());

    } else {  // Assignment is a compound assignment.
      // Get the old value of the lhs.
      target.GetValueAndSpill();
      Literal* literal = node->value()->AsLiteral();
      bool overwrite =
          (node->value()->AsBinaryOperation() != NULL &&
           node->value()->AsBinaryOperation()->ResultOverwriteAllowed());
      if (literal != NULL && literal->handle()->IsSmi()) {
        SmiOperation(node->binary_op(),
                     literal->handle(),
                     false,
                     overwrite ? OVERWRITE_RIGHT : NO_OVERWRITE);
        frame_->EmitPush(v0);

      } else {
        LoadAndSpill(node->value());
        GenericBinaryOperation(node->binary_op(),
                               overwrite ? OVERWRITE_RIGHT : NO_OVERWRITE);
        frame_->EmitPush(v0);
      }
    }

    Variable* var = node->target()->AsVariableProxy()->AsVariable();
    if (var != NULL &&
        (var->mode() == Variable::CONST) &&
        node->op() != Token::INIT_VAR && node->op() != Token::INIT_CONST) {
      // Assignment ignored - leave the value on the stack.
      UnloadReference(&target);
    } else {
      CodeForSourcePosition(node->position());
      if (node->op() == Token::INIT_CONST) {
        // Dynamic constant initializations must use the function context
        // and initialize the actual constant declared. Dynamic variable
        // initializations are simply assignments and use SetValue.
        target.SetValue(CONST_INIT);
      } else {
        target.SetValue(NOT_CONST_INIT);
      }
    }
  }
  ASSERT(frame_->height() == original_height + 1);
}


void CodeGenerator::VisitThrow(Throw* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope;
  Comment cmnt(masm_, "[ Throw");

  LoadAndSpill(node->exception());
  CodeForSourcePosition(node->position());
  frame_->CallRuntime(Runtime::kThrow, 1);
  frame_->EmitPush(v0);
  ASSERT(frame_->height() == original_height + 1);
}


void CodeGenerator::VisitProperty(Property* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope;
  Comment cmnt(masm_, "[ Property");

  { Reference property(this, node);
    property.GetValueAndSpill();
  }
  ASSERT(frame_->height() == original_height + 1);
}


void CodeGenerator::VisitCall(Call* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope;
  Comment cmnt(masm_, "[ Call");

  Expression* function = node->expression();
  ZoneList<Expression*>* args = node->arguments();

  // Standard function call.
  // Check if the function is a variable or a property.
  Variable* var = function->AsVariableProxy()->AsVariable();
  Property* property = function->AsProperty();

  // ------------------------------------------------------------------------
  // Fast-case: Use inline caching.
  // ---
  // According to ECMA-262, section 11.2.3, page 44, the function to call
  // must be resolved after the arguments have been evaluated. The IC code
  // automatically handles this by loading the arguments before the function
  // is resolved in cache misses (this also holds for megamorphic calls).
  // ------------------------------------------------------------------------

  if (var != NULL && var->is_possibly_eval()) {
    UNIMPLEMENTED_MIPS();
    __ break_(__LINE__);

  } else if (var != NULL && !var->is_this() && var->is_global()) {
    // -----------------------------------------------------
    // JavaScript example: 'foo(1, 2, 3)'  // foo is global.
    // -----------------------------------------------------


    // Pass the global object as the receiver and let the IC stub
    // patch the stack to use the global proxy as 'this' in the
    // invoked function.
    LoadGlobal();

    // Load the arguments.
    int arg_count = args->length();
    for (int i = 0; i < arg_count; i++) {
      LoadAndSpill(args->at(i));
    }

    // Setup the receiver register and call the IC initialization code.
    __ li(a2, Operand(var->name()));
    InLoopFlag in_loop = loop_nesting() > 0 ? IN_LOOP : NOT_IN_LOOP;
    Handle<Code> stub = ComputeCallInitialize(arg_count, in_loop);
    CodeForSourcePosition(node->position());
    frame_->CallCodeObject(stub, RelocInfo::CODE_TARGET_CONTEXT,
                           arg_count + 1);
    __ lw(cp, frame_->Context());
    // Remove the function from the stack.
    frame_->EmitPush(v0);

  } else if (var != NULL && var->slot() != NULL &&
             var->slot()->type() == Slot::LOOKUP) {
    // ----------------------------------------------------------------
    // JavaScript example: 'with (obj) foo(1, 2, 3)'  // foo is in obj.
    // ----------------------------------------------------------------

    // Load the function
    frame_->EmitPush(cp);
    __ li(a0, Operand(var->name()));
    frame_->EmitPush(a0);
    frame_->CallRuntime(Runtime::kLoadContextSlot, 2);
    // v0: slot value; v1: receiver

    // Load the receiver.
    // Push the function and receiver on the stack.
    frame_->EmitMultiPushReversed(v0.bit() | v1.bit());

    // Call the function.
    CallWithArguments(args, NO_CALL_FUNCTION_FLAGS, node->position());
    frame_->EmitPush(v0);

  } else if (property != NULL) {
    // Check if the key is a literal string.
    Literal* literal = property->key()->AsLiteral();

    if (literal != NULL && literal->handle()->IsSymbol()) {
      // ------------------------------------------------------------------
      // JavaScript example: 'object.foo(1, 2, 3)' or 'map["key"](1, 2, 3)'
      // ------------------------------------------------------------------

      LoadAndSpill(property->obj());  // Receiver.
      // Load the arguments.
      int arg_count = args->length();
      for (int i = 0; i < arg_count; i++) {
        LoadAndSpill(args->at(i));
      }

      // Set the name register and call the IC initialization code.
      __ li(a2, Operand(literal->handle()));
      InLoopFlag in_loop = loop_nesting() > 0 ? IN_LOOP : NOT_IN_LOOP;
      Handle<Code> stub = ComputeCallInitialize(arg_count, in_loop);
      CodeForSourcePosition(node->position());
      frame_->CallCodeObject(stub, RelocInfo::CODE_TARGET, arg_count + 1);
      __ lw(cp, frame_->Context());
      frame_->EmitPush(v0);

    } else {
      UNIMPLEMENTED_MIPS();
      __ break_(__LINE__);
    }

  } else {
    // --------------------------------------------------------
    // JavaScript example: 'foo(1, 2, 3)'  // foo is not global
    // --------------------------------------------------------

    // Load the function.
    LoadAndSpill(function);

    // Pass the global proxy as the receiver.
    LoadGlobalReceiver(a0);

    // Call the function (and allocate args slots).
    CallWithArguments(args, NO_CALL_FUNCTION_FLAGS, node->position());
    frame_->EmitPush(v0);
  }

  ASSERT(frame_->height() == original_height + 1);
}


void CodeGenerator::VisitCallNew(CallNew* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope;
  Comment cmnt(masm_, "[ CallNew");

  // According to ECMA-262, section 11.2.2, page 44, the function
  // expression in new calls must be evaluated before the
  // arguments. This is different from ordinary calls, where the
  // actual function to call is resolved after the arguments have been
  // evaluated.


  // Compute function to call and use the global object as the
  // receiver. There is no need to use the global proxy here because
  // it will always be replaced with a newly allocated object.
  LoadAndSpill(node->expression());
  LoadGlobal();

  ZoneList<Expression*>* args = node->arguments();
  int arg_count = args->length();
  // Push the arguments ("left-to-right") on the stack.
  for (int i = 0; i < arg_count; i++) {
    LoadAndSpill(args->at(i));
  }

  // a0: the number of arguments.
  __ li(a0, Operand(arg_count));
  // Load the function into a1 as per calling convention.
  __ lw(a1, frame_->ElementAt(arg_count + 1));

  // Call the construct call builtin that handles allocation and
  // constructor invocation.
  CodeForSourcePosition(node->position());
  Handle<Code> ic(Builtins::builtin(Builtins::JSConstructCall));
  frame_->CallCodeObject(ic,
                         RelocInfo::CONSTRUCT_CALL,
                         arg_count + 1);
  // Discard old TOS value and push v0 on the stack (same as Pop(), push(v0)).
  __ sw(v0, frame_->Top());
  ASSERT(frame_->height() == original_height + 1);
}


void CodeGenerator::GenerateClassOf(ZoneList<Expression*>* args) {
//  __ break_(__LINE__);
  VirtualFrame::SpilledScope spilled_scope;
  ASSERT(args->length() == 1);
  JumpTarget leave, null, function, non_function_constructor;

  // Load the object into a0.
  LoadAndSpill(args->at(0));
  frame_->EmitPop(a0);

  // If the object is a smi, we return null.
  __ And(t0, a0, Operand(kSmiTagMask));
  null.Branch(eq, t0, Operand(zero_reg), no_hint);

  // Check that the object is a JS object but take special care of JS
  // functions to make sure they have 'Function' as their class.
  __ GetObjectType(a0, a0, a1);
  null.Branch(less, a1, Operand(FIRST_JS_OBJECT_TYPE), no_hint);

  // As long as JS_FUNCTION_TYPE is the last instance type and it is
  // right after LAST_JS_OBJECT_TYPE, we can avoid checking for
  // LAST_JS_OBJECT_TYPE.
  ASSERT(LAST_TYPE == JS_FUNCTION_TYPE);
  ASSERT(JS_FUNCTION_TYPE == LAST_JS_OBJECT_TYPE + 1);
  function.Branch(eq, a1, Operand(JS_FUNCTION_TYPE), no_hint);

  // Check if the constructor in the map is a function.
  __ lw(a0, FieldMemOperand(a0, Map::kConstructorOffset));
  __ GetObjectType(a0, a1, a1);
  non_function_constructor.Branch(ne, a1, Operand(JS_FUNCTION_TYPE), no_hint);

  // The a0 register now contains the constructor function. Grab the
  // instance class name from there.
  __ lw(a0, FieldMemOperand(a0, JSFunction::kSharedFunctionInfoOffset));
  __ lw(v0, FieldMemOperand(a0, SharedFunctionInfo::kInstanceClassNameOffset));
  frame_->EmitPush(v0);
  leave.Jump();

  // Functions have class 'Function'.
  function.Bind();
  __ li(v0, Operand(Factory::function_class_symbol()));
  frame_->EmitPush(v0);
  leave.Jump();

  // Objects with a non-function constructor have class 'Object'.
  non_function_constructor.Bind();
  __ li(v0, Operand(Factory::Object_symbol()));
  frame_->EmitPush(v0);
  leave.Jump();

  // Non-JS objects have class null.
  null.Bind();
  __ LoadRoot(v0, Heap::kNullValueRootIndex);
  frame_->EmitPush(v0);

  // All done.
  leave.Bind();
}


void CodeGenerator::GenerateValueOf(ZoneList<Expression*>* args) {
__ break_(__LINE__);
  VirtualFrame::SpilledScope spilled_scope;
  ASSERT(args->length() == 1);
  JumpTarget leave;
  LoadAndSpill(args->at(0));
  frame_->EmitPop(a0);  // r0 contains object.
  // if (object->IsSmi()) return the object.
  __ And(t0, a0, Operand(kSmiTagMask));
  leave.Branch(eq, t0, Operand(zero_reg));
  // It is a heap object - get map. If (!object->IsJSValue()) return the object.
  __ GetObjectType(a0, a1, a1);
  leave.Branch(ne, a1, Operand(JS_VALUE_TYPE));
  // Load the value.
  __ lw(v0, FieldMemOperand(a0, JSValue::kValueOffset));
  leave.Bind();
  frame_->EmitPush(v0);
}


void CodeGenerator::GenerateSetValueOf(ZoneList<Expression*>* args) {
  VirtualFrame::SpilledScope spilled_scope;
  ASSERT(args->length() == 2);
  JumpTarget leave;
  LoadAndSpill(args->at(0));  // Load the object.
  LoadAndSpill(args->at(1));  // Load the value.
  frame_->EmitPop(a0);  // a0 contains value
  frame_->EmitPop(a1);  // a1 contains object
  // if (object->IsSmi()) return object.
  __ And(t1, a1, Operand(kSmiTagMask));
  leave.Branch(eq, t1, Operand(zero_reg), no_hint);
  // It is a heap object - get map. If (!object->IsJSValue()) return the object.
  __ GetObjectType(a1, a2, a2);
  leave.Branch(ne, a2, Operand(JS_VALUE_TYPE), no_hint);
  // Store the value.
  __ sw(v0, FieldMemOperand(a1, JSValue::kValueOffset));
  // Update the write barrier.
  __ li(a2, Operand(JSValue::kValueOffset - kHeapObjectTag));
  __ RecordWrite(a1, a2, a3);
  // Leave.
  leave.Bind();
  frame_->EmitPush(v0);
}


void CodeGenerator::GenerateIsSmi(ZoneList<Expression*>* args) {
__ break_(__LINE__);
  VirtualFrame::SpilledScope spilled_scope;
  ASSERT(args->length() == 1);
  LoadAndSpill(args->at(0));
  frame_->EmitPop(t0);
  __ And(condReg1, t0, Operand(kSmiTagMask));
  __ mov(condReg2, zero_reg);
  cc_reg_ = eq;
}


void CodeGenerator::GenerateLog(ZoneList<Expression*>* args) {
  UNIMPLEMENTED_MIPS();
  __ break_(__LINE__);
}


void CodeGenerator::GenerateIsNonNegativeSmi(ZoneList<Expression*>* args) {
  VirtualFrame::SpilledScope spilled_scope;
  ASSERT(args->length() == 1);
  LoadAndSpill(args->at(0));
  frame_->EmitPop(t0);
  __ And(condReg1, a0, Operand(kSmiTagMask | 0x80000000u));
  __ mov(condReg2, zero_reg);
  cc_reg_ = eq;
}


void CodeGenerator::GenerateMathPow(ZoneList<Expression*>* args) {
  UNIMPLEMENTED_MIPS();
  __ break_(__LINE__);
}


void CodeGenerator::GenerateMathCos(ZoneList<Expression*>* args) {
  UNIMPLEMENTED_MIPS();
  __ break_(__LINE__);
}


void CodeGenerator::GenerateMathSin(ZoneList<Expression*>* args) {
  UNIMPLEMENTED_MIPS();
  __ break_(__LINE__);
}


void CodeGenerator::GenerateMathSqrt(ZoneList<Expression*>* args) {
  UNIMPLEMENTED_MIPS();
  __ break_(__LINE__);
}


void CodeGenerator::GenerateFastCharCodeAt(ZoneList<Expression*>* args) {
  UNIMPLEMENTED_MIPS();
  __ break_(__LINE__);
  frame_->EmitPush(zero_reg);
}


void CodeGenerator::GenerateCharFromCode(ZoneList<Expression*>* args) {
  UNIMPLEMENTED_MIPS();
  __ break_(__LINE__);
  frame_->EmitPush(zero_reg);
}


void CodeGenerator::GenerateIsArray(ZoneList<Expression*>* args) {
  __ break_(__LINE__);
  VirtualFrame::SpilledScope spilled_scope;
  ASSERT(args->length() == 1);
  LoadAndSpill(args->at(0));
  JumpTarget answer;

  // We need the condition to be not_equal is the object is a smi.
  frame_->EmitPop(a0);
  __ And(t0, a0, Operand(kSmiTagMask));
  __ Xor(condReg1, t0, Operand(kSmiTagMask));
  __ mov(condReg2, zero_reg);
  answer.Branch(eq, t0, Operand(zero_reg));
  // It is a heap object - get the map. Check if the object is a JS array.
  __ GetObjectType(a0, t1, condReg1);
  __ li(condReg2, Operand(JS_ARRAY_TYPE));
  answer.Bind();
  cc_reg_ = eq;
}


void CodeGenerator::GenerateIsRegExp(ZoneList<Expression*>* args) {
  __ break_(__LINE__);
  VirtualFrame::SpilledScope spilled_scope;
  ASSERT(args->length() == 1);
  LoadAndSpill(args->at(0));
  JumpTarget answer;
  // We need the condition to be not_equal is the object is a smi.
  frame_->EmitPop(a0);
  __ And(t0, a0, Operand(kSmiTagMask));
  __ Xor(condReg1, t0, Operand(kSmiTagMask));
  __ mov(condReg2, zero_reg);
  answer.Branch(eq, t0, Operand(zero_reg));
  // It is a heap object - get the map. Check if the object is a regexp.
  __ GetObjectType(a0, t1, condReg1);
  __ li(condReg2, Operand(JS_REGEXP_TYPE));
  answer.Bind();
  cc_reg_ = eq;
}


void CodeGenerator::GenerateIsConstructCall(ZoneList<Expression*>* args) {
  VirtualFrame::SpilledScope spilled_scope;
  ASSERT(args->length() == 0);

  // Get the frame pointer for the calling frame.
  __ lw(a2, MemOperand(fp, StandardFrameConstants::kCallerFPOffset));

  // Skip the arguments adaptor frame if it exists.
  Label check_frame_marker;
  __ lw(a1, MemOperand(a2, StandardFrameConstants::kContextOffset));
  __ Branch(ne, &check_frame_marker,
              a1, Operand(Smi::FromInt(StackFrame::ARGUMENTS_ADAPTOR)));
  __ lw(a2, MemOperand(a2, StandardFrameConstants::kCallerFPOffset));

  // Check the marker in the calling frame.
  __ bind(&check_frame_marker);
  __ lw(condReg1, MemOperand(a2, StandardFrameConstants::kMarkerOffset));
  __ li(condReg2, Operand(Smi::FromInt(StackFrame::CONSTRUCT)));
  cc_reg_ = eq;
}


void CodeGenerator::GenerateArgumentsLength(ZoneList<Expression*>* args) {
  VirtualFrame::SpilledScope spilled_scope;
  ASSERT(args->length() == 0);

  // Seed the result with the formal parameters count, which will be used
  // in case no arguments adaptor frame is found below the current frame.
  __ li(a0, Operand(Smi::FromInt(scope()->num_parameters())));

  // Call the shared stub to get to arguments[key].
  ArgumentsAccessStub stub(ArgumentsAccessStub::READ_LENGTH);
  frame_->CallStub(&stub, 0);
  frame_->EmitPush(v0);
}


void CodeGenerator::GenerateArguments(ZoneList<Expression*>* args) {
  VirtualFrame::SpilledScope spilled_scope;
  ASSERT(args->length() == 1);

  // Satisfy contract with ArgumentsAccessStub:
  // Load the key into r1 and the formal parameters count into r0.
  LoadAndSpill(args->at(0));
  frame_->EmitPop(a1);
  __ li(a0, Operand(Smi::FromInt(scope()->num_parameters())));

  // Call the shared stub to get to arguments[key].
  ArgumentsAccessStub stub(ArgumentsAccessStub::READ_ELEMENT);
  frame_->CallStub(&stub, 0);
  frame_->EmitPush(v0);
}


void CodeGenerator::GenerateRandomPositiveSmi(ZoneList<Expression*>* args) {
  UNIMPLEMENTED_MIPS();
  __ break_(__LINE__);
}


void CodeGenerator::GenerateObjectEquals(ZoneList<Expression*>* args) {
__ break_(__LINE__);
  VirtualFrame::SpilledScope spilled_scope;
  ASSERT(args->length() == 2);

  // Load the two objects into registers and perform the comparison.
  LoadAndSpill(args->at(0));
  LoadAndSpill(args->at(1));
  frame_->EmitPop(a0);
  frame_->EmitPop(a1);
  __ mov(condReg1, a0);
  __ mov(condReg2, a1);
  cc_reg_ = eq;
}


void CodeGenerator::GenerateIsObject(ZoneList<Expression*>* args) {
  // This generates a fast version of:
  // (typeof(arg) === 'object' || %_ClassOf(arg) == 'RegExp')
  VirtualFrame::SpilledScope spilled_scope;
  ASSERT(args->length() == 1);
  LoadAndSpill(args->at(0));
  frame_->EmitPop(a1);
  __ And(t1, a1, Operand(kSmiTagMask));
  false_target()->Branch(eq, t1, Operand(zero_reg));

  __ LoadRoot(t0, Heap::kNullValueRootIndex);
  true_target()->Branch(eq, a1, Operand(t0));

  Register map_reg = a2;
  __ lw(map_reg, FieldMemOperand(a1, HeapObject::kMapOffset));
  // Undetectable objects behave like undefined when tested with typeof.
  __ lbu(a1, FieldMemOperand(map_reg, Map::kBitFieldOffset));
  __ And(t1, a1, Operand(1 << Map::kIsUndetectable));
  false_target()->Branch(eq, t1, Operand(1 << Map::kIsUndetectable));

  __ lbu(t1, FieldMemOperand(map_reg, Map::kInstanceTypeOffset));
  false_target()->Branch(less, t1, Operand(FIRST_JS_OBJECT_TYPE));
  __ mov(condReg1, t1);
  __ li(condReg2, Operand(LAST_JS_OBJECT_TYPE));
  cc_reg_ = less_equal;
}


void CodeGenerator::GenerateIsFunction(ZoneList<Expression*>* args) {
  // This generates a fast version of:
  // (%_ClassOf(arg) === 'Function')
  VirtualFrame::SpilledScope spilled_scope;
  ASSERT(args->length() == 1);
  LoadAndSpill(args->at(0));
  frame_->EmitPop(a0);
  __ And(t0, a0, Operand(kSmiTagMask));
  false_target()->Branch(eq, t0, Operand(zero_reg));
  Register map_reg = a2;
  __ GetObjectType(a0, map_reg, a1);
  __ mov(condReg1, a1);
  __ li(condReg2, Operand(JS_FUNCTION_TYPE));
  cc_reg_ = eq;
}


void CodeGenerator::GenerateIsUndetectableObject(ZoneList<Expression*>* args) {
__ break_(__LINE__);
  VirtualFrame::SpilledScope spilled_scope;
  ASSERT(args->length() == 1);
  LoadAndSpill(args->at(0));
  frame_->EmitPop(a0);
  __ And(t0, a0, Operand(kSmiTagMask));
  false_target()->Branch(eq, t0, Operand(zero_reg));
  __ lw(a1, FieldMemOperand(a0, HeapObject::kMapOffset));
  __ lbu(a1, FieldMemOperand(a1, Map::kBitFieldOffset));
  __ And(condReg1, a1, Operand(1 << Map::kIsUndetectable));
  __ mov(condReg2, zero_reg);
  cc_reg_ = ne;
}


void CodeGenerator::GenerateStringAdd(ZoneList<Expression*>* args) {
  Comment cmnt(masm_, "[ GenerateStringAdd");
  ASSERT_EQ(2, args->length());

  Load(args->at(0));
  Load(args->at(1));

  StringAddStub stub(NO_STRING_ADD_FLAGS);
  frame_->CallStub(&stub, 2);
  frame_->EmitPush(v0);
}


void CodeGenerator::GenerateSubString(ZoneList<Expression*>* args) {
  UNIMPLEMENTED_MIPS();
  __ break_(__LINE__);
}


void CodeGenerator::GenerateStringCompare(ZoneList<Expression*>* args) {
__ break_(__LINE__);
  ASSERT_EQ(2, args->length());

  Load(args->at(0));
  Load(args->at(1));

  StringCompareStub stub;
  __ break_(__LINE__);
  frame_->CallStub(&stub, 2);
  frame_->EmitPush(v0);
}


void CodeGenerator::GenerateRegExpExec(ZoneList<Expression*>* args) {
  UNIMPLEMENTED_MIPS();
  __ break_(__LINE__);
}


void CodeGenerator::GenerateNumberToString(ZoneList<Expression*>* args) {
  ASSERT_EQ(args->length(), 1);

  // Load the argument on the stack and jump to the runtime.
  Load(args->at(0));

  NumberToStringStub stub;
  frame_->CallStub(&stub, 1);
  frame_->EmitPush(v0);
}


void CodeGenerator::VisitCallRuntime(CallRuntime* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope;
  if (CheckForInlineRuntimeCall(node)) {
    ASSERT((has_cc() && frame_->height() == original_height) ||
           (!has_cc() && frame_->height() == original_height + 1));
    return;
  }

  ZoneList<Expression*>* args = node->arguments();
  Comment cmnt(masm_, "[ CallRuntime");
  Runtime::Function* function = node->function();

  int arg_count = args->length();

  if (function == NULL) {
    // Prepare stack for calling JS runtime function.
    // Push the builtins object found in the current global object.
    __ lw(a1, GlobalObject());
    __ lw(a0, FieldMemOperand(a1, GlobalObject::kBuiltinsOffset));
    frame_->EmitPush(a0);
  }

  // Push the arguments ("left-to-right").
  for (int i = 0; i < arg_count; i++) {
    LoadAndSpill(args->at(i));
  }

  if (function == NULL) {
    // Call the JS runtime function.
    __ li(a2, Operand(node->name()));
    InLoopFlag in_loop = loop_nesting() > 0 ? IN_LOOP : NOT_IN_LOOP;
    Handle<Code> stub = ComputeCallInitialize(arg_count, in_loop);
    frame_->CallCodeObject(stub, RelocInfo::CODE_TARGET, arg_count + 1);
    __ lw(cp, frame_->Context());
    frame_->EmitPush(v0);
  } else {
    // Call the C runtime function.
    frame_->CallRuntime(function, arg_count);
    frame_->EmitPush(v0);
  }
  ASSERT(frame_->height() == original_height + 1);
}


void CodeGenerator::VisitUnaryOperation(UnaryOperation* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope;
  Comment cmnt(masm_, "[ UnaryOperation");

  Token::Value op = node->op();

  if (op == Token::NOT) {
    // LoadConditionAndSpill reversing the false and true targets.
    LoadConditionAndSpill(node->expression(),
                          false_target(),
                          true_target(),
                          true);
    // LoadCondition may (and usually does) leave a test and branch to
    // be emitted by the caller.  In that case, negate the condition.
    if (has_cc()) cc_reg_ = NegateCondition(cc_reg_);

  } else if (op == Token::DELETE) {
    UNIMPLEMENTED_MIPS();
    __ break_(__LINE__);

  } else if (op == Token::TYPEOF) {
    // Special case for loading the typeof expression; see comment on
    // LoadTypeofExpression().
    LoadTypeofExpression(node->expression());
    frame_->CallRuntime(Runtime::kTypeof, 1);
    frame_->EmitPush(v0);  // v0 holds the result.

  } else {
    bool overwrite =
        (node->expression()->AsBinaryOperation() != NULL &&
         node->expression()->AsBinaryOperation()->ResultOverwriteAllowed());
    LoadAndSpill(node->expression());
    frame_->EmitPop(a0);
    switch (op) {
      case Token::NOT:
      case Token::DELETE:
      case Token::TYPEOF:
        UNREACHABLE();  // Handled above.
        break;

      case Token::SUB: {
        UNIMPLEMENTED_MIPS();
        __ break_(__LINE__);
        break;
      }

      case Token::BIT_NOT: {
        JumpTarget smi_label;
        JumpTarget continue_label;
        __ And(t0, a0, Operand(kSmiTagMask));
        smi_label.Branch(eq, t0, Operand(zero_reg));

        GenericUnaryOpStub stub(Token::BIT_NOT, overwrite);
        frame_->CallStub(&stub, 0);
        continue_label.Jump();

        smi_label.Bind();
        // We have a smi. Invert all bits except bit 0.
        __ Xor(v0, a0, 0xfffffffe);
        continue_label.Bind();
        break;
      }

      case Token::VOID:
        // Just load the value in v0, which will be pushed next.
        __ LoadRoot(v0, Heap::kUndefinedValueRootIndex);
        break;

      case Token::ADD: {
        UNIMPLEMENTED_MIPS();
        __ break_(__LINE__);
        break;
      }
      default:
        UNREACHABLE();
    }
    frame_->EmitPush(v0);  // v0 holds the result.
  }
  ASSERT(!has_valid_frame() ||
         (has_cc() && frame_->height() == original_height) ||
         (!has_cc() && frame_->height() == original_height + 1));
}


void CodeGenerator::VisitCountOperation(CountOperation* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope;
  Comment cmnt(masm_, "[ CountOperation");

  bool is_postfix = node->is_postfix();
  bool is_increment = node->op() == Token::INC;

  Variable* var = node->expression()->AsVariableProxy()->AsVariable();
  bool is_const = (var != NULL && var->mode() == Variable::CONST);

  // Postfix: Make room for the result.
  if (is_postfix) {
    __ mov(v0, zero_reg);
    frame_->EmitPush(v0);
  }

  { Reference target(this, node->expression(), !is_const);
    if (target.is_illegal()) {
      // Spoof the virtual frame to have the expected height (one higher
      // than on entry).
      if (!is_postfix) {
        __ mov(v0, zero_reg);
        frame_->EmitPush(v0);
      }
      ASSERT(frame_->height() == original_height + 1);
      return;
    }
    // Get the old value in a0.
    target.GetValueAndSpill();
    frame_->EmitPop(a0);

    JumpTarget slow;
    JumpTarget exit;

    // Check for smi operand.
    __ And(t0, a0, Operand(kSmiTagMask));
    slow.Branch(ne, t0, Operand(zero_reg), no_hint);

    // Postfix: Store the old value as the result.
    if (is_postfix) {
      __ sw(a0, frame_->ElementAt(target.size()));
    }

    // Perform optimistic increment/decrement and check for overflow.
    // If we don't overflow we are done.
    if (is_increment) {
      __ Addu(v0, a0, Operand(Smi::FromInt(1)));
      exit.Branch(ne, a0, Operand(Smi::kMaxValue), no_hint);
    } else {
      __ Addu(v0, a0, Operand((Smi::FromInt(-1))));
      exit.Branch(ne, a0, Operand(Smi::kMinValue), no_hint);
    }

    // We had an overflow.
    // Slow case: Convert to number.
    // a0 still holds the original value.
    slow.Bind();
    UNIMPLEMENTED_MIPS();
    __ break_(__LINE__);   // We should not come here yet.

    // Store the new value in the target if not const.
    exit.Bind();
    frame_->EmitPush(v0);
    if (!is_const) target.SetValue(NOT_CONST_INIT);
  }

  // Postfix: Discard the new value and use the old.
  if (is_postfix) frame_->EmitPop(v0);
  ASSERT(frame_->height() == original_height + 1);
}


void CodeGenerator::VisitBinaryOperation(BinaryOperation* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope;
  Comment cmnt(masm_, "[ BinaryOperation");
  Token::Value op = node->op();

  // According to ECMA-262 section 11.11, page 58, the binary logical
  // operators must yield the result of one of the two expressions
  // before any ToBoolean() conversions. This means that the value
  // produced by a && or || operator is not necessarily a boolean.

  // NOTE: If the left hand side produces a materialized value (not in
  // the CC register), we force the right hand side to do the
  // same. This is necessary because we may have to branch to the exit
  // after evaluating the left hand side (due to the shortcut
  // semantics), but the compiler must (statically) know if the result
  // of compiling the binary operation is materialized or not.

  if (op == Token::AND) {
    JumpTarget is_true;
    LoadConditionAndSpill(node->left(),
                          &is_true,
                          false_target(),
                          false);
    if (has_valid_frame() && !has_cc()) {
      // The left-hand side result is on top of the virtual frame.
      JumpTarget pop_and_continue;
      JumpTarget exit;

      __ lw(a0, frame_->Top());  // Duplicate the stack top.
      frame_->EmitPush(a0);
      // Avoid popping the result if it converts to 'false' using the
      // standard ToBoolean() conversion as described in ECMA-262,
      // section 9.2, page 30.
      ToBoolean(&pop_and_continue, &exit);
      Branch(false, &exit);

      // Pop the result of evaluating the first part.
      pop_and_continue.Bind();
      frame_->EmitPop(t0);

      // Evaluate right side expression.
      is_true.Bind();
      LoadAndSpill(node->right());

      // Exit (always with a materialized value).
      exit.Bind();
    } else if (has_cc() || is_true.is_linked()) {
      // The left-hand side is either (a) partially compiled to
      // control flow with a final branch left to emit or (b) fully
      // compiled to control flow and possibly true.
      if (has_cc()) {
        Branch(false, false_target());
      }
      is_true.Bind();
      LoadConditionAndSpill(node->right(),
                            true_target(),
                            false_target(),
                            false);
    } else {
      // Nothing to do.
      ASSERT(!has_valid_frame() && !has_cc() && !is_true.is_linked());
    }

  } else if (op == Token::OR) {
    JumpTarget is_false;
    LoadConditionAndSpill(node->left(),
                          true_target(),
                          &is_false,
                          false);
    if (has_valid_frame() && !has_cc()) {
      // The left-hand side result is on top of the virtual frame.
      JumpTarget pop_and_continue;
      JumpTarget exit;

      __ lw(a0, frame_->Top());
      frame_->EmitPush(a0);
      // Avoid popping the result if it converts to 'true' using the
      // standard ToBoolean() conversion as described in ECMA-262,
      // section 9.2, page 30.
      ToBoolean(&exit, &pop_and_continue);
      Branch(true, &exit);

      // Pop the result of evaluating the first part.
      pop_and_continue.Bind();
      frame_->EmitPop(a0);

      // Evaluate right side expression.
      is_false.Bind();
      LoadAndSpill(node->right());

      // Exit (always with a materialized value).
      exit.Bind();
    } else if (has_cc() || is_false.is_linked()) {
      // The left-hand side is either (a) partially compiled to
      // control flow with a final branch left to emit or (b) fully
      // compiled to control flow and possibly false.
      if (has_cc()) {
        Branch(true, true_target());
      }
      is_false.Bind();
      LoadConditionAndSpill(node->right(),
                            true_target(),
                            false_target(),
                            false);
    } else {
      // Nothing to do.
      ASSERT(!has_valid_frame() && !has_cc() && !is_false.is_linked());
    }

  } else {
    // Optimize for the case where (at least) one of the expressions
    // is a literal small integer.
    Literal* lliteral = node->left()->AsLiteral();
    Literal* rliteral = node->right()->AsLiteral();
    // NOTE: The code below assumes that the slow cases (calls to runtime)
    // never return a constant/immutable object.
    bool overwrite_left =
        (node->left()->AsBinaryOperation() != NULL &&
         node->left()->AsBinaryOperation()->ResultOverwriteAllowed());
    bool overwrite_right =
        (node->right()->AsBinaryOperation() != NULL &&
         node->right()->AsBinaryOperation()->ResultOverwriteAllowed());

    if (rliteral != NULL && rliteral->handle()->IsSmi()) {
      LoadAndSpill(node->left());
      SmiOperation(node->op(),
                   rliteral->handle(),
                   false,
                   overwrite_right ? OVERWRITE_RIGHT : NO_OVERWRITE);

    } else if (lliteral != NULL && lliteral->handle()->IsSmi()) {
      LoadAndSpill(node->right());
      SmiOperation(node->op(),
                   lliteral->handle(),
                   true,
                   overwrite_left ? OVERWRITE_LEFT : NO_OVERWRITE);
    } else {
      OverwriteMode overwrite_mode = NO_OVERWRITE;
      if (overwrite_left) {
        overwrite_mode = OVERWRITE_LEFT;
      } else if (overwrite_right) {
        overwrite_mode = OVERWRITE_RIGHT;
      }
      LoadAndSpill(node->left());
      LoadAndSpill(node->right());
      GenericBinaryOperation(node->op(), overwrite_mode);
    }
    frame_->EmitPush(v0);
  }
  ASSERT(!has_valid_frame() ||
         (has_cc() && frame_->height() == original_height) ||
         (!has_cc() && frame_->height() == original_height + 1));
}


void CodeGenerator::VisitThisFunction(ThisFunction* node) {
  UNIMPLEMENTED_MIPS();
  __ break_(__LINE__);
}


void CodeGenerator::VisitCompareOperation(CompareOperation* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope;
  Comment cmnt(masm_, "[ CompareOperation");

  // Get the expressions from the node.
  Expression* left = node->left();
  Expression* right = node->right();
  Token::Value op = node->op();

  // To make null checks efficient, we check if either left or right is the
  // literal 'null'. If so, we optimize the code by inlining a null check
  // instead of calling the (very) general runtime routine for checking
  // equality.
  if (op == Token::EQ || op == Token::EQ_STRICT) {
    bool left_is_null =
        left->AsLiteral() != NULL && left->AsLiteral()->IsNull();
    bool right_is_null =
        right->AsLiteral() != NULL && right->AsLiteral()->IsNull();
    // The 'null' value can only be equal to 'null' or 'undefined'.
    if (left_is_null || right_is_null) {
      LoadAndSpill(left_is_null ? right : left);
      frame_->EmitPop(condReg1);
      __ LoadRoot(condReg2, Heap::kNullValueRootIndex);

      // The 'null' value is only equal to 'undefined' if using non-strict
      // comparisons.
      if (op != Token::EQ_STRICT) {
        true_target()->Branch(eq, condReg1, Operand(condReg2), no_hint);

        __ LoadRoot(condReg2, Heap::kUndefinedValueRootIndex);
        true_target()->Branch(eq, condReg1, Operand(condReg2), no_hint);

        __ And(condReg2, condReg1, kSmiTagMask);
        false_target()->Branch(eq, condReg2, Operand(zero_reg), no_hint);

        // It can be an undetectable object.
        __ lw(condReg1, FieldMemOperand(condReg1, HeapObject::kMapOffset));
        __ lbu(condReg1, FieldMemOperand(condReg1, Map::kBitFieldOffset));
        __ And(condReg1, condReg1, 1 << Map::kIsUndetectable);
        __ li(condReg2, Operand(1 << Map::kIsUndetectable));
      }

      // We don't need to load anyting in condReg1 and condReg2 as they are
      // already correctly loaded.
      cc_reg_ = eq;
      ASSERT(has_cc() && frame_->height() == original_height);
      return;
    }
  }

  // To make typeof testing for natives implemented in JavaScript really
  // efficient, we generate special code for expressions of the form:
  // 'typeof <expression> == <string>'.
  UnaryOperation* operation = left->AsUnaryOperation();
  if ((op == Token::EQ || op == Token::EQ_STRICT) &&
      (operation != NULL && operation->op() == Token::TYPEOF) &&
      (right->AsLiteral() != NULL &&
       right->AsLiteral()->handle()->IsString())) {
    Handle<String> check(String::cast(*right->AsLiteral()->handle()));

    // Load the operand, move it to register condReg1.
    LoadTypeofExpression(operation->expression());
    frame_->EmitPop(condReg1);

    if (check->Equals(Heap::number_symbol())) {
      __ And(condReg2, condReg1, kSmiTagMask);
      true_target()->Branch(eq, condReg2, Operand(zero_reg), no_hint);
      __ lw(condReg1, FieldMemOperand(condReg1, HeapObject::kMapOffset));
      __ LoadRoot(condReg2, Heap::kHeapNumberMapRootIndex);
      cc_reg_ = eq;

    } else if (check->Equals(Heap::string_symbol())) {
      __ And(condReg2, condReg1, kSmiTagMask);
      false_target()->Branch(eq, condReg2, Operand(zero_reg), no_hint);

      __ lw(condReg1, FieldMemOperand(condReg1, HeapObject::kMapOffset));

      // It can be an undetectable string object.
      __ lbu(condReg2, FieldMemOperand(condReg1, Map::kBitFieldOffset));
      __ And(condReg2, condReg2, 1 << Map::kIsUndetectable);
      false_target()->Branch(eq, condReg2, Operand(1 << Map::kIsUndetectable),
          no_hint);

      __ lbu(condReg1, FieldMemOperand(condReg1, Map::kInstanceTypeOffset));
      __ li(condReg2, Operand(FIRST_NONSTRING_TYPE));
      cc_reg_ = less;

    } else if (check->Equals(Heap::boolean_symbol())) {
      __ LoadRoot(condReg2, Heap::kTrueValueRootIndex);
      true_target()->Branch(eq, condReg1, Operand(condReg2), no_hint);
      __ LoadRoot(condReg2, Heap::kFalseValueRootIndex);
      cc_reg_ = eq;

    } else if (check->Equals(Heap::undefined_symbol())) {
      __ LoadRoot(condReg2, Heap::kUndefinedValueRootIndex);
      true_target()->Branch(eq, condReg1, Operand(condReg2), no_hint);

      __ And(condReg2, condReg1, kSmiTagMask);
      false_target()->Branch(eq, condReg2, Operand(zero_reg), no_hint);

      // It can be an undetectable object.
      __ lw(condReg1, FieldMemOperand(condReg1, HeapObject::kMapOffset));
      __ lbu(condReg1, FieldMemOperand(condReg1, Map::kBitFieldOffset));
      __ And(condReg1, condReg1, 1 << Map::kIsUndetectable);
      __ li(condReg2, Operand(1 << Map::kIsUndetectable));

      cc_reg_ = eq;

    } else if (check->Equals(Heap::function_symbol())) {
      UNIMPLEMENTED_MIPS();
      __ break_(__LINE__);

    } else if (check->Equals(Heap::object_symbol())) {
      UNIMPLEMENTED_MIPS();
      __ break_(__LINE__);

    } else {
      // Uncommon case: typeof testing against a string literal that is
      // never returned from the typeof operator.
      false_target()->Jump();
    }
    ASSERT(!has_valid_frame() ||
           (has_cc() && frame_->height() == original_height));
    return;
  }

  switch (op) {
    case Token::EQ:
      Comparison(eq, left, right, false);
      break;

    case Token::LT:
      Comparison(less, left, right);
      break;

    case Token::GT:
      Comparison(greater, left, right);
      break;

    case Token::LTE:
      Comparison(less_equal, left, right);
      break;

    case Token::GTE:
      Comparison(greater_equal, left, right);
      break;

    case Token::EQ_STRICT:
      Comparison(eq, left, right, true);
      break;

    case Token::IN: {
      LoadAndSpill(left);
      LoadAndSpill(right);
      frame_->InvokeBuiltin(Builtins::IN, CALL_JS, 2);
      frame_->EmitPush(v0);
      break;
    }

    case Token::INSTANCEOF: {
      LoadAndSpill(left);
      LoadAndSpill(right);
      InstanceofStub stub;
      frame_->CallStub(&stub, 2);
      // At this point if instanceof succeeded then v0 == 0.
      __ mov(condReg1, v0);
      __ mov(condReg2, zero_reg);
      cc_reg_ = eq;
      break;
    }

    default:
      UNREACHABLE();
  }
  ASSERT((has_cc() && frame_->height() == original_height) ||
         (!has_cc() && frame_->height() == original_height + 1));
}


void CodeGenerator::EmitKeyedLoad(bool is_global) {
  Comment cmnt(masm_, "[ Load from keyed Property");
  Handle<Code> ic(Builtins::builtin(Builtins::KeyedLoadIC_Initialize));
  RelocInfo::Mode rmode = is_global
                          ? RelocInfo::CODE_TARGET_CONTEXT
                          : RelocInfo::CODE_TARGET;
  frame_->CallCodeObject(ic, rmode, 0);
}


#ifdef DEBUG
bool CodeGenerator::HasValidEntryRegisters() { return true; }
#endif


#undef __
#define __ ACCESS_MASM(masm)

// -----------------------------------------------------------------------------
// Reference support.

Reference::Reference(CodeGenerator* cgen,
                     Expression* expression,
                     bool persist_after_get)
    : cgen_(cgen),
      expression_(expression),
      type_(ILLEGAL),
      persist_after_get_(persist_after_get) {
  cgen->LoadReference(this);
}


Reference::~Reference() {
  ASSERT(is_unloaded() || is_illegal());
}


Handle<String> Reference::GetName() {
  ASSERT(type_ == NAMED);
  Property* property = expression_->AsProperty();
  if (property == NULL) {
    // Global variable reference treated as a named property reference.
    VariableProxy* proxy = expression_->AsVariableProxy();
    ASSERT(proxy->AsVariable() != NULL);
    ASSERT(proxy->AsVariable()->is_global());
    return proxy->name();
  } else {
    Literal* raw_name = property->key()->AsLiteral();
    ASSERT(raw_name != NULL);
    return Handle<String>(String::cast(*raw_name->handle()));
  }
}


void Reference::GetValue() {
  ASSERT(cgen_->HasValidEntryRegisters());
  ASSERT(!is_illegal());
  ASSERT(!cgen_->has_cc());
  MacroAssembler* masm = cgen_->masm();
  Property* property = expression_->AsProperty();
  if (property != NULL) {
    cgen_->CodeForSourcePosition(property->position());
  }

  switch (type_) {
    case SLOT: {
      Comment cmnt(masm, "[ Load from Slot");
      Slot* slot = expression_->AsVariableProxy()->AsVariable()->slot();
      ASSERT(slot != NULL);
      cgen_->LoadFromSlot(slot, NOT_INSIDE_TYPEOF);
      break;
    }

    case NAMED: {
      VirtualFrame* frame = cgen_->frame();
      Comment cmnt(masm, "[ Load from named Property");
      Handle<String> name(GetName());
      Variable* var = expression_->AsVariableProxy()->AsVariable();
      Handle<Code> ic(Builtins::builtin(Builtins::LoadIC_Initialize));
      // Setup the name register.
      __ li(a2, Operand(name));
      ASSERT(var == NULL || var->is_global());
      RelocInfo::Mode rmode = (var == NULL)
                            ? RelocInfo::CODE_TARGET
                            : RelocInfo::CODE_TARGET_CONTEXT;
      frame->CallCodeObject(ic, rmode, 0);
      frame->EmitPush(v0);
      break;
    }

    case KEYED: {
      ASSERT(property != NULL);
      Variable* var = expression_->AsVariableProxy()->AsVariable();
      ASSERT(var == NULL || var->is_global());
      cgen_->EmitKeyedLoad(var != NULL);
      cgen_->frame()->EmitPush(v0);
      break;
    }

    default:
      UNREACHABLE();
  }

  if (!persist_after_get_) {
    cgen_->UnloadReference(this);
  }
}


void Reference::SetValue(InitState init_state) {
  ASSERT(!is_illegal());
  ASSERT(!cgen_->has_cc());
  MacroAssembler* masm = cgen_->masm();
  VirtualFrame* frame = cgen_->frame();
  Property* property = expression_->AsProperty();
  if (property != NULL) {
    cgen_->CodeForSourcePosition(property->position());
  }

  switch (type_) {
    case SLOT: {
      Comment cmnt(masm, "[ Store to Slot");
      Slot* slot = expression_->AsVariableProxy()->AsVariable()->slot();
      cgen_->StoreToSlot(slot, init_state);
      cgen_->UnloadReference(this);
      break;
    }

    case NAMED: {
      Comment cmnt(masm, "[ Store to named Property");
      // Call the appropriate IC code.
      Handle<Code> ic(Builtins::builtin(Builtins::StoreIC_Initialize));
      Handle<String> name(GetName());

      frame->EmitPop(a0);
      frame->EmitPop(a1);
      // Setup the name register.
      __ li(a2, Operand(name));
      frame->CallCodeObject(ic, RelocInfo::CODE_TARGET, 0);
      frame->EmitPush(v0);
      set_unloaded();
      break;
    }

    case KEYED: {
      Comment cmnt(masm, "[ Store to keyed Property");
      Property* property = expression_->AsProperty();
      ASSERT(property != NULL);
      cgen_->CodeForSourcePosition(property->position());

      // Call IC code.
      Handle<Code> ic(Builtins::builtin(Builtins::KeyedStoreIC_Initialize));
      frame->EmitPop(a0);
      frame->CallCodeObject(ic, RelocInfo::CODE_TARGET, 0);
      frame->EmitPush(v0);
      cgen_->UnloadReference(this);
      break;
    }

    default:
      UNREACHABLE();
  }
}


// Takes a Smi and converts to an IEEE 64 bit floating point value in two
// registers.  The format is 1 sign bit, 11 exponent bits (biased 1023) and
// 52 fraction bits (20 in the first word, 32 in the second).  Zeros is a
// scratch register.  Destroys the source register.  No GC occurs during this
// stub so you don't have to set up the frame.
class ConvertToDoubleStub : public CodeStub {
 public:
  ConvertToDoubleStub(Register result_reg_1,
                      Register result_reg_2,
                      Register source_reg,
                      Register scratch_reg)
      : result1_(result_reg_1),
        result2_(result_reg_2),
        source_(source_reg),
        zeros_(scratch_reg) { }

 private:
  Register result1_;
  Register result2_;
  Register source_;
  Register zeros_;

  // Minor key encoding in 16 bits.
  class ModeBits: public BitField<OverwriteMode, 0, 2> {};
  class OpBits: public BitField<Token::Value, 2, 14> {};

  Major MajorKey() { return ConvertToDouble; }
  int MinorKey() {
    // Encode the parameters in a unique 16 bit value.
    return  result1_.code() +
           (result2_.code() << 4) +
           (source_.code() << 8) +
           (zeros_.code() << 12);
  }

  void Generate(MacroAssembler* masm);

  const char* GetName() { return "ConvertToDoubleStub"; }

#ifdef DEBUG
  void Print() { PrintF("ConvertToDoubleStub\n"); }
#endif
};


void ConvertToDoubleStub::Generate(MacroAssembler* masm) {
#ifndef BIG_ENDIAN_FLOATING_POINT
  Register exponent = result1_;
  Register mantissa = result2_;
#else
  Register exponent = result2_;
  Register mantissa = result1_;
#endif
  Label not_special;
  // Convert from Smi to integer.
  __ sra(source_, source_, kSmiTagSize);
  // Move sign bit from source to destination.  This works because the sign bit
  // in the exponent word of the double has the same position and polarity as
  // the 2's complement sign bit in a Smi.
  ASSERT(HeapNumber::kSignMask == 0x80000000u);
  __ And(exponent, source_, Operand(HeapNumber::kSignMask));
  // Subtract from 0 if source was negative.
  __ subu(at, zero_reg, source_);
  __ movn(source_, at, exponent);

  // We have -1, 0 or 1, which we treat specially. Register source_ contains
  // absolute value: it is either equal to 1 (special case of -1 and 1),
  // greater than 1 (not a special case) or less than 1 (special case of 0).
  __ Branch(gt, &not_special, source_, Operand(1));

  // For 1 or -1 we need to or in the 0 exponent (biased to 1023).
  static const uint32_t exponent_word_for_1 =
      HeapNumber::kExponentBias << HeapNumber::kExponentShift;
  // Safe to use 'at' as dest reg here.
  __ Or(at, exponent, Operand(exponent_word_for_1));
  __ movn(exponent, at, source_);  // Write exp when source not 0.
  // 1, 0 and -1 all have 0 for the second word.
  __ mov(mantissa, zero_reg);
  __ Ret();

  __ bind(&not_special);
  // Count leading zeros.
  // Gets the wrong answer for 0, but we already checked for that case above.
  __ clz(zeros_, source_);
  // Compute exponent and or it into the exponent register.
  // We use mantissa as a scratch register here.
  __ li(mantissa, Operand(31 + HeapNumber::kExponentBias));
  __ subu(mantissa, mantissa, zeros_);
  __ sll(mantissa, mantissa, HeapNumber::kExponentShift);
  __ Or(exponent, exponent, mantissa);

  // Shift up the source chopping the top bit off.
  __ Addu(zeros_, zeros_, Operand(1));
  // This wouldn't work for 1.0 or -1.0 as the shift would be 32 which means 0.
  __ sllv(source_, source_, zeros_);
  // Compute lower part of fraction (last 12 bits).
  __ sll(mantissa, source_, HeapNumber::kMantissaBitsInTopWord);
  // And the top (top 20 bits).
  __ srl(source_, source_, 32 - HeapNumber::kMantissaBitsInTopWord);
  __ or_(exponent, exponent, source_);

  __ Ret();
}


// See comment for class, this does NOT work for int32's that are in Smi range.
void WriteInt32ToHeapNumberStub::Generate(MacroAssembler* masm) {
  Label max_negative_int;
  // the_int_ has the answer which is a signed int32 but not a Smi.
  // We test for the special value that has a different exponent.
  ASSERT(HeapNumber::kSignMask == 0x80000000u);
  // Test sign, and save for later conditionals.
  __ And(sign_, the_int_, Operand(0x80000000u));
  __ Branch(eq, &max_negative_int, the_int_, Operand(0x80000000u));

  // Set up the correct exponent in scratch_.  All non-Smi int32s have the same.
  // A non-Smi integer is 1.xxx * 2^30 so the exponent is 30 (biased).
  uint32_t non_smi_exponent =
      (HeapNumber::kExponentBias + 30) << HeapNumber::kExponentShift;
  __ li(scratch_, Operand(non_smi_exponent));
  // Set the sign bit in scratch_ if the value was negative.
  __ or_(scratch_, scratch_, sign_);
  // Subtract from 0 if the value was negative.
  __ subu(at, zero_reg, the_int_);
  __ movn(the_int_, at, sign_);
  // We should be masking the implict first digit of the mantissa away here,
  // but it just ends up combining harmlessly with the last digit of the
  // exponent that happens to be 1.  The sign bit is 0 so we shift 10 to get
  // the most significant 1 to hit the last bit of the 12 bit sign and exponent.
  ASSERT(((1 << HeapNumber::kExponentShift) & non_smi_exponent) != 0);
  const int shift_distance = HeapNumber::kNonMantissaBitsInTopWord - 2;
  __ srl(at, the_int_, shift_distance);
  __ or_(scratch_, scratch_, at);
  __ sw(scratch_, FieldMemOperand(the_heap_number_,
                                   HeapNumber::kExponentOffset));
  __ sll(scratch_, the_int_, 32 - shift_distance);
  __ sw(scratch_, FieldMemOperand(the_heap_number_,
                                   HeapNumber::kMantissaOffset));
  __ Ret();

  __ bind(&max_negative_int);
  // The max negative int32 is stored as a positive number in the mantissa of
  // a double because it uses a sign bit instead of using two's complement.
  // The actual mantissa bits stored are all 0 because the implicit most
  // significant 1 bit is not stored.
  non_smi_exponent += 1 << HeapNumber::kExponentShift;
  __ li(scratch_, Operand(HeapNumber::kSignMask | non_smi_exponent));
  __ sw(scratch_,
        FieldMemOperand(the_heap_number_, HeapNumber::kExponentOffset));
  __ li(scratch_, Operand(0));
  __ sw(scratch_,
        FieldMemOperand(the_heap_number_, HeapNumber::kMantissaOffset));
  __ Ret();
}


// Handle the case where the lhs and rhs are the same object.
// Equality is almost reflexive (everything but NaN), so this is a test
// for "identity and not NaN".
static void EmitIdenticalObjectComparison(MacroAssembler* masm,
                                          Label* slow,
                                          Condition cc,
                                          bool never_nan_nan) {
  Label not_identical;
  Label heap_number, return_equal;

  __ Branch(ne, &not_identical, a0, Operand(a1));

  // The two objects are identical. If we know that one of them isn't NaN then
  // we now know they test equal.
  if (cc != eq || !never_nan_nan) {
    // Test for NaN. Sadly, we can't just compare to Factory::nan_value(),
    // so we do the second best thing - test it ourselves.
    // They are both equal and they are not both Smis so both of them are not
    // Smis. If it's not a heap number, then return equal.
    if (cc == less || cc == greater) {
      __ GetObjectType(a0, t4, t4);
      __ Branch(greater, slow, t4, Operand(FIRST_JS_OBJECT_TYPE));
    } else {
      __ GetObjectType(a0, t4, t4);
      __ Branch(eq, &heap_number, t4, Operand(HEAP_NUMBER_TYPE));
      // Comparing JS objects with <=, >= is complicated.
      if (cc != eq) {
      __ Branch(greater, slow, t4, Operand(FIRST_JS_OBJECT_TYPE));
        // Normally here we fall through to return_equal, but undefined is
        // special: (undefined == undefined) == true, but
        // (undefined <= undefined) == false!  See ECMAScript 11.8.5.
        if (cc == less_equal || cc == greater_equal) {
          __ Branch(ne, &return_equal, t4, Operand(ODDBALL_TYPE));
          __ LoadRoot(t2, Heap::kUndefinedValueRootIndex);
          __ Branch(ne, &return_equal, a0, Operand(t2));
          if (cc == le) {
            // undefined <= undefined should fail.
            __ li(v0, Operand(GREATER));
          } else  {
            // undefined >= undefined should fail.
            __ li(v0, Operand(LESS));
          }
          __ Ret();
        }
      }
    }
  }

  __ bind(&return_equal);
  if (cc == less) {
    __ li(v0, Operand(GREATER));  // Things aren't less than themselves.
  } else if (cc == greater) {
    __ li(v0, Operand(LESS));     // Things aren't greater than themselves.
  } else {
    __ li(v0, Operand(0));        // Things are <=, >=, ==, === themselves.
  }
  __ Ret();

  // For less and greater we don't have to check for NaN since the result of
  // x < x is false regardless. For the others here is some code to check
  // for NaN.
  if (cc != less && cc != greater) {
    __ bind(&heap_number);
    UNIMPLEMENTED_MIPS();
    __ break_(__LINE__);
  }
  // No fall through here.

  __ bind(&not_identical);
}


void FastNewClosureStub::Generate(MacroAssembler* masm) {
  // Create a new closure from the given function info in new
  // space. Set the context to the current context in cp.
  Label gc;

  // Pop the function info from the stack.
  __ Pop(a3);

  // Attempt to allocate new JSFunction in new space.
  __ AllocateInNewSpace(JSFunction::kSize / kPointerSize,
                        v0,
                        a1,
                        a2,
                        &gc,
                        TAG_OBJECT);

  // Compute the function map in the current global context and set that
  // as the map of the allocated object.
  __ lw(a2, MemOperand(cp, Context::SlotOffset(Context::GLOBAL_INDEX)));
  __ lw(a2, FieldMemOperand(a2, GlobalObject::kGlobalContextOffset));
  __ lw(a2, MemOperand(a2, Context::SlotOffset(Context::FUNCTION_MAP_INDEX)));
  __ sw(a2, FieldMemOperand(v0, HeapObject::kMapOffset));

  // Initialize the rest of the function. We don't have to update the
  // write barrier because the allocated object is in new space.
  __ LoadRoot(a1, Heap::kEmptyFixedArrayRootIndex);
  __ LoadRoot(a2, Heap::kTheHoleValueRootIndex);
  __ sw(a1, FieldMemOperand(v0, JSObject::kPropertiesOffset));
  __ sw(a1, FieldMemOperand(v0, JSObject::kElementsOffset));
  __ sw(a2, FieldMemOperand(v0, JSFunction::kPrototypeOrInitialMapOffset));
  __ sw(a3, FieldMemOperand(v0, JSFunction::kSharedFunctionInfoOffset));
  __ sw(cp, FieldMemOperand(v0, JSFunction::kContextOffset));
  __ sw(a1, FieldMemOperand(v0, JSFunction::kLiteralsOffset));

  // Return result. The argument function info has been popped already.
  __ Ret();

  // Create a new closure through the slower runtime call.
  __ bind(&gc);
  __ addiu(sp, sp, 2 * kPointerSize);
  __ sw(cp, MemOperand(sp, 1 * kPointerSize));
  __ sw(a3, MemOperand(sp, 0 * kPointerSize));
  __ TailCallRuntime(Runtime::kNewClosure, 2, 1);
}


void FastCloneShallowArrayStub::Generate(MacroAssembler* masm) {
  // Stack layout on entry:
  // [sp]: constant elements.
  // [sp + kPointerSize]: literal index.
  // [sp + (2 * kPointerSize)]: literals array.

  // All sizes here are multiples of kPointerSize.
  int elements_size = (length_ > 0) ? FixedArray::SizeFor(length_) : 0;
  int size = JSArray::kSize + elements_size;

  // Load boilerplate object into r3 and check if we need to create a
  // boilerplate.
  Label slow_case;
  __ lw(a3, MemOperand(sp, 2 * kPointerSize));
  __ lw(a0, MemOperand(sp, 1 * kPointerSize));
  __ Add(a3, a3, Operand(FixedArray::kHeaderSize - kHeapObjectTag));
  __ sll(t0, a0, kPointerSizeLog2 - kSmiTagSize);
  __ Add(t0, a3, t0);
  __ lw(a3, MemOperand(t0));
  __ LoadRoot(t1, Heap::kUndefinedValueRootIndex);
  __ Branch(eq, &slow_case, a3, Operand(t1));

  // Allocate both the JS array and the elements array in one big
  // allocation. This avoids multiple limit checks.
  __ AllocateInNewSpace(size / kPointerSize,
                        a0,
                        a1,
                        a2,
                        &slow_case,
                        TAG_OBJECT);

  // Copy the JS array part.
  for (int i = 0; i < JSArray::kSize; i += kPointerSize) {
    if ((i != JSArray::kElementsOffset) || (length_ == 0)) {
      __ lw(a1, FieldMemOperand(a3, i));
      __ sw(a1, FieldMemOperand(a0, i));
    }
  }

  if (length_ > 0) {
    // Get hold of the elements array of the boilerplate and setup the
    // elements pointer in the resulting object.
    __ lw(a3, FieldMemOperand(a3, JSArray::kElementsOffset));
    __ Add(a2, a0, Operand(JSArray::kSize));
    __ sw(a2, FieldMemOperand(a0, JSArray::kElementsOffset));

    // Copy the elements array.
    for (int i = 0; i < elements_size; i += kPointerSize) {
      __ lw(a1, FieldMemOperand(a3, i));
      __ sw(a1, FieldMemOperand(a2, i));
    }
  }

  // Return and remove the on-stack parameters.
  __ Add(sp, sp, Operand(3 * kPointerSize));
  __ Ret();

  __ bind(&slow_case);
  __ TailCallRuntime(Runtime::kCreateArrayLiteralShallow, 3, 1);
}


static void EmitSmiNonsmiComparison(MacroAssembler* masm,
                                    Label* both_loaded_as_doubles,
                                    Label* slow,
                                    bool strict) {
  Label lhs_is_smi;
  __ And(t0, a0, Operand(kSmiTagMask));
  __ Branch(eq, &lhs_is_smi, t0, Operand(zero_reg));

  // Rhs is a Smi.
  // Check whether the non-smi is a heap number.
  __ GetObjectType(a0, t4, t4);
  if (strict) {
    // If lhs was not a number and rhs was a Smi then strict equality cannot
    // succeed. Return non-equal (a0 is already not zero)
    __ mov(v0, a0);
    __ Ret(ne, t4, Operand(HEAP_NUMBER_TYPE));
  } else {
    // Smi compared non-strictly with a non-Smi non-heap-number. Call
    // the runtime.
    __ Branch(ne, slow, t4, Operand(HEAP_NUMBER_TYPE));
  }

  // Rhs is a smi, lhs is a number.
  // Convert a1 to double.
  __ mtc1(a1, f12);
  __ cvt_d_s(f12, f12);
  __ ldc1(f14, FieldMemOperand(a0, HeapNumber::kValueOffset));

  // We now have both loaded as doubles.
  __ jmp(both_loaded_as_doubles);

  __ bind(&lhs_is_smi);
  // Lhs is a Smi.  Check whether the non-smi is a heap number.
  __ GetObjectType(a1, t4, t4);
  if (strict) {
    // If lhs was not a number and rhs was a Smi then strict equality cannot
    // succeed. Return non-equal.
    __ li(v0, Operand(1));
    __ Ret(ne, t4, Operand(HEAP_NUMBER_TYPE));
  } else {
    // Smi compared non-strictly with a non-Smi non-heap-number. Call
    // the runtime.
    __ Branch(ne, slow, t4, Operand(HEAP_NUMBER_TYPE));
  }

  // Lhs is a smi, rhs is a number.
  // a0 is Smi and a1 is heap number.
  // Convert a1 to double.
  __ mtc1(a0, f14);
  __ cvt_d_s(f14, f14);
  __ ldc1(f12, FieldMemOperand(a1, HeapNumber::kValueOffset));
  // Fall through to both_loaded_as_doubles.
}


void EmitNanCheck(MacroAssembler* masm, Condition cc) {
  // We use the coprocessor c.cond instructions.
  Label one_is_nan, neither_is_nan;

  // Test the Unordered condition on both doubles. This is false if any of them
  // is Nan.
  __ c(UN, D, f12, f14);
  __ bc1f(&neither_is_nan);
  __ nop();
  __ bc1t(&one_is_nan);
  __ nop();

  // At least one is nan
  __ bind(&one_is_nan);
  // NaN comparisons always fail.
  // Load whatever we need in r0 to make the comparison fail.
  if (cc == less || cc == less_equal) {
    __ li(v0, Operand(GREATER));
  } else {
    __ li(v0, Operand(LESS));
  }
  __ Ret();

  __ bind(&neither_is_nan);
}


static void EmitTwoNonNanDoubleComparison(MacroAssembler* masm, Condition cc) {
  // f12 and f14 have the two doubles.  Neither is a NaN.
  // Call a native function to do a comparison between two non-NaNs.
  // Call C routine that may not cause GC or other trouble.
  // We use a call_was and return manually because we need arguments slots to
  // be freed.
  
  __ li(t9, Operand(ExternalReference::compare_doubles()));
  __ SetupAlignedCall(t0, 0);
  __ Call(t9);  // Call the code
  __ Addu(sp, sp, Operand(-StandardFrameConstants::kCArgsSlotsSize));
  __ Addu(sp, sp, Operand(StandardFrameConstants::kCArgsSlotsSize));
  __ ReturnFromAlignedCall();
  
  __ Ret();
}


static void EmitStrictTwoHeapObjectCompare(MacroAssembler* masm) {
    // If either operand is a JSObject or an oddball value, then they are
    // not equal since their pointers are different.
    // There is no test for undetectability in strict equality.
    ASSERT(LAST_TYPE == JS_FUNCTION_TYPE);
    Label first_non_object;
    // Get the type of the first operand into a2 and compare it with
    // FIRST_JS_OBJECT_TYPE.
    __ GetObjectType(a0, a2, a2);
    __ Branch(less, &first_non_object, a2, Operand(FIRST_JS_OBJECT_TYPE));

    // Return non-zero.
    Label return_not_equal;
    __ bind(&return_not_equal);
    __ li(v0, Operand(1));
    __ Ret();

    __ bind(&first_non_object);
    // Check for oddballs: true, false, null, undefined.
    __ Branch(eq, &return_not_equal, a2, Operand(ODDBALL_TYPE));

    __ GetObjectType(a1, a3, a3);
    __ Branch(greater, &return_not_equal, a3, Operand(FIRST_JS_OBJECT_TYPE));

    // Check for oddballs: true, false, null, undefined.
    __ Branch(eq, &return_not_equal, a3, Operand(ODDBALL_TYPE));

    // Now that we have the types we might as well check for symbol-symbol.
    // Ensure that no non-strings have the symbol bit set.
    ASSERT(kNotStringTag + kIsSymbolMask > LAST_TYPE);
    ASSERT(kSymbolTag != 0);
    __ And(t2, a2, Operand(a3));
    __ And(t0, t2, Operand(kIsSymbolMask));
    __ Branch(ne, &return_not_equal, t0, Operand(zero_reg));
}


static void EmitCheckForTwoHeapNumbers(MacroAssembler* masm,
                                       Label* both_loaded_as_doubles,
                                       Label* not_heap_numbers,
                                       Label* slow) {
  __ GetObjectType(a0, a2, a2);
  __ Branch(ne, not_heap_numbers, a2, Operand(HEAP_NUMBER_TYPE));
  __ GetObjectType(a1, a3, a3);
  // First was a heap number, second wasn't. Go slow case.
  __ Branch(ne, not_heap_numbers, a3, Operand(HEAP_NUMBER_TYPE));

  // Both are heap numbers. Load them up then jump to the code we have
  // for that.
  __ ldc1(f12, FieldMemOperand(a0, HeapNumber::kValueOffset));
  __ ldc1(f14, FieldMemOperand(a1, HeapNumber::kValueOffset));
  __ jmp(both_loaded_as_doubles);
}


static void EmitCheckForSymbols(MacroAssembler* masm, Label* slow) {
  // a2 is object type of a0.
  // Ensure that no non-strings have the symbol bit set.
  ASSERT(kNotStringTag + kIsSymbolMask > LAST_TYPE);
  ASSERT(kSymbolTag != 0);
  __ And(t2, a2, Operand(kIsSymbolMask));
  __ Branch(eq, slow, t2, Operand(zero_reg));
  __ lw(a3, FieldMemOperand(a1, HeapObject::kMapOffset));
  __ lbu(a3, FieldMemOperand(a3, Map::kInstanceTypeOffset));
  __ And(t3, a3, Operand(kIsSymbolMask));
  __ Branch(eq, slow, t3, Operand(zero_reg));

  // Both are symbols. We already checked they weren't the same pointer
  // so they are not equal.
  __ li(v0, Operand(1));   // Non-zero indicates not equal.
  __ Ret();
}


void NumberToStringStub::GenerateLookupNumberStringCache(MacroAssembler* masm,
                                                         Register object,
                                                         Register result,
                                                         Register scratch1,
                                                         Register scratch2,
                                                         bool object_is_smi,
                                                         Label* not_found) {
  // Currently only lookup for smis. Check for smi if object is not known to be
  // a smi.
  if (!object_is_smi) {
    ASSERT(kSmiTag == 0);
    __ BranchOnNotSmi(object, not_found, scratch1);
  }

  // Use of registers. Register result is used as a temporary.
  Register number_string_cache = result;
  Register mask = scratch1;
  Register scratch = scratch2;

  // Load the number string cache.
  __ LoadRoot(number_string_cache, Heap::kNumberStringCacheRootIndex);

  // Make the hash mask from the length of the number string cache. It
  // contains two elements (number and string) for each cache entry.
  __ lw(mask, FieldMemOperand(number_string_cache, FixedArray::kLengthOffset));
  // Divide length by two (length is not a smi).
  __ sra(mask, mask, 1);
  __ Addu(mask, mask, -1);  // Make mask.

  // Calculate the entry in the number string cache. The hash value in the
  // number string cache for smis is just the smi value.
  __ sra(scratch, object, 1);
  __ And(scratch, mask, scratch);

  // Calculate address of entry in string cache: each entry consists
  // of two pointer sized fields.
  __ sll(scratch, scratch, kPointerSizeLog2 + 1);
  __ Addu(scratch, number_string_cache, scratch);

  // Check if the entry is the smi we are looking for.
  Register object1 = scratch1;
  __ lw(object1, FieldMemOperand(scratch, FixedArray::kHeaderSize));
  __ Branch(ne, not_found, object, Operand(object1));

  // Get the result from the cache.
  __ lw(result,
         FieldMemOperand(scratch, FixedArray::kHeaderSize + kPointerSize));

  __ IncrementCounter(&Counters::number_to_string_native,
                      1,
                      scratch1,
                      scratch2);
}


void NumberToStringStub::Generate(MacroAssembler* masm) {
  Label runtime;

  __ lw(a1, MemOperand(sp, 0));

  // Generate code to lookup number in the number string cache.
//  GenerateLookupNumberStringCache(masm, a1, a0, a2, a3, false, &runtime);
//  __ Add(sp, sp, Operand(1 * kPointerSize));
//  __ Ret();

  __ bind(&runtime);
  // Handle number to string in the runtime system if not found in the cache.
  __ TailCallRuntime(Runtime::kNumberToString, 1, 1);
}


// On entry a0 and a1 are the things to be compared. On exit v0 is 0,
// positive or negative to indicate the result of the comparison.
void CompareStub::Generate(MacroAssembler* masm) {
  Label slow;  // Call builtin.
  Label not_smis, both_loaded_as_doubles;

  // NOTICE! This code is only reached after a smi-fast-case check, so
  // it is certain that at least one operand isn't a smi.

  // Handle the case where the objects are identical.  Either returns the answer
  // or goes to slow.  Only falls through if the objects were not identical.
  EmitIdenticalObjectComparison(masm, &slow, cc_, never_nan_nan_);

  // If either is a Smi (we know that not both are), then they can only
  // be strictly equal if the other is a HeapNumber.
  ASSERT_EQ(0, kSmiTag);
  ASSERT_EQ(0, Smi::FromInt(0));
  __ And(t2, a0, Operand(a1));
  __ BranchOnNotSmi(t2, &not_smis, t0);
  // One operand is a smi. EmitSmiNonsmiComparison generates code that can:
  // 1) Return the answer.
  // 2) Go to slow.
  // 3) Fall through to both_loaded_as_doubles.
  // 4) Jump to rhs_not_nan.
  // In cases 3 and 4 we have found out we were dealing with a number-number
  // comparison and the numbers have been loaded into f12 and f14 as doubles.
  EmitSmiNonsmiComparison(masm, &both_loaded_as_doubles, &slow, strict_);

  __ bind(&both_loaded_as_doubles);
  // f12, f14 are the double representations of the left hand side
  // and the right hand side.

  // Checks for NaN in the doubles we have loaded.  Can return the answer or
  // fall through if neither is a NaN.  Also binds rhs_not_nan.
  EmitNanCheck(masm, cc_);

  // Compares two doubles that are not NaNs. Returns the answer.
  // Never falls through.
  EmitTwoNonNanDoubleComparison(masm, cc_);

  __ bind(&not_smis);
  // At this point we know we are dealing with two different objects,
  // and neither of them is a Smi. The objects are in a0 and a1.
  if (strict_) {
    // This returns non-equal for some object types, or falls through if it
    // was not lucky.
    EmitStrictTwoHeapObjectCompare(masm);
  }

  Label check_for_symbols;
  Label flat_string_check;
  // Check for heap-number-heap-number comparison. Can jump to slow case,
  // or load both doubles and jump to the code that handles
  // that case. If the inputs are not doubles then jumps to check_for_symbols.
  // In this case a2 will contain the type of a0.
  EmitCheckForTwoHeapNumbers(masm,
                             &both_loaded_as_doubles,
                             &check_for_symbols,
                             &flat_string_check);

  __ bind(&check_for_symbols);
  if (cc_ == eq) {
    // Either jumps to slow or returns the answer. Assumes that a2 is the type
    // of a0 on entry.
    EmitCheckForSymbols(masm, &flat_string_check);
//    EmitCheckForSymbols(masm, &slow);
  }

  // Check for both being sequential ASCII strings, and inline if that is the
  // case.
  __ bind(&flat_string_check);
  UNIMPLEMENTED_MIPS();
  __ break_(__LINE__);

  __ bind(&slow);
  UNIMPLEMENTED_MIPS();
  // TOCHECK: Check push order. In Comparison() we pop in the reverse order.
  __ MultiPush(a1.bit() | a0.bit());
  // Figure out which native to call and setup the arguments.
  Builtins::JavaScript native;
  if (cc_ == eq) {
    native = strict_ ? Builtins::STRICT_EQUALS : Builtins::EQUALS;
  } else {
    native = Builtins::COMPARE;
    int ncr;  // NaN compare result
    if (cc_ == lt || cc_ == le) {
      ncr = GREATER;
    } else {
      ASSERT(cc_ == gt || cc_ == ge);  // remaining cases
      ncr = LESS;
    }
    __ li(a0, Operand(Smi::FromInt(ncr)));
    __ Push(a0);
  }

  // Call the native; it returns -1 (less), 0 (equal), or 1 (greater)
  // tagged as a small integer.
  __ InvokeBuiltin(native, JUMP_JS);
}


Handle<Code> GetBinaryOpStub(int key, BinaryOpIC::TypeInfo type_info) {
  UNIMPLEMENTED_MIPS();
  return Handle<Code>::null();
}


void StackCheckStub::Generate(MacroAssembler* masm) {
  // Do tail-call to runtime routine.  Runtime routines expect at least one
  // argument, so give it a Smi.
  __ Push(zero_reg);
  __ TailCallRuntime(Runtime::kStackGuard, 1, 1);
  __ StubReturn(1);
}


void GenericUnaryOpStub::Generate(MacroAssembler* masm) {
  Label slow, done;

  if (op_ == Token::SUB) {
    UNIMPLEMENTED_MIPS();
    __ break_(__LINE__);

  } else if (op_ == Token::BIT_NOT) {
    UNIMPLEMENTED_MIPS();
    __ break_(__LINE__);

  } else {
    UNIMPLEMENTED();
    __ break_(__LINE__);
  }

  __ bind(&done);
  __ StubReturn(1);

  // Handle the slow case by jumping to the JavaScript builtin.
  __ bind(&slow);
  __ Push(a0);

  __ break_(__LINE__);  // MIPS does not support builtins yet.

  switch (op_) {
    case Token::SUB:
      __ InvokeBuiltin(Builtins::UNARY_MINUS, JUMP_JS);
      break;
    case Token::BIT_NOT:
      __ InvokeBuiltin(Builtins::BIT_NOT, JUMP_JS);
      break;
    default:
      UNREACHABLE();
  }
}


void CEntryStub::GenerateThrowTOS(MacroAssembler* masm) {
  // v0 holds the exception.

  // Adjust this code if not the case.
  ASSERT(StackHandlerConstants::kSize == 4 * kPointerSize);

  // Drop the sp to the top of the handler.
  __ li(a3, Operand(ExternalReference(Top::k_handler_address)));
  __ lw(sp, MemOperand(a3));
  
  // Restore the next handler and frame pointer, discard handler state.
  ASSERT(StackHandlerConstants::kNextOffset == 0);
  __ Pop(a2);
  __ sw(a2, MemOperand(a3));
  ASSERT(StackHandlerConstants::kFPOffset == 2 * kPointerSize);
  __ MultiPop(a3.bit() | fp.bit());

  // Before returning we restore the context from the frame pointer if
  // not NULL. The frame pointer is NULL in the exception handler of a
  // JS entry frame.
  // Set cp to NULL if fp is NULL.
  Label done;
  __ Branch(false, eq, &done, fp, Operand(zero_reg));
  __ mov(cp, zero_reg);   // Use the branch delay slot.
  __ lw(cp, MemOperand(fp, StandardFrameConstants::kContextOffset));
  __ bind(&done);

#ifdef DEBUG
  // TODO(MIPS): Implement debug code.
#endif

  ASSERT(StackHandlerConstants::kPCOffset == 3 * kPointerSize);
  __ Pop(t9);
  __ Jump(t9);
}


void CEntryStub::GenerateThrowUncatchable(MacroAssembler* masm,
                                          UncatchableExceptionType type) {
  UNIMPLEMENTED_MIPS();
  __ break_(__LINE__);
}

void CEntryStub::GenerateCore(MacroAssembler* masm,
                              Label* throw_normal_exception,
                              Label* throw_termination_exception,
                              Label* throw_out_of_memory_exception,
                              bool do_gc,
                              bool always_allocate) {
  // v0: result parameter for PerformGC, if any
  // s0: number of arguments including receiver (C callee-saved)
  // s1: pointer to the first argument          (C callee-saved)
  // s2: pointer to builtin function            (C callee-saved)

  if (do_gc) {
    UNIMPLEMENTED_MIPS();
    __ break_(__LINE__);
  }

  ExternalReference scope_depth =
      ExternalReference::heap_always_allocate_scope_depth();
  if (always_allocate) {
    UNIMPLEMENTED_MIPS();
    __ break_(__LINE__);
  }

  // Call C built-in.
  // a0 = argc, a1 = argv
  __ mov(a0, s0);
  __ mov(a1, s1);

  // We are calling compiled C/C++ code. a0 and a1 hold our two arguments. We
  // also need the argument slots.
  __ jalr(s2);
  __ addiu(sp, sp, -StandardFrameConstants::kCArgsSlotsSize);
  __ addiu(sp, sp, StandardFrameConstants::kCArgsSlotsSize);

  if (always_allocate) {
    UNIMPLEMENTED_MIPS();
    __ break_(__LINE__);
  }

  // Check for failure result.
  Label failure_returned;
  ASSERT(((kFailureTag + 1) & kFailureTagMask) == 0);
  __ addiu(a2, v0, 1);
  __ andi(t0, a2, kFailureTagMask);
  __ Branch(eq, &failure_returned, t0, Operand(zero_reg));

  // Exit C frame and return.
  // v0:v1: result
  // sp: stack pointer
  // fp: frame pointer
  __ LeaveExitFrame(mode_);

  // Check if we should retry or throw exception.
  Label retry;
  __ bind(&failure_returned);
  ASSERT(Failure::RETRY_AFTER_GC == 0);
  __ andi(t0, v0, ((1 << kFailureTypeTagSize) - 1) << kFailureTagSize);
  __ Branch(eq, &retry, t0, Operand(zero_reg));

  // Special handling of out of memory exceptions.
  Failure* out_of_memory = Failure::OutOfMemoryException();
  __ Branch(eq, throw_out_of_memory_exception,
            v0, Operand(reinterpret_cast<int32_t>(out_of_memory)));

  // Retrieve the pending exception and clear the variable.
  __ LoadExternalReference(t0, ExternalReference::the_hole_value_location());
  __ lw(a3, MemOperand(t0));
  __ LoadExternalReference(t0,
      ExternalReference(Top::k_pending_exception_address));
  __ lw(v0, MemOperand(t0));
  __ sw(a3, MemOperand(t0));

  // Special handling of termination exceptions which are uncatchable
  // by javascript code.
  __ Branch(eq, throw_termination_exception,
            v0, Operand(Factory::termination_exception()));

  // Handle normal exception.
  __ jmp(throw_normal_exception);

  __ bind(&retry);  // Pass last failure (v0) as parameter (a0) when retrying.
  __ mov(a0, v0);
}

void CEntryStub::Generate(MacroAssembler* masm) {
  // Called from JavaScript; parameters are on stack as if calling JS function
  // a0: number of arguments including receiver
  // a1: pointer to builtin function
  // fp: frame pointer    (restored after C call)
  // sp: stack pointer    (restored as callee's sp after C call)
  // cp: current context  (C callee-saved)

  // NOTE: Invocations of builtins may return failure objects
  // instead of a proper result. The builtin entry handles
  // this by performing a garbage collection and retrying the
  // builtin once.

  // Enter the exit frame that transitions from JavaScript to C++.
  __ EnterExitFrame(mode_, s0, s1, s2);

  // s0: number of arguments (C callee-saved)
  // s1: pointer to first argument (C callee-saved)
  // s2: pointer to builtin function (C callee-saved)

  Label throw_normal_exception;
  Label throw_termination_exception;
  Label throw_out_of_memory_exception;

  // Call into the runtime system.
  GenerateCore(masm,
               &throw_normal_exception,
               &throw_termination_exception,
               &throw_out_of_memory_exception,
               false,
               false);

  // Do space-specific GC and retry runtime call.
  GenerateCore(masm,
               &throw_normal_exception,
               &throw_termination_exception,
               &throw_out_of_memory_exception,
               true,
               false);

  // Do full GC and retry runtime call one final time.
  Failure* failure = Failure::InternalError();
  __ li(v0, Operand(reinterpret_cast<int32_t>(failure)));
  GenerateCore(masm,
               &throw_normal_exception,
               &throw_termination_exception,
               &throw_out_of_memory_exception,
               true,
               true);

  __ bind(&throw_out_of_memory_exception);
  GenerateThrowUncatchable(masm, OUT_OF_MEMORY);

  __ bind(&throw_termination_exception);
  GenerateThrowUncatchable(masm, TERMINATION);

  __ bind(&throw_normal_exception);
  GenerateThrowTOS(masm);
}

void JSEntryStub::GenerateBody(MacroAssembler* masm, bool is_construct) {
  Label invoke, exit;

  // Registers:
  // a0: entry address
  // a1: function
  // a2: reveiver
  // a3: argc
  //
  // Stack:
  // 4 args slots
  // args

  // Save callee saved registers on the stack.
  __ MultiPush((kCalleeSaved | ra.bit()) & ~sp.bit());

  // Load argv in s0 register.
  __ lw(s0, MemOperand(sp, kNumCalleeSaved * kPointerSize +
                           StandardFrameConstants::kCArgsSlotsSize));

  // We build an EntryFrame.
  __ li(t3, Operand(-1));  // Push a bad frame pointer to fail if it is used.
  int marker = is_construct ? StackFrame::ENTRY_CONSTRUCT : StackFrame::ENTRY;
  __ li(t2, Operand(Smi::FromInt(marker)));
  __ li(t1, Operand(Smi::FromInt(marker)));
  __ LoadExternalReference(t0, ExternalReference(Top::k_c_entry_fp_address));
  __ lw(t0, MemOperand(t0));
  __ MultiPush(t0.bit() | t1.bit() | t2.bit() | t3.bit());

  // Setup frame pointer for the frame to be pushed.
  __ addiu(fp, sp, -EntryFrameConstants::kCallerFPOffset);

  // Registers:
  // a0: entry_address
  // a1: function
  // a2: reveiver_pointer
  // a3: argc
  // s0: argv
  //
  // Stack:
  // caller fp          |
  // function slot      | entry frame
  // context slot       |
  // bad fp (0xff...f)  |
  // callee saved registers + ra
  // 4 args slots
  // args

  // Call a faked try-block that does the invoke.
  __ bal(&invoke);
  __ nop();   // Branch delay slot nop.

  // Caught exception: Store result (exception) in the pending
  // exception field in the JSEnv and return a failure sentinel.
  // Coming in here the fp will be invalid because the PushTryHandler below
  // sets it to 0 to signal the existence of the JSEntry frame.
  __ LoadExternalReference(t0,
      ExternalReference(Top::k_pending_exception_address));
  __ sw(v0, MemOperand(t0));  // We come back from 'invoke'. result is in v0.
  __ li(v0, Operand(reinterpret_cast<int32_t>(Failure::Exception())));
  __ b(&exit);
  __ nop();   // Branch delay slot nop.

  // Invoke: Link this frame into the handler chain.
  __ bind(&invoke);
  __ PushTryHandler(IN_JS_ENTRY, JS_ENTRY_HANDLER);
  // If an exception not caught by another handler occurs, this handler
  // returns control to the code after the bal(&invoke) above, which
  // restores all kCalleeSaved registers (including cp and fp) to their
  // saved values before returning a failure to C.

  // Clear any pending exceptions.
  __ LoadExternalReference(t0, ExternalReference::the_hole_value_location());
  __ lw(t1, MemOperand(t0));
  __ LoadExternalReference(t0,
      ExternalReference(Top::k_pending_exception_address));
  __ sw(t1, MemOperand(t0));

  // Invoke the function by calling through JS entry trampoline builtin.
  // Notice that we cannot store a reference to the trampoline code directly in
  // this stub, because runtime stubs are not traversed when doing GC.

  // Registers:
  // a0: entry_address
  // a1: function
  // a2: reveiver_pointer
  // a3: argc
  // s0: argv
  //
  // Stack:
  // handler frame
  // entry frame
  // callee saved registers + ra
  // 4 args slots
  // args

  if (is_construct) {
    ExternalReference construct_entry(Builtins::JSConstructEntryTrampoline);
    __ LoadExternalReference(t0, construct_entry);
  } else {
    ExternalReference entry(Builtins::JSEntryTrampoline);
    __ LoadExternalReference(t0, entry);
  }
  __ lw(t9, MemOperand(t0));  // Deref address.

  // Call JSEntryTrampoline.
  __ addiu(t9, t9, Code::kHeaderSize - kHeapObjectTag);
  __ CallBuiltin(t9);

  // Unlink this frame from the handler chain. When reading the
  // address of the next handler, there is no need to use the address
  // displacement since the current stack pointer (sp) points directly
  // to the stack handler.
  __ lw(t1, MemOperand(sp, StackHandlerConstants::kNextOffset));
  __ LoadExternalReference(t0, ExternalReference(Top::k_handler_address));
  __ sw(t1, MemOperand(t0));

  // This restores sp to its position before PushTryHandler.
  __ addiu(sp, sp, StackHandlerConstants::kSize);

  __ bind(&exit);  // v0 holds result.
  // Restore the top frame descriptors from the stack.
  __ Pop(t1);
  __ LoadExternalReference(t0, ExternalReference(Top::k_c_entry_fp_address));
  __ sw(t1, MemOperand(t0));

  // Reset the stack to the callee saved registers.
  __ addiu(sp, sp, -EntryFrameConstants::kCallerFPOffset);

  // Restore callee saved registers from the stack.
  __ MultiPop((kCalleeSaved | ra.bit()) & ~sp.bit());
  // Return.
  __ Jump(ra);
}


// This stub performs an instanceof, calling the builtin function if
// necessary. Uses a1 for the object, a0 for the function that it may
// be an instance of (these are fetched from the stack).
void InstanceofStub::Generate(MacroAssembler* masm) {
  // Get the object - slow case for smis (we may need to throw an exception
  // depending on the rhs).
  Label slow, loop, is_instance, is_not_instance;
  __ lw(a0, MemOperand(sp, 1 * kPointerSize));
  __ BranchOnSmi(a0, &slow);

  // Check that the left hand is a JS object and put map in a3.
  __ GetObjectType(a0, a3, a2);
  __ Branch(less, &slow, a2, Operand(FIRST_JS_OBJECT_TYPE));
  __ Branch(greater, &slow, a2, Operand(LAST_JS_OBJECT_TYPE));

  // Get the prototype of the function (t0 is result, a2 is scratch).
  __ lw(a1, MemOperand(sp, 0 * kPointerSize));
  __ TryGetFunctionPrototype(a1, t0, a2, &slow);

  // Check that the function prototype is a JS object.
  __ BranchOnSmi(t0, &slow);
  __ GetObjectType(t0, t1, t1);
  __ Branch(less, &slow, t1, Operand(FIRST_JS_OBJECT_TYPE));
  __ Branch(greater, &slow, t1, Operand(LAST_JS_OBJECT_TYPE));

  // Register mapping: a3 is object map and t0 is function prototype.
  // Get prototype of object into a2.
  __ lw(a2, FieldMemOperand(a3, Map::kPrototypeOffset));

  __ LoadRoot(t1, Heap::kNullValueRootIndex);
  // Loop through the prototype chain looking for the function prototype.
  __ bind(&loop);
  __ Branch(eq, &is_instance, a2, Operand(t0));
  __ Branch(eq, &is_not_instance, a2, Operand(t1));
  __ lw(a2, FieldMemOperand(a2, HeapObject::kMapOffset));
  __ lw(a2, FieldMemOperand(a2, Map::kPrototypeOffset));
  __ jmp(&loop);

  __ bind(&is_instance);
  __ li(v0, Operand(Smi::FromInt(0)));
  __ Pop(2);
  __ Ret();

  __ bind(&is_not_instance);
  __ li(v0, Operand(Smi::FromInt(1)));
  __ Pop(2);
  __ Ret();

  // Slow-case. Tail call builtin.
  __ bind(&slow);
  // TODO(MIPS): instanceof slow case. Need JS builtins.
  __ break_(0x3137);
}


void ArgumentsAccessStub::GenerateReadLength(MacroAssembler* masm) {
  // Check if the calling frame is an arguments adaptor frame.
  Label adaptor;
  __ lw(a2, MemOperand(fp, StandardFrameConstants::kCallerFPOffset));
  __ lw(a3, MemOperand(a2, StandardFrameConstants::kContextOffset));
  __ Branch(eq, &adaptor, a3, Operand(Smi::FromInt(StackFrame::ARGUMENTS_ADAPTOR)));

  // Nothing to do: The formal number of parameters has already been
  // passed in register a0 by calling function. Just return it.
  __ mov(v0,a0);
  __ Ret();

  // Arguments adaptor case: Read the arguments length from the
  // adaptor frame and return it.
  __ bind(&adaptor);
  __ lw(v0, MemOperand(a2, ArgumentsAdaptorFrameConstants::kLengthOffset));
  __ Ret();
}


void ArgumentsAccessStub::GenerateReadElement(MacroAssembler* masm) {
__ break_(__LINE__);
  // The displacement is the offset of the last parameter (if any)
  // relative to the frame pointer.
  static const int kDisplacement =
      StandardFrameConstants::kCallerSPOffset - kPointerSize;

  // Check that the key is a smiGenerateReadElement.
  Label slow;
  __ BranchOnNotSmi(a1, &slow);

  // Check if the calling frame is an arguments adaptor frame.
  Label adaptor;
  __ lw(a2, MemOperand(fp, StandardFrameConstants::kCallerFPOffset));
  __ lw(a3, MemOperand(a2, StandardFrameConstants::kContextOffset));
  __ Branch(eq, &adaptor, a3, Operand(Smi::FromInt(StackFrame::ARGUMENTS_ADAPTOR)));

  // Check index against formal parameters count limit passed in
  // through register a0. Use unsigned comparison to get negative
  // check for free.
  __ break_(__LINE__);
  __ Branch(Ugreater_equal, &slow, a0, Operand(a1));

  // Read the argument from the stack and return it.
  __ sub(a0, a0, a1);
  __ sll(t3, a3, kPointerSizeLog2 - kSmiTagSize);
  __ Addu(a3, fp, Operand(t3));
  __ lw(v0, MemOperand(a3, kDisplacement));
  __ Ret();

  // Arguments adaptor case: Check index against actual arguments
  // limit found in the arguments adaptor frame. Use unsigned
  // comparison to get negative check for free.
  __ bind(&adaptor);
  __ lw(a0, MemOperand(a2, ArgumentsAdaptorFrameConstants::kLengthOffset));
  __ Branch(greater_equal, &slow, a1, Operand(a0));

  // Read the argument from the adaptor frame and return it.
  __ sub(a3, a0, a1);
  __ sll(t3, a3, kPointerSizeLog2 - kSmiTagSize);
  __ Addu(a3, a2, Operand(t3));
  __ lw(v0, MemOperand(a3, kDisplacement));
  __ Ret();

  // Slow-case: Handle non-smi or out-of-bounds access to arguments
  // by calling the runtime system.
  __ bind(&slow);
  __ Push(a1);
  __ TailCallRuntime(Runtime::kGetArgumentsProperty, 1, 1);
}


void ArgumentsAccessStub::GenerateNewObject(MacroAssembler* masm) {
  // sp[0] : number of parameters
  // sp[4] : receiver displacement
  // sp[8] : function

  // Check if the calling frame is an arguments adaptor frame.
  Label adaptor_frame, runtime;
  __ lw(t2, MemOperand(fp, StandardFrameConstants::kCallerFPOffset));
  __ lw(t3, MemOperand(t2, StandardFrameConstants::kContextOffset));
  __ Branch(ne,
      &runtime,
      t3,
      Operand(Smi::FromInt(StackFrame::ARGUMENTS_ADAPTOR)));

  // Patch the arguments.length and the parameters pointer.
  __ bind(&adaptor_frame);
  __ break_(__LINE__);
  __ lw(t1, MemOperand(t2, ArgumentsAdaptorFrameConstants::kLengthOffset));
  __ sw(t1, MemOperand(sp));
  __ sll(t0, t1, kPointerSizeLog2 - kSmiTagSize);
  __ Addu(t3, t2, t0);
  __ Addu(t3, t3, Operand(StandardFrameConstants::kCallerSPOffset));
  __ sw(t3, MemOperand(sp, 1 * kPointerSize));

  // Do the runtime call to allocate the arguments object.
  __ bind(&runtime);
  __ TailCallRuntime(Runtime::kNewArgumentsFast, 3, 1);
}


const char* CompareStub::GetName() {
  UNIMPLEMENTED_MIPS();
  return NULL;  // UNIMPLEMENTED RETURN.
}


int CompareStub::MinorKey() {
  // Encode the two parameters in a unique 16 bit value.
  ASSERT(static_cast<unsigned>(cc_) >> 28 < (1 << 15));
  return (static_cast<unsigned>(cc_) >> 27) | (strict_ ? 1 : 0);
}


// We fall into this code if the operands were Smis, but the result was
// not (eg. overflow).  We branch into this code (to the not_smi label) if
// the operands were not both Smi.  The operands are in a1 (x) and a0 (y).
// To call the C-implemented binary fp operation routines we need to end up
// with the double precision floating point operands in a0 and a1 (for the
// value in a1) and a2 and a3 (for the value in a0).

static void HandleBinaryOpSlowCases(MacroAssembler* masm,
                                    Label* not_smi,
                                    const Builtins::JavaScript& builtin,
                                    Token::Value operation,
                                    OverwriteMode mode) {
  Label slow, slow_pop_2_first, do_the_call;
  Label a0_is_smi, a1_is_smi, finished_loading_a0, finished_loading_a1;

  // Smi-smi case (overflow).
  // Since both are Smis there is no heap number to overwrite, so allocate.
  // The new heap number is in t0. t1 and t2 are scratch.
  __ AllocateHeapNumber(t0, t1, t2, &slow);

  // If we have floating point hardware, inline ADD, SUB, MUL, and DIV,
  // using registers f12 and f14 for the double values.
  bool use_fp_registers = CpuFeatures::IsSupported(FPU) &&
    Token::MOD != operation;

  if (use_fp_registers) {
    CpuFeatures::Scope scope(FPU);
    // Convert a1 (x) to double in f12
    __ sra(t2, a1, kSmiTagSize);
    __ mtc1(t2, f12);
    __ cvt_d_w(f12, f12);

    // Convert a0 (y) to double in f14
    __ sra(t2, a0, kSmiTagSize);
    __ mtc1(t2, f14);
    __ cvt_d_w(f14, f14);

  } else {
    // Write Smi from a0 to a3 and a2 in double format. t1 is scratch.
    ConvertToDoubleStub stub1(a3, a2, a0, t1);
    __ Push(ra);
    __ Call(stub1.GetCode(), RelocInfo::CODE_TARGET);

    // Write Smi from a1 to a1 and a0 in double format. t1 is scratch.
    // Needs a1 in temp (t2); cannot use same reg for src & dest.
    __ mov(t2, a1);
    ConvertToDoubleStub stub2(a1, a0, t2, t1);
    __ Call(stub2.GetCode(), RelocInfo::CODE_TARGET);
    __ Pop(ra);
  }
  __ jmp(&do_the_call);  // Tail call.  No return.

  // We jump to here if something goes wrong (one param is not a number of any
  // sort or new-space allocation fails).
  __ bind(&slow);

  // Push arguments to the stack
  __ Push(a1);
  __ Push(a0);

  if (Token::ADD == operation) {
    // Test for string arguments before calling runtime.
    // a1 : first argument
    // a0 : second argument
    // sp[0] : second argument
    // sp[4] : first argument

    Label not_strings, not_string1, string1, string1_smi2;
    __ And(t0, a1, Operand(kSmiTagMask));
    __ Branch(eq, &not_string1, t0, Operand(zero_reg));

    __ GetObjectType(a1, t0, t0);
    __ Branch(ge, &not_string1, t0, Operand(FIRST_NONSTRING_TYPE));

    // First argument is a a string, test second.
    __ And(t0, a0, Operand(kSmiTagMask));
    __ Branch(eq, &string1_smi2, t0, Operand(zero_reg));

    __ GetObjectType(a0, t0, t0);
    __ Branch(ge, &string1, t0, Operand(FIRST_NONSTRING_TYPE));

    // First and second argument are strings.
    StringAddStub string_add_stub(NO_STRING_CHECK_IN_STUB);
//  __ break_(__LINE__);
    __ TailCallStub(&string_add_stub);

    __ bind(&string1_smi2);
    NumberToStringStub::GenerateLookupNumberStringCache(
        masm, a0, a2, t0, t1, true, &string1);
    
    // Replace second argument on stack and tailcall string add stub to make
    // the result.
    __ sw(a2, MemOperand(sp, 0));
    __ TailCallStub(&string_add_stub);

    // Only first argument is a string.
    __ bind(&string1);
//    __ break_(0x3350);  // Cannot call builtins yet.
    __ InvokeBuiltin(Builtins::STRING_ADD_LEFT, JUMP_JS);

    // First argument was not a string, test second.
    __ bind(&not_string1);
    __ And(t0, a0, Operand(kSmiTagMask));
    __ Branch(eq, &not_strings, t0, Operand(zero_reg));

    __ GetObjectType(a0, t0, t0);
    __ Branch(ge, &not_strings, t0, Operand(FIRST_NONSTRING_TYPE));

    // Only second argument is a string.
    __ break_(0x3351);  // Cannot call builtins yet.
    __ InvokeBuiltin(Builtins::STRING_ADD_RIGHT, JUMP_JS);

    __ bind(&not_strings);
  }
  __ break_(0x3352);  // Cannot call builtins yet.
  __ InvokeBuiltin(builtin, JUMP_JS);  // Tail call.  No return.

  // We branch here if at least one of a0 and a1 is not a Smi.
  __ bind(not_smi);

  if (mode == NO_OVERWRITE) {
    // In the case where there is no chance of an overwritable float we may as
    // well do the allocation immediately while a0 and a1 are untouched.
    __ AllocateHeapNumber(t0, t1, t2, &slow);
  }

  // Move a0 (y) to a double in a2-a3.
  __ And(t1, a0, Operand(kSmiTagMask));
  // If it is an Smi, don't check if it is a heap number.
  __ Branch(eq, &a0_is_smi, t1, Operand(zero_reg));
  __ GetObjectType(a0, t1, t1);
  __ Branch(ne, &slow, t1, Operand(HEAP_NUMBER_TYPE));

  if (mode == OVERWRITE_RIGHT) {
    __ mov(t0, a0);  // Overwrite this heap number.
  }
  if (use_fp_registers) {
    CpuFeatures::Scope scope(FPU);
    // Load the double from tagged HeapNumber a1 to f14.
    __ Subu(t1, a0, Operand(kHeapObjectTag));
    __ ldc1(f14, MemOperand(t1, HeapNumber::kValueOffset));
  } else {
    // Calling convention says that 'right' double (x) is in a2 and a3.
    __ lw(a2, FieldMemOperand(a0, HeapNumber::kValueOffset));
    __ lw(a3, FieldMemOperand(a0, HeapNumber::kValueOffset + 4));
  }
  __ jmp(&finished_loading_a0);
  __ bind(&a0_is_smi);
  if (mode == OVERWRITE_RIGHT) {
    // We can't overwrite a Smi so get address of new heap number into t0.
    __ AllocateHeapNumber(t0, t1, t2, &slow);
  }

  if (use_fp_registers) {
    CpuFeatures::Scope scope(FPU);
    // Convert smi in a0 to double in f14.
    __ sra(t2, a0, kSmiTagSize);
    __ mtc1(t2, f14);
    __ cvt_d_w(f14, f14);
  } else {
    // Write Smi from a0 to a3 and a2 in double format.
    __ mov(t1, a0);
    ConvertToDoubleStub stub3(a3, a2, t1, t2);
    __ Push(ra);
    __ Call(stub3.GetCode(), RelocInfo::CODE_TARGET);
    __ Pop(ra);
  }

  __ bind(&finished_loading_a0);

  // Move a1 (x) to a double in a0-a1.
  __ And(t1, a1, Operand(kSmiTagMask));
  // If it is an Smi, don't check if it is a heap number.
  __ Branch(eq, &a1_is_smi, t1, Operand(zero_reg));
  __ GetObjectType(a1, t1, t1);
  __ Branch(ne, &slow, t1, Operand(HEAP_NUMBER_TYPE));
  if (mode == OVERWRITE_LEFT) {
    __ mov(t0, a1);  // Overwrite this heap number.
  }
  if (use_fp_registers) {
    CpuFeatures::Scope scope(FPU);
    // Load the double from tagged HeapNumber a1 to f12.
    __ Subu(t1, a1, Operand(kHeapObjectTag));
    __ ldc1(f12, MemOperand(t1, HeapNumber::kValueOffset));
  } else {
    __ lw(a0, FieldMemOperand(a1, HeapNumber::kValueOffset));
    __ lw(a1, FieldMemOperand(a1, HeapNumber::kValueOffset + 4));
  }
  __ jmp(&finished_loading_a1);
  __ bind(&a1_is_smi);
  if (mode == OVERWRITE_LEFT) {
    // We can't overwrite a Smi so get address of new heap number into t0.
    __ AllocateHeapNumber(t0, t1, t2, &slow);
  }

  if (use_fp_registers) {
    CpuFeatures::Scope scope(FPU);
    // Convert smi in a1 to double in f12.
    __ sra(t2, a1, kSmiTagSize);
    __ mtc1(t2, f12);
    __ cvt_d_w(f12, f12);

  } else {
    // Write Smi from a1 to a0 and a1 in double format.
    __ mov(t1, a1);
    ConvertToDoubleStub stub4(a1, a0, t1, t2);
    __ Push(ra);
    __ Call(stub4.GetCode(), RelocInfo::CODE_TARGET);
    __ Pop(ra);
  }

  __ bind(&finished_loading_a1);

  __ bind(&do_the_call);
  // If we are inlining the operation using FPU instructions for
  // add, subtract, multiply, or divide, the arguments are in f12 and f14.
  if (use_fp_registers) {
    CpuFeatures::Scope scope(FPU);
    // MIPS FPU instructions to implement
    // double precision, add, subtract, multiply, divide.
    if (Token::MUL == operation) {
      __ mul_d(f0, f12, f14);
    } else if (Token::DIV == operation) {
      __ div_d(f0, f12, f14);
    } else if (Token::ADD == operation) {
      __ add_d(f0, f12, f14);
    } else if (Token::SUB == operation) {
      __ sub_d(f0, f12, f14);
    } else {
      UNREACHABLE();
    }
    __ Subu(v0, t0, Operand(kHeapObjectTag));
    __ sdc1(f0, MemOperand(v0, HeapNumber::kValueOffset));
    __ Addu(v0, v0, Operand(kHeapObjectTag));
    __ Ret();
    return;
  }

  // If we did not inline the operation, then the arguments are in:
  // a0: Left value (least significant part of mantissa).
  // a1: Left value (sign, exponent, top of mantissa).
  // a2: Right value (least significant part of mantissa).
  // a3: Right value (sign, exponent, top of mantissa).
  // t0: Address of heap number for result.

  __ Push(ra);
  __ Push(t0);    // Address of heap number that is answer.
  __ mov(s3, sp);  // Save sp.
  __ AlignStack(0);
  // Call C routine that may not cause GC or other trouble.
  __ li(t0, Operand(ExternalReference::double_fp_operation(operation)));
  __ Call(t0);
  __ mov(sp, s3);  // Restore stack pointer.
  __ Pop(t0);  // Address of heap number.
  // Store answer in the overwritable heap number.
  // Double returned in registers v0 and v1.
  __ sw(v0, FieldMemOperand(t0, HeapNumber::kValueOffset));
  __ sw(v1, FieldMemOperand(t0, HeapNumber::kValueOffset + 4));
  __ mov(v0, t0);  // Return object ptr to caller.
  // And we are done.
  __ Pop(ra);
  __ Ret();
}


// Tries to get a signed int32 out of a double precision floating point heap
// number.  Rounds towards 0.  Fastest for doubles that are in the ranges
// -0x7fffffff to -0x40000000 or 0x40000000 to 0x7fffffff.  This corresponds
// almost to the range of signed int32 values that are not Smis.  Jumps to the
// label 'slow' if the double isn't in the range -0x80000000.0 to 0x80000000.0
// (excluding the endpoints).
static void GetInt32(MacroAssembler* masm,
                     Register source,
                     Register dest,
                     Register scratch,
                     Register scratch2,
                     Label* slow) {
  Label right_exponent, done;
  // Get exponent word (ENDIAN issues).
  __ lw(scratch, FieldMemOperand(source, HeapNumber::kExponentOffset));
  // Get exponent alone in scratch2.
  __ And(scratch2, scratch, Operand(HeapNumber::kExponentMask));
  // Load dest with zero.  We use this either for the final shift or
  // for the answer.
  __ mov(dest, zero_reg);
  // Check whether the exponent matches a 32 bit signed int that is not a Smi.
  // A non-Smi integer is 1.xxx * 2^30 so the exponent is 30 (biased).  This is
  // the exponent that we are fastest at and also the highest exponent we can
  // handle here.
  const uint32_t non_smi_exponent =
      (HeapNumber::kExponentBias + 30) << HeapNumber::kExponentShift;
  // If we have a match of the int32-but-not-Smi exponent then skip some logic.
  __ Branch(eq, &right_exponent, scratch2, Operand(non_smi_exponent));
  // If the exponent is higher than that then go to slow case.  This catches
  // numbers that don't fit in a signed int32, infinities and NaNs.
  __ Branch(gt, slow, scratch2, Operand(non_smi_exponent));

  // We know the exponent is smaller than 30 (biased).  If it is less than
  // 0 (biased) then the number is smaller in magnitude than 1.0 * 2^0, ie
  // it rounds to zero.
  const uint32_t zero_exponent =
      (HeapNumber::kExponentBias + 0) << HeapNumber::kExponentShift;
  __ Subu(scratch2, scratch2, Operand(zero_exponent));
  // Dest already has a Smi zero.
  __ Branch(lt, &done, scratch2, Operand(zero_exponent));
  if (!CpuFeatures::IsSupported(FPU)) {
    // We have a shifted exponent between 0 and 30 in scratch2.
    __ srl(dest, scratch2, HeapNumber::kExponentShift);
    // We now have the exponent in dest.  Subtract from 30 to get
    // how much to shift down.
    __ li(at, Operand(30));
    __ subu(dest, at, dest);
  }
  __ bind(&right_exponent);
  if (CpuFeatures::IsSupported(FPU)) {
    CpuFeatures::Scope scope(FPU);
    // MIPS FPU instructions implementing double precision to integer
    // conversion using round to zero. Since the FP value was qualified
    // above, the resulting integer should be a legal int32.
    // The original 'Exponent' word is still in scratch.
    __ lwc1(f12, FieldMemOperand(source, HeapNumber::kMantissaOffset));
    __ mtc1(scratch, f13);
    __ cvt_w_d(f0, f12);
    __ mfc1(dest, f0);
  } else {
    // On entry, dest has final downshift, scratch has original sign/exp/mant.
    // Save sign bit in top bit of dest.
    __ And(scratch2, scratch, Operand(0x80000000));
    __ Or(dest, dest, Operand(scratch2));
    // Put back the implicit 1, just above mantissa field.
    __ Or(scratch, scratch, Operand(1 << HeapNumber::kExponentShift));

    // Shift up the mantissa bits to take up the space the exponent used to
    // take. We just orred in the implicit bit so that took care of one and
    // we want to leave the sign bit 0 so we subtract 2 bits from the shift
    // distance. But we want to clear the sign-bit so shift one more bit
    // left, then shift right one bit.
    const int shift_distance = HeapNumber::kNonMantissaBitsInTopWord - 2;
    __ sll(scratch, scratch, shift_distance + 1);
    __ srl(scratch, scratch, 1);

    // Get the second half of the double. For some exponents we don't
    // actually need this because the bits get shifted out again, but
    // it's probably slower to test than just to do it.
    __ lw(scratch2, FieldMemOperand(source, HeapNumber::kMantissaOffset));
    // Extract the top 10 bits, and insert those bottom 10 bits of scratch.
    // The width of the field here is the same as the shift amount above.
    const int field_width = shift_distance;
    __ ext(scratch2, scratch2, 32-shift_distance, field_width);
    __ ins(scratch, scratch2, 0, field_width);
    // Move down according to the exponent.
    __ srlv(scratch, scratch, dest);
    // Prepare the negative version of our integer.
    __ subu(scratch2, zero_reg, scratch);
    // Trick to check sign bit (msb) held in dest, count leading zero.
    // 0 indicates negative, save negative version with conditional move.
    __ clz(dest, dest);
    __ movz(scratch, scratch2, dest);
    __ mov(dest, scratch);
  }
  __ bind(&done);
}



// For bitwise ops where the inputs are not both Smis we here try to determine
// whether both inputs are either Smis or at least heap numbers that can be
// represented by a 32 bit signed value.  We truncate towards zero as required
// by the ES spec.  If this is the case we do the bitwise op and see if the
// result is a Smi.  If so, great, otherwise we try to find a heap number to
// write the answer into (either by allocating or by overwriting).
// On entry the operands are in a1 (x) and a0 (y). (Result = x op y).
// On exit the result is in v0.
void GenericBinaryOpStub::HandleNonSmiBitwiseOp(MacroAssembler* masm) {
  Label slow, result_not_a_smi;
  Label a0_is_smi, a1_is_smi;
  Label done_checking_a0, done_checking_a1;

  __ And(t1, a1, Operand(kSmiTagMask));
  __ Branch(eq, &a1_is_smi, t1, Operand(zero_reg));
  __ GetObjectType(a1, t4, t4);
  __ Branch(ne, &slow, t4, Operand(HEAP_NUMBER_TYPE));
  GetInt32(masm, a1, a3, t2, t3, &slow);  // Convert HeapNum a1 to integer a3.
  __ b(&done_checking_a1);
  __ nop();   // NOP_ADDED

  __ bind(&a1_is_smi);
  __ sra(a3, a1, kSmiTagSize);  // Remove tag from Smi.
  __ bind(&done_checking_a1);

  __ And(t0, a0, Operand(kSmiTagMask));
  __ Branch(eq, &a0_is_smi, t0, Operand(zero_reg));
  __ GetObjectType(a0, t4, t4);
  __ Branch(ne, &slow, t4, Operand(HEAP_NUMBER_TYPE));
  GetInt32(masm, a0, a2, t2, t3, &slow);  // Convert HeapNum a0 to integer a2.
  __ b(&done_checking_a0);
  __ nop();   // NOP_ADDED

  __ bind(&a0_is_smi);
  __ sra(a2, a0, kSmiTagSize);  // Remove tag from Smi.
  __ bind(&done_checking_a0);

  // a1 (x) and a0 (y): Original operands (Smi or heap numbers).
  // a3 (x) and a2 (y): Signed int32 operands.

  switch (op_) {
    case Token::BIT_OR:  __ or_(v1, a3, a2); break;
    case Token::BIT_XOR: __ xor_(v1, a3, a2); break;
    case Token::BIT_AND: __ and_(v1, a3, a2); break;
    case Token::SAR:
      __ srav(v1, a3, a2);
      break;
    case Token::SHR:
      __ srlv(v1, a3, a2);
      // SHR is special because it is required to produce a positive answer.
      // The code below for writing into heap numbers isn't capable of writing
      // the register as an unsigned int so we go to slow case if we hit this
      // case.
      __ And(t3, v1, Operand(0x80000000));
      __ Branch(ne, &slow, t3, Operand(zero_reg));
      break;
    case Token::SHL:
        __ sllv(v1, a3, a2);
      break;
    default: UNREACHABLE();
  }
  // check that the *signed* result fits in a smi
  __ Addu(t3, v1, Operand(0x40000000));
  __ And(t3, t3, Operand(0x80000000));
  __ Branch(ne, &result_not_a_smi, t3, Operand(zero_reg));
  // Smi tag result.
  __ sll(v0, v1, kSmiTagMask);
  __ Ret();

  Label have_to_allocate, got_a_heap_number;
  __ bind(&result_not_a_smi);
  switch (mode_) {
    case OVERWRITE_RIGHT: {
      // t0 has not been changed since  __ andi(t0, a0, Operand(kSmiTagMask));
      __ Branch(eq, &have_to_allocate, t0, Operand(zero_reg));
      __ mov(t5, a0);
      break;
    }
    case OVERWRITE_LEFT: {
      // t1 has not been changed since  __ andi(t1, a1, Operand(kSmiTagMask));
      __ Branch(eq, &have_to_allocate, t1, Operand(zero_reg));
      __ mov(t5, a1);
      break;
    }
    case NO_OVERWRITE: {
      // Get a new heap number in t5.  t6 and t7 are scratch.
      __ AllocateHeapNumber(t5, t6, t7, &slow);
    }
    default: break;
  }

  __ bind(&got_a_heap_number);
  // v1: Result as signed int32.
  // t5: Heap number to write answer into.

  // Nothing can go wrong now, so move the heap number to v0, which is the
  // result.
  __ mov(v0, t5);

  // Tail call that writes the int32 in v1 to the heap number in v0, using
  // t0, t1 as scratch.  v0 is preserved and returned by the stub.
  WriteInt32ToHeapNumberStub stub(v1, v0, t0, t1);
  __ Jump(stub.GetCode(), RelocInfo::CODE_TARGET);

  if (mode_ != NO_OVERWRITE) {
    __ bind(&have_to_allocate);
    // Get a new heap number in t5.  t6 and t7 are scratch.
    __ AllocateHeapNumber(t5, t6, t7, &slow);
    __ b(&got_a_heap_number);
    __ nop();   // NOP_ADDED
  }

  // If all else failed then we go to the runtime system.
  __ bind(&slow);

  __ Push(a1);  // restore stack
  __ Push(a0);
  __ li(a0, Operand(1));  // 1 argument (not counting receiver).

  __ break_(0x4441);  // MIPS does not support builtins yet.

  switch (op_) {
    case Token::BIT_OR:
      __ InvokeBuiltin(Builtins::BIT_OR, JUMP_JS);
      break;
    case Token::BIT_AND:
      __ InvokeBuiltin(Builtins::BIT_AND, JUMP_JS);
      break;
    case Token::BIT_XOR:
      __ InvokeBuiltin(Builtins::BIT_XOR, JUMP_JS);
      break;
    case Token::SAR:
      __ InvokeBuiltin(Builtins::SAR, JUMP_JS);
      break;
    case Token::SHR:
      __ InvokeBuiltin(Builtins::SHR, JUMP_JS);
      break;
    case Token::SHL:
      __ InvokeBuiltin(Builtins::SHL, JUMP_JS);
      break;
    default:
      UNREACHABLE();
  }
}


const char* GenericBinaryOpStub::GetName() {
  if (name_ != NULL) return name_;
  const int len = 100;
  name_ = Bootstrapper::AllocateAutoDeletedArray(len);
  if (name_ == NULL) return "OOM";
  const char* op_name = Token::Name(op_);
  const char* overwrite_name;
  switch (mode_) {
    case NO_OVERWRITE: overwrite_name = "Alloc"; break;
    case OVERWRITE_RIGHT: overwrite_name = "OverwriteRight"; break;
    case OVERWRITE_LEFT: overwrite_name = "OverwriteLeft"; break;
    default: overwrite_name = "UnknownOverwrite"; break;
  }

  OS::SNPrintF(Vector<char>(name_, len),
               "GenericBinaryOpStub_%s_%s%s",
               op_name,
               overwrite_name,
               specialized_on_rhs_ ? "_ConstantRhs" : 0);
  return name_;
}


// Can we multiply by x with max two shifts and an add.
// This answers yes to all integers from 2 to 10.
static bool IsEasyToMultiplyBy(int x) {
  if (x < 2) return false;                          // Avoid special cases.
  if (x > (Smi::kMaxValue + 1) >> 2) return false;  // Almost always overflows.
  if (IsPowerOf2(x)) return true;                   // Simple shift.
  if (PopCountLessThanEqual2(x)) return true;       // Shift and add and shift.
  if (IsPowerOf2(x + 1)) return true;               // Patterns like 11111.
  return false;
}


// Can multiply by anything that IsEasyToMultiplyBy returns true for.
// Source and destination may be the same register.  This routine does
// not set carry and overflow the way a mul instruction would.
static void MultiplyByKnownInt(MacroAssembler* masm,
                               Register source,
                               Register destination,
                               int known_int) {
  if (IsPowerOf2(known_int)) {
    __ sll(destination, source, BitPosition(known_int));
  } else if (PopCountLessThanEqual2(known_int)) {
    int first_bit = BitPosition(known_int);
    int second_bit = BitPosition(known_int ^ (1 << first_bit));
    __ sll(t0, source, second_bit - first_bit);
    __ Addu(destination, source, Operand(t0));
    if (first_bit != 0) {
      __ sll(destination, destination, first_bit);
    }
  } else {
    ASSERT(IsPowerOf2(known_int + 1));  // Patterns like 1111.
    int the_bit = BitPosition(known_int + 1);
    __ sll(t0, source, the_bit);
    __ Subu(destination, t0, Operand(source));
  }
}



void GenericBinaryOpStub::Generate(MacroAssembler* masm) {
  // a1 : x
  // a0 : y
  // result : v0 = x op y

  // All ops need to know whether we are dealing with two Smis.  Set up t2 to
  // tell us that.
  __ Or(t2, a1, Operand(a0));  // t2 = x | y;

  switch (op_) {
    case Token::ADD: {
      Label not_smi;
      // Fast path.
      ASSERT(kSmiTag == 0);  // Adjust code below.
      __ And(t3, t2, Operand(kSmiTagMask));
      __ Branch(ne, &not_smi, t3, Operand(zero_reg));
      __ addu(v0, a1, a0);    // Add y.
      // Check for overflow.
      __ xor_(t0, v0, a0);
      __ xor_(t1, v0, a1);
      __ and_(t0, t0, t1);    // Overflow occurred if result is negative.
      __ Ret(ge, t0, Operand(zero_reg));  // Return on NO overflow (ge 0).

      // Fall thru on overflow, with a0 and a1 preserved.
      HandleBinaryOpSlowCases(masm,
                              &not_smi,
                              Builtins::ADD,
                              Token::ADD,
                              mode_);
      break;
    }

    case Token::SUB: {
      Label not_smi;
      // Fast path.
      ASSERT(kSmiTag == 0);  // Adjust code below.
      __ And(t3, t2, Operand(kSmiTagMask));
      __ Branch(ne, &not_smi, t3, Operand(zero_reg));
      __ subu(v0, a1, a0);  // Subtract y.
      // Check for overflow.
      __ xor_(t0, v0, a1);
      __ xor_(t1, a0, a1);
      __ and_(t0, t0, t1);    // Overflow occurred if result is negative.
      __ Ret(ge, t0, Operand(zero_reg));  // Return on NO overflow (ge 0).

      // Fall thru on overflow, with a0 and a1 preserved.
      HandleBinaryOpSlowCases(masm,
                              &not_smi,
                              Builtins::SUB,
                              Token::SUB,
                              mode_);
      break;
    }

    case Token::MUL: {
      Label not_smi, slow;
      ASSERT(kSmiTag == 0);  // Adjust code below.
      __ And(t3, t2, Operand(kSmiTagMask));
      __ Branch(ne, &not_smi, t3, Operand(zero_reg));
      // Remove tag from one operand (but keep sign), so that result is Smi.
      __ sra(t0, a0, kSmiTagSize);
      // Do multiplication.
      __ mult(a1, t0);
      __ mflo(v0);
      __ mfhi(v1);

      // Go 'slow' on overflow, detected if top 33 bits are not same.
      __ sra(t0, v0, 31);
      __ Branch(ne, &slow, t0, Operand(v1));

      // Return if non-zero Smi result.
      __ Ret(ne, v0, Operand(zero_reg));

      // We can return 0, if we multiplied positive number by 0.
      // We know one of them was 0, so sign of sum is sign of other.
      // (note that result of 0 is already in v0, and Smi::FromInt(0) is 0.)
      __ addu(t0, a0, a1);
      __ Ret(gt, t0, Operand(zero_reg));
      // Else, fall thru to slow case to handle -0

      __ bind(&slow);
      HandleBinaryOpSlowCases(masm,
                              &not_smi,
                              Builtins::MUL,
                              Token::MUL,
                              mode_);
      break;
    }


    case Token::DIV: {
      Label not_smi, slow;
      ASSERT(kSmiTag == 0);  // Adjust code below.

      // t2 = x | y at entry.
      __ And(t3, t2, Operand(kSmiTagMask));
      __ Branch(ne, &not_smi, t3, Operand(zero_reg));
      // Remove tags, preserving sign.
      __ sra(t0, a0, kSmiTagSize);
      __ sra(t1, a1, kSmiTagSize);
      // Check for divisor of 0.
      __ Branch(eq, &slow, t0, Operand(zero_reg));
      // Divide x by y.
      __ Div(t1, Operand(t0));
      __ mflo(v1);    // Integer (un-tagged) quotient.
      __ mfhi(v0);    // Integer remainder.

      // Go to slow (float) case if remainder is not 0.
      __ Branch(ne, &slow, v0, Operand(zero_reg));

      ASSERT(kSmiTag == 0 && kSmiTagSize == 1);
      __ sll(v0, v1, kSmiTagSize);  // Smi tag return value in v0.

      // Check for the corner case of dividing the most negative smi by -1.
      __ Branch(eq, &slow, v1, Operand(0x40000000));
      // Check for negative zero result.
      __ Ret(ne, v0, Operand(zero_reg));  // OK if result was non-zero.
      __ li(t0, Operand(0x80000000));
      __ And(t2, t2, Operand(t0));
      __ Branch(eq, &slow, t2, Operand(t0));  // Go slow if operands negative.
      __ Ret();

      __ bind(&slow);
      HandleBinaryOpSlowCases(masm,
                              &not_smi,
                              op_ == Token::MOD ? Builtins::MOD : Builtins::DIV,
                              op_,
                              mode_);
      break;
    }

    case Token::MOD: {
      Label not_smi, slow;
      ASSERT(kSmiTag == 0);  // Adjust code below.
      // t2 = x | y at entry.
      __ And(t3, t2, Operand(kSmiTagMask));
      __ Branch(ne, &not_smi, t3, Operand(zero_reg));
      // Check for divisor of 0.
      __ Branch(eq, &slow, t0, Operand(zero_reg));
      // Remove tags, preserving sign.
      __ sra(t0, a0, kSmiTagSize);
      __ sra(t1, a1, kSmiTagSize);
      __ Div(t1, Operand(t0));
      __ mfhi(v0);
      __ sll(v0, v0, kSmiTagSize);  // Smi tag return value.
      // Check for negative zero result.
      __ Ret(ne, v0, Operand(zero_reg));  // OK if result was non-zero.
      __ li(t0, Operand(0x80000000));
      __ And(t2, t2, Operand(t0));
      __ Branch(eq, &slow, t2, Operand(t0));  // Go slow if operands negative.
      __ Ret();

      __ bind(&slow);
      HandleBinaryOpSlowCases(masm,
                              &not_smi,
                              op_ == Token::MOD ? Builtins::MOD : Builtins::DIV,
                              op_,
                              mode_);
      break;
    }


    case Token::BIT_OR:
    case Token::BIT_AND:
    case Token::BIT_XOR:
    case Token::SAR:
    case Token::SHR:
    case Token::SHL: {
      Label slow;
      ASSERT(kSmiTag == 0);  // Adjust code below.
      __ And(t3, t2, Operand(kSmiTagMask));
      __ Branch(ne, &slow, t3, Operand(zero_reg));
      switch (op_) {
        case Token::BIT_OR:  __ Or(v0, a0, Operand(a1)); break;
        case Token::BIT_AND: __ And(v0, a0, Operand(a1)); break;
        case Token::BIT_XOR: __ Xor(v0, a0, Operand(a1)); break;
        case Token::SAR:
          // Remove tags from operands.
          __ sra(a2, a0, kSmiTagSize);
          __ sra(a3, a1, kSmiTagSize);
          // Shift.
          __ srav(v0, a3, a2);
          // Smi tag result.
          __ sll(v0, v0, kSmiTagMask);
          break;
        case Token::SHR:
          // Remove tags from operands.
          __ sra(a2, a0, kSmiTagSize);
          __ sra(a3, a1, kSmiTagSize);
          // Shift.
          __ srlv(v0, a3, a2);
          // Unsigned shift is not allowed to produce a negative number, so
          // check the sign bit and the sign bit after Smi tagging.
          __ And(t3, v0, Operand(0xc0000000));
          __ Branch(ne, &slow, t3, Operand(zero_reg));
          // Smi tag result.
          __ sll(v0, v0, kSmiTagMask);
          break;
        case Token::SHL:
          // Remove tags from operands.
          __ sra(a2, a0, kSmiTagSize);
          __ sra(a3, a1, kSmiTagSize);
          // Shift
          __ sllv(v0, a3, a2);
          // Check that the signed result fits in a Smi.
          __ Addu(t3, v0, Operand(0x40000000));
          __ And(t3, t3, Operand(0x80000000));
          __ Branch(ne, &slow, t3, Operand(zero_reg));
          // Smi tag result.
          __ sll(v0, v0, kSmiTagMask);
          break;
        default: UNREACHABLE();
      }
      __ Ret();
      __ bind(&slow);
      HandleNonSmiBitwiseOp(masm);
      break;
    }

    default: UNREACHABLE();
  }
  // This code should be unreachable.
  __ stop("Unreachable");
}


void StringStubBase::GenerateCopyCharacters(MacroAssembler* masm,
                                            Register dest,
                                            Register src,
                                            Register count,
                                            Register scratch,
                                            bool ascii) {
  Label loop;
  Label done;
  // This loop just copies one character at a time, as it is only used for
  // very short strings.
  if (!ascii) {
    __ addu(count, count, count);
  }
  __ Branch(eq, &done, count, Operand(zero_reg));
  __ addu(count, dest, count);  // Count now points to the last dest byte.

  __ bind(&loop);
  __ lbu(scratch, MemOperand(src));
  __ addiu(src, src, 1);
  __ sb(scratch, MemOperand(dest));
  __ addiu(dest, dest, 1);
  __ Branch(lt, &loop, dest, Operand(count));

  __ bind(&done);
}


enum CopyCharactersFlags {
  COPY_ASCII = 1,
  DEST_ALWAYS_ALIGNED = 2
};


void StringStubBase::GenerateCopyCharactersLong(MacroAssembler* masm,
                                                Register dest,
                                                Register src,
                                                Register count,
                                                Register scratch1,
                                                Register scratch2,
                                                Register scratch3,
                                                Register scratch4,
                                                Register scratch5,
                                                int flags) {
  // Postpone coding of optimized (long) version, until basics work.
  bool ascii = (flags & COPY_ASCII) != 0;
  GenerateCopyCharacters(masm, dest, src, count, scratch1, ascii);
}


void StringStubBase::GenerateTwoCharacterSymbolTableProbe(MacroAssembler* masm,
                                                          Register c1,
                                                          Register c2,
                                                          Register scratch1,
                                                          Register scratch2,
                                                          Register scratch3,
                                                          Register scratch4,
                                                          Register scratch5,
                                                          Label* not_found) {
  // Register scratch3 is the general scratch register in this function.
  Register scratch = scratch3;

//  __ break_(__LINE__);
  // Make sure that both characters are not digits as such strings has a
  // different hash algorithm. Don't try to look for these in the symbol table.
  Label not_array_index;
  __ Subu(scratch, c1, Operand(static_cast<int>('0')));
  __ Branch(Ugreater, &not_array_index, scratch, Operand(static_cast<int>('9' - '0')));
  __ Subu(scratch, c2, Operand(static_cast<int>('0')));

  // If check failed combine both characters into single halfword.
  // This is required by the contract of the method: code at the
  // not_found branch expects this combination in c1 register
  Label tmp;
  __ sll(scratch1, c2, kBitsPerByte);
  __ Branch(Ugreater, &tmp, scratch, Operand(static_cast<int>('9' - '0')));
  __ Or(c1, c1, scratch1);
  __ bind(&tmp);
  __ Branch(Uless_equal, not_found, scratch, Operand(static_cast<int>('9' - '0')));

  __ bind(&not_array_index);
//  __ break_(__LINE__);
  // Calculate the two character string hash.
  Register hash = scratch1;
  GenerateHashInit(masm, hash, c1);
  GenerateHashAddCharacter(masm, hash, c2);
  GenerateHashGetHash(masm, hash);
//  __ break_(__LINE__);

  // Collect the two characters in a register.
  Register chars = c1;
  __ sll(scratch, c2, kBitsPerByte);
  __ Or(chars, chars, scratch);

  // chars: two character string, char 1 in byte 0 and char 2 in byte 1.
  // hash:  hash of two character string.

  // Load symbol table
  // Load address of first element of the symbol table.
  Register symbol_table = c2;
  __ LoadRoot(symbol_table, Heap::kSymbolTableRootIndex);

  // Load undefined value
  Register undefined = scratch4;
  __ LoadRoot(undefined, Heap::kUndefinedValueRootIndex);

  // Calculate capacity mask from the symbol table capacity.
  Register mask = scratch2;
  __ lw(mask, FieldMemOperand(symbol_table, SymbolTable::kCapacityOffset));
  __ sra(mask, mask, 1);
  __ Addu(mask, mask, -1);

  // Calculate untagged address of the first element of the symbol table.
  Register first_symbol_table_element = symbol_table;
  __ Add(first_symbol_table_element, symbol_table,
         Operand(SymbolTable::kElementsStartOffset - kHeapObjectTag));

//  __ break_(__LINE__);
  // Registers
  // chars: two character string, char 1 in byte 0 and char 2 in byte 1.
  // hash:  hash of two character string
  // mask:  capacity mask
  // first_symbol_table_element: address of the first element of
  //                             the symbol table
  // scratch: -

  // Perform a number of probes in the symbol table.
  static const int kProbes = 4;
  Label found_in_symbol_table;
  Label next_probe[kProbes];
  for (int i = 0; i < kProbes; i++) {
    Register candidate = scratch5;  // Scratch register contains candidate.

    // Calculate entry in symbol table.
    if (i > 0) {
      __ Add(candidate, hash, Operand(SymbolTable::GetProbeOffset(i)));
    } else {
      __ mov(candidate, hash);
    }

    __ And(candidate, candidate, Operand(mask));

    // Load the entry from the symble table.
    ASSERT_EQ(1, SymbolTable::kEntrySize);
    __ sll(scratch, candidate, kPointerSizeLog2);
    __ Add(scratch, scratch, first_symbol_table_element);
    __ lw(candidate, MemOperand(scratch));

//  __ break_(__LINE__);
    // If entry is undefined no string with this hash can be found.
    __ Branch(eq, not_found, candidate, Operand(undefined));

//  __ break_(__LINE__);
    // If length is not 2 the string is not a candidate.
    __ lw(scratch, FieldMemOperand(candidate, String::kLengthOffset));
    __ Branch(ne, &next_probe[i], scratch, Operand(2));

    // Check that the candidate is a non-external ascii string.
    __ lw(scratch, FieldMemOperand(candidate, HeapObject::kMapOffset));
    __ lbu(scratch, FieldMemOperand(scratch, Map::kInstanceTypeOffset));
//  __ break_(__LINE__);
    __ JumpIfInstanceTypeIsNotSequentialAscii(scratch, scratch, &next_probe[i]);

    // Check if the two characters match.
    // Assumes that word load is little endian.
    __ lhu(scratch, FieldMemOperand(candidate, SeqAsciiString::kHeaderSize));
    __ Branch(eq, &found_in_symbol_table, chars, Operand(scratch));
    __ bind(&next_probe[i]);
  }

//  __ break_(__LINE__);
  // No matching 2 character string found by probing.
  __ jmp(not_found);

  // Scratch register contains result when we fall through to here.
  Register result = scratch;
  __ bind(&found_in_symbol_table);
  if (!result.is(v0)) {
    __ mov(v0, result);
  }
}


void StringStubBase::GenerateHashInit(MacroAssembler* masm,
                                      Register hash,
                                      Register character) {
  // hash = character + (character << 10);
  __ sll(hash, character, 10);
  __ addu(hash, hash, character);
  // hash ^= hash >> 6;
  __ sra(at, hash, 6);
  __ xor_(hash, hash, at);
}


void StringStubBase::GenerateHashAddCharacter(MacroAssembler* masm,
                                              Register hash,
                                              Register character) {
  // hash += character;
  __ addu(hash, hash, character);
  // hash += hash << 10;
  __ sll(at, hash, 10);
  __ addu(hash, hash, at);
  // hash ^= hash >> 6;
  __ sra(at, hash, 6);
  __ xor_(hash, hash, at);
}


void StringStubBase::GenerateHashGetHash(MacroAssembler* masm,
                                         Register hash) {
  // hash += hash << 3;
  __ sll(at, hash, 3);
  __ addu(hash, hash, at);
  // hash ^= hash >> 11;
  __ sra(at, hash, 11);
  __ xor_(hash, hash, at);
  // hash += hash << 15;
  __ sll(at, hash, 15);
  __ addu(hash, hash, at);

  // if (hash == 0) hash = 27;
  __ ori(at, zero_reg, 27);
  __ movz(hash, at, hash);
}


void StringCompareStub::GenerateCompareFlatAsciiStrings(MacroAssembler* masm,
                                                        Register left,
                                                        Register right,
                                                        Register scratch1,
                                                        Register scratch2,
                                                        Register scratch3,
                                                        Register scratch4) {
  UNIMPLEMENTED_MIPS();
  __ break_(__LINE__);
}


void StringCompareStub::Generate(MacroAssembler* masm) {
  Label runtime;

  // Stack frame on entry.
  //  sp[0]: right string
  //  sp[4]: left string
  __ lw(a0, MemOperand(sp, 1 * kPointerSize));  // left
  __ lw(a1, MemOperand(sp, 0 * kPointerSize));  // right

  Label not_same;
  __ Branch(ne, &not_same, a0, Operand(a1));
  ASSERT_EQ(0, EQUAL);
  ASSERT_EQ(0, kSmiTag);
  __ li(a0, Operand(Smi::FromInt(EQUAL)));
  __ IncrementCounter(&Counters::string_compare_native, 1, a1, a2);
  __ Add(sp, sp, Operand(2 * kPointerSize));
  __ Ret();

  __ bind(&not_same);
  UNIMPLEMENTED_MIPS();
  __ break_(__LINE__);

  __ bind(&runtime);
  UNIMPLEMENTED_MIPS();
  __ break_(__LINE__);
}


void StringAddStub::Generate(MacroAssembler* masm) {
  Label string_add_runtime;
  // Stack on entry:
  // sp[0]: second argument.
  // sp[4]: first argument.

  // Load the two arguments.
  __ lw(a0, MemOperand(sp, 1 * kPointerSize));  // First argument.
  __ lw(a1, MemOperand(sp, 0 * kPointerSize));  // Second argument.

  // Make sure that both arguments are strings if not known in advance.
  if (string_check_) {
    ASSERT_EQ(0, kSmiTag);
    __ JumpIfEitherSmi(a0, a1, &string_add_runtime);
    // Load instance types.
    __ lw(t0, FieldMemOperand(a0, HeapObject::kMapOffset));
    __ lw(t1, FieldMemOperand(a1, HeapObject::kMapOffset));
    __ lbu(t0, FieldMemOperand(t0, Map::kInstanceTypeOffset));
    __ lbu(t1, FieldMemOperand(t1, Map::kInstanceTypeOffset));
    ASSERT_EQ(0, kStringTag);
    // If either is not a string, go to runtime.
    __ Or(t4, t0, Operand(t1));
    __ And(t4, t4, Operand(kIsNotStringMask));
    __ Branch(ne, &string_add_runtime, t4, Operand(zero_reg));
  }

  // Both arguments are strings.
  // a0: first string
  // a1: second string
  // t0: first string instance type (if string_check_)
  // t1: second string instance type (if string_check_)
  {
    Label strings_not_empty;
    // Check if either of the strings are empty. In that case return the other.
    __ lw(a2, FieldMemOperand(a0, String::kLengthOffset));
    __ lw(a3, FieldMemOperand(a1, String::kLengthOffset));
    __ mov(v0, a0);       // Assume we'll return first string (from a0).
    __ movz(v0, a1, a2);  // If first is empty, return second (from a1).
    __ slt(t4, zero_reg, a2);   // if (a2 > 0) t4 = 1.
    __ slt(t5, zero_reg, a3);   // if (a3 > 0) t5 = 1.
    __ and_(t4, t4, t5);        // Branch if both strings were non-empty.
    __ Branch(ne, &strings_not_empty, t0, Operand(zero_reg));

    __ IncrementCounter(&Counters::string_add_native, 1, a2, a3);
    __ Add(sp, sp, Operand(2 * kPointerSize));
    __ Ret();

    __ bind(&strings_not_empty);
  }

  // Both strings are non-empty.
  // a0: first string
  // a1: second string
  // a2: length of first string
  // a3: length of second string
  // t0: first string instance type (if string_check_)
  // t1: second string instance type (if string_check_)
  // Look at the length of the result of adding the two strings.
  Label string_add_flat_result, longer_than_two;
  // Adding two lengths can't overflow.
  ASSERT(String::kMaxLength * 2 > String::kMaxLength);
  __ Add(t2, a2, Operand(a3));
  // Use the runtime system when adding two one character strings, as it
  // contains optimizations for this specific case using the symbol table.
  __ Branch(ne, &longer_than_two, t2, Operand(2));

  // Check that both strings are non-external ascii strings.
  if (!string_check_) {
    __ lw(t0, FieldMemOperand(a0, HeapObject::kMapOffset));
    __ lw(t1, FieldMemOperand(a1, HeapObject::kMapOffset));
    __ lbu(t0, FieldMemOperand(t0, Map::kInstanceTypeOffset));
    __ lbu(t1, FieldMemOperand(t1, Map::kInstanceTypeOffset));
  }
  __ JumpIfBothInstanceTypesAreNotSequentialAscii(t0, t1, t2, t3,
                                                  &string_add_runtime);

  // Get the two characters forming the sub string.
  __ lbu(a2, FieldMemOperand(a0, SeqAsciiString::kHeaderSize));
  __ lbu(a3, FieldMemOperand(a1, SeqAsciiString::kHeaderSize));

  // Try to lookup two character string in symbol table. If it is not found
  // just allocate a new one.
  Label make_two_character_string;
  GenerateTwoCharacterSymbolTableProbe(masm, a2, a3, t2, t3, t0, t1, t4,
                                       &make_two_character_string);
  __ IncrementCounter(&Counters::string_add_native, 1, a2, a3);
  __ Add(sp, sp, Operand(2 * kPointerSize));
  __ Ret();

  __ bind(&make_two_character_string);
  // Resulting string has length 2 and first chars of two strings
  // are combined into single halfword in a2 register.
  // So we can fill resulting string without two loops by a single
  // halfword store instruction (which assumes that processor is
  // in a little endian mode)
  __ li(t2, Operand(2));
  __ AllocateAsciiString(a0, t2, t0, t1, t4, &string_add_runtime);
  __ sh(a2, FieldMemOperand(a0, SeqAsciiString::kHeaderSize));
  __ IncrementCounter(&Counters::string_add_native, 1, a2, a3);
  __ Add(sp, sp, Operand(2 * kPointerSize));
  __ Ret();

  __ bind(&longer_than_two);
  // Check if resulting string will be flat.
  __ Branch(lt, &string_add_flat_result, t2,
            Operand(String::kMinNonFlatLength));
  // Handle exceptionally long strings in the runtime system.
  ASSERT((String::kMaxLength & 0x80000000) == 0);
  ASSERT(IsPowerOf2(String::kMaxLength + 1));
  // kMaxLength + 1 is representable as shifted literal, kMaxLength is not.
  __ Branch(hs, &string_add_runtime, t2, Operand(String::kMaxLength + 1));

  // If result is not supposed to be flat, allocate a cons string object.
  // If both strings are ascii the result is an ascii cons string.
  if (!string_check_) {
    __ lw(t0, FieldMemOperand(a0, HeapObject::kMapOffset));
    __ lw(t1, FieldMemOperand(a1, HeapObject::kMapOffset));
    __ lbu(t0, FieldMemOperand(t0, Map::kInstanceTypeOffset));
    __ lbu(t1, FieldMemOperand(t1, Map::kInstanceTypeOffset));
  }
  Label non_ascii, allocated;
  ASSERT_EQ(0, kTwoByteStringTag);
  // Branch to non_ascii if either string-encoding field is zero (non-ascii).
  __ And(t4, t2, Operand(t3));
  __ And(t4, t4, Operand(kStringEncodingMask));
  __ Branch(eq, &non_ascii, t4, Operand(zero_reg));

  // Allocate an ASCII cons string.
  __ AllocateAsciiConsString(t3, t2, t0, t1, &string_add_runtime);
  __ bind(&allocated);
  // Fill the fields of the cons string.
  __ sw(a0, FieldMemOperand(t3, ConsString::kFirstOffset));
  __ sw(a1, FieldMemOperand(t3, ConsString::kSecondOffset));
  __ mov(v0, t3);
  __ IncrementCounter(&Counters::string_add_native, 1, a2, a3);
  __ Add(sp, sp, Operand(2 * kPointerSize));
  __ Ret();

  __ bind(&non_ascii);
  // Allocate a two byte cons string.
  __ AllocateTwoByteConsString(t3, t2, t0, t1, &string_add_runtime);
  __ Branch(al, &allocated);

  // Handle creating a flat result. First check that both strings are
  // sequential and that they have the same encoding.
  // a0: first string
  // a1: second string
  // a2: length of first string
  // a3: length of second string
  // t0: first string instance type (if string_check_)
  // t1: second string instance type (if string_check_)
  // t2: sum of lengths.
  __ bind(&string_add_flat_result);
  if (!string_check_) {
    __ lw(t0, FieldMemOperand(a0, HeapObject::kMapOffset));
    __ lw(t1, FieldMemOperand(a1, HeapObject::kMapOffset));
    __ lbu(t0, FieldMemOperand(t0, Map::kInstanceTypeOffset));
    __ lbu(t1, FieldMemOperand(t1, Map::kInstanceTypeOffset));
  }
  // Check that both strings are sequential, meaning that we
  // branch to runtime if either string tag is non-zero.
  ASSERT_EQ(0, kSeqStringTag);
  __ Or(t4, t0, Operand(t1));
  __ And(t4, t4, Operand(kStringRepresentationMask));
  __ Branch(ne, &string_add_runtime, t4, Operand(zero_reg));

  // Now check if both strings have the same encoding (ASCII/Two-byte).
  // a0: first string
  // a1: second string
  // a2: length of first string
  // a3: length of second string
  // t0: first string instance type
  // t1: second string instance type
  // t2: sum of lengths.
  Label non_ascii_string_add_flat_result;
  ASSERT(IsPowerOf2(kStringEncodingMask));  // Just one bit to test.
  __ xor_(t3, t1, t0);
  __ And(t3, t3, Operand(kStringEncodingMask));
  __ Branch(ne, &string_add_runtime, t3, Operand(zero_reg));
  // And see if it's ASCII (0) or two-byte (1).
  __ And(t3, t0, Operand(kStringEncodingMask));
  __ Branch(eq, &non_ascii_string_add_flat_result, t3, Operand(zero_reg));

  // Both strings are sequential ASCII strings. We also know that they are
  // short (since the sum of the lengths is less than kMinNonFlatLength).
  // t2: length of resulting flat string
  __ AllocateAsciiString(t3, t2, t0, t1, t4, &string_add_runtime);
  // Locate first character of result.
  __ Add(t2, t3, Operand(SeqAsciiString::kHeaderSize - kHeapObjectTag));
  // Locate first character of first argument.
  __ Add(a0, a0, Operand(SeqAsciiString::kHeaderSize - kHeapObjectTag));
  // a0: first character of first string.
  // a1: second string.
  // a2: length of first string.
  // a3: length of second string.
  // t2: first character of result.
  // t3: result string.
  GenerateCopyCharacters(masm, t2, a0, a2, t0, true);

  // Load second argument and locate first character.
  __ Add(a1, a1, Operand(SeqAsciiString::kHeaderSize - kHeapObjectTag));
  // a1: first character of second string.
  // a3: length of second string.
  // t2: next character of result.
  // t3: result string.
  GenerateCopyCharacters(masm, t2, a1, a3, t0, true);
  __ mov(v0, t3);
  __ IncrementCounter(&Counters::string_add_native, 1, a2, a3);
  __ Add(sp, sp, Operand(2 * kPointerSize));
  __ Ret();

  __ bind(&non_ascii_string_add_flat_result);
  // Both strings are sequential two byte strings.
  // a0: first string.
  // a1: second string.
  // a2: length of first string.
  // a3: length of second string.
  // t2: sum of length of strings.
  __ AllocateTwoByteString(t3, t2, t0, t1, t4, &string_add_runtime);
  // a0: first string.
  // a1: second string.
  // a2: length of first string.
  // a3: length of second string.
  // t3: result string.

  // Locate first character of result.
  __ Add(t2, t3, Operand(SeqTwoByteString::kHeaderSize - kHeapObjectTag));
  // Locate first character of first argument.
  __ Add(a0, a0, Operand(SeqTwoByteString::kHeaderSize - kHeapObjectTag));

  // a0: first character of first string.
  // a1: second string.
  // a2: length of first string.
  // a3: length of second string.
  // t2: first character of result.
  // t3: result string.
  GenerateCopyCharacters(masm, t2, a0, a2, t0, false);

  // Locate first character of second argument.
  __ Add(a1, a1, Operand(SeqTwoByteString::kHeaderSize - kHeapObjectTag));

  // a1: first character of second string.
  // a3: length of second string.
  // t2: next character of result (after copy of first string).
  // t3: result string.
  GenerateCopyCharacters(masm, t2, a1, a3, t0, false);

  __ mov(v0, t3);
  __ IncrementCounter(&Counters::string_add_native, 1, a2, a3);
  __ Add(sp, sp, Operand(2 * kPointerSize));
  __ Ret();

  // Just jump to runtime to add the two strings.
  __ bind(&string_add_runtime);
  __ break_(0x3340);  // We cannot do runtime calls yet.
  __ TailCallRuntime(Runtime::kStringAdd, 2, 1);
}


void CallFunctionStub::Generate(MacroAssembler* masm) {
  Label slow;

  // If the receiver might be a value (string, number or boolean) check for this
  // and box it if it is.
  if (ReceiverMightBeValue()) {
    __ break_(__LINE__);
    // Get the receiver from the stack.
    // function, receiver [, arguments]
    Label receiver_is_value, receiver_is_js_object;
    __ lw(a1, MemOperand(sp, argc_ * kPointerSize));

    // Check if receiver is a smi (which is a number value).
    __ BranchOnSmi(a1, &receiver_is_value);

    // Check if the receiver is a valid JS object.
    __ GetObjectType(a1, a2, a2);
    __ Branch(greater_equal, &receiver_is_js_object, a2, Operand(FIRST_JS_OBJECT_TYPE));

    // Call the runtime to box the value.
    __ bind(&receiver_is_value);
    // We need natives to execute this.
    __ break_(__LINE__);
    __ EnterInternalFrame();
    __ Push(a1);
    __ InvokeBuiltin(Builtins::TO_OBJECT, CALL_JS);
    __ LeaveInternalFrame();
    __ sw(a0, MemOperand(sp, argc_ * kPointerSize));

    __ bind(&receiver_is_js_object);
  }

  // Get the function to call from the stack.
  // function, receiver [, arguments]
  __ lw(a1, MemOperand(sp, (argc_ + 1) * kPointerSize));

  // Check that the function is really a JavaScript function.
  // a1: pushed function (to be verified)
  __ BranchOnSmi(a1, &slow);
  // Get the map of the function object.
  __ GetObjectType(a1, a2, a2);
  __ Branch(ne, &slow, a2, Operand(JS_FUNCTION_TYPE));

  // Fast-case: Invoke the function now.
  // a1: pushed function
  ParameterCount actual(argc_);
  __ InvokeFunction(a1, actual, JUMP_FUNCTION);

  // Slow-case: Non-function called.
  __ bind(&slow);
  __ break_(__LINE__);
  // CALL_NON_FUNCTION expects the non-function callee as receiver (instead
  // of the original receiver from the call site).
  __ sw(a1, MemOperand(sp, argc_ * kPointerSize));
  __ li(a0, Operand(argc_));  // Setup the number of arguments.
  __ mov(a2, zero_reg);
  __ GetBuiltinEntry(a3, Builtins::CALL_NON_FUNCTION);
  __ Jump(Handle<Code>(Builtins::builtin(Builtins::ArgumentsAdaptorTrampoline)),
          RelocInfo::CODE_TARGET);
}


#undef __

} }  // namespace v8::internal
