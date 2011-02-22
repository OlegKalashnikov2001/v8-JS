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

#include "runtime-profiler.h"

#include "assembler.h"
#include "code-stubs.h"
#include "compilation-cache.h"
#include "deoptimizer.h"
#include "execution.h"
#include "global-handles.h"
#include "platform.h"
#include "scopeinfo.h"

namespace v8 {
namespace internal {


class PendingListNode : public Malloced {
 public:
  explicit PendingListNode(JSFunction* function);
  ~PendingListNode() { Destroy(); }

  PendingListNode* next() const { return next_; }
  void set_next(PendingListNode* node) { next_ = node; }
  Handle<JSFunction> function() { return Handle<JSFunction>::cast(function_); }

  // If the function is garbage collected before we've had the chance
  // to optimize it the weak handle will be null.
  bool IsValid() { return !function_.is_null(); }

  // Returns the number of microseconds this node has been pending.
  int Delay() const { return static_cast<int>(OS::Ticks() - start_); }

 private:
  void Destroy();
  static void WeakCallback(v8::Persistent<v8::Value> object, void* data);

  PendingListNode* next_;
  Handle<Object> function_;  // Weak handle.
  int64_t start_;
};


// Optimization sampler constants.
static const int kSamplerFrameCount = 2;
static const int kSamplerFrameWeight[kSamplerFrameCount] = { 2, 1 };

static const int kSamplerTicksDelta = 32;

static const int kSamplerThresholdInit = 3;
static const int kSamplerThresholdMin = 1;
static const int kSamplerThresholdDelta = 1;

static const int kSamplerThresholdSizeFactorInit = 3;
static const int kSamplerThresholdSizeFactorMin = 1;
static const int kSamplerThresholdSizeFactorDelta = 1;

static const int kSizeLimit = 1500;


PendingListNode::PendingListNode(JSFunction* function) : next_(NULL) {
  GlobalHandles* global_handles = Isolate::Current()->global_handles();
  function_ = global_handles->Create(function);
  start_ = OS::Ticks();
  global_handles->MakeWeak(function_.location(), this, &WeakCallback);
}


void PendingListNode::Destroy() {
  if (!IsValid()) return;
  GlobalHandles* global_handles = Isolate::Current()->global_handles();
  global_handles->Destroy(function_.location());
  function_= Handle<Object>::null();
}


void PendingListNode::WeakCallback(v8::Persistent<v8::Value>, void* data) {
  reinterpret_cast<PendingListNode*>(data)->Destroy();
}


static bool IsOptimizable(JSFunction* function) {
  Code* code = function->code();
  return code->kind() == Code::FUNCTION && code->optimizable();
}


Atomic32 RuntimeProfiler::state_ = 0;
// TODO(isolates): Create the semaphore lazily and clean it up when no
// longer required.
Semaphore* RuntimeProfiler::semaphore_ = OS::CreateSemaphore(0);


RuntimeProfiler::RuntimeProfiler(Isolate* isolate)
    : isolate_(isolate),
      sampler_threshold_(kSamplerThresholdInit),
      sampler_threshold_size_factor_(kSamplerThresholdSizeFactorInit),
      sampler_window_position_(0),
      optimize_soon_list_(NULL) {
  ClearSampleBuffer();
}


bool RuntimeProfiler::IsEnabled() {
  return V8::UseCrankshaft() && FLAG_opt;
}


void RuntimeProfiler::Optimize(JSFunction* function, bool eager, int delay) {
  ASSERT(IsOptimizable(function));
  if (FLAG_trace_opt) {
    PrintF("[marking (%s) ", eager ? "eagerly" : "lazily");
    function->PrintName();
    PrintF(" for recompilation");
    if (delay > 0) {
      PrintF(" (delayed %0.3f ms)", static_cast<double>(delay) / 1000);
    }
    PrintF("]\n");
  }

  // The next call to the function will trigger optimization.
  function->MarkForLazyRecompilation();
}


void RuntimeProfiler::AttemptOnStackReplacement(JSFunction* function) {
  // See AlwaysFullCompiler (in compiler.cc) comment on why we need
  // Debug::has_break_points().
  ASSERT(function->IsMarkedForLazyRecompilation());
  if (!FLAG_use_osr ||
      isolate_->debug()->has_break_points() ||
      function->IsBuiltin()) {
    return;
  }

  SharedFunctionInfo* shared = function->shared();
  // If the code is not optimizable, don't try OSR.
  if (!shared->code()->optimizable()) return;

  // We are not prepared to do OSR for a function that already has an
  // allocated arguments object.  The optimized code would bypass it for
  // arguments accesses, which is unsound.  Don't try OSR.
  if (shared->scope_info()->HasArgumentsShadow()) return;

  // We're using on-stack replacement: patch the unoptimized code so that
  // any back edge in any unoptimized frame will trigger on-stack
  // replacement for that frame.
  if (FLAG_trace_osr) {
    PrintF("[patching stack checks in ");
    function->PrintName();
    PrintF(" for on-stack replacement]\n");
  }

  // Get the stack check stub code object to match against.  We aren't
  // prepared to generate it, but we don't expect to have to.
  StackCheckStub check_stub;
  Object* check_code;
  MaybeObject* maybe_check_code = check_stub.TryGetCode();
  if (maybe_check_code->ToObject(&check_code)) {
    Code* replacement_code =
        isolate_->builtins()->builtin(Builtins::OnStackReplacement);
    Code* unoptimized_code = shared->code();
    // Iterate the unoptimized code and patch every stack check except at
    // the function entry.  This code assumes the function entry stack
    // check appears first i.e., is not deferred or otherwise reordered.
    bool first = true;
    for (RelocIterator it(unoptimized_code, RelocInfo::kCodeTargetMask);
         !it.done();
         it.next()) {
      RelocInfo* rinfo = it.rinfo();
      if (rinfo->target_address() == Code::cast(check_code)->entry()) {
        if (first) {
          first = false;
        } else {
          Deoptimizer::PatchStackCheckCode(rinfo, replacement_code);
        }
      }
    }
  }
}


void RuntimeProfiler::ClearSampleBuffer() {
  memset(sampler_window_, 0, sizeof(sampler_window_));
  memset(sampler_window_weight_, 0, sizeof(sampler_window_weight_));
}


void RuntimeProfiler::ClearSampleBufferNewSpaceEntries() {
  for (int i = 0; i < kSamplerWindowSize; i++) {
    if (isolate_->heap()->InNewSpace(sampler_window_[i])) {
      sampler_window_[i] = NULL;
      sampler_window_weight_[i] = 0;
    }
  }
}


int RuntimeProfiler::LookupSample(JSFunction* function) {
  int weight = 0;
  for (int i = 0; i < kSamplerWindowSize; i++) {
    Object* sample = sampler_window_[i];
    if (sample != NULL) {
      if (function == sample) {
        weight += sampler_window_weight_[i];
      }
    }
  }
  return weight;
}


void RuntimeProfiler::AddSample(JSFunction* function, int weight) {
  ASSERT(IsPowerOf2(kSamplerWindowSize));
  sampler_window_[sampler_window_position_] = function;
  sampler_window_weight_[sampler_window_position_] = weight;
  sampler_window_position_ = (sampler_window_position_ + 1) &
      (kSamplerWindowSize - 1);
}


void RuntimeProfiler::OptimizeNow() {
  HandleScope scope(isolate_);
  PendingListNode* current = optimize_soon_list_;
  while (current != NULL) {
    PendingListNode* next = current->next();
    if (current->IsValid()) {
      Handle<JSFunction> function = current->function();
      int delay = current->Delay();
      if (IsOptimizable(*function)) {
        Optimize(*function, true, delay);
      }
    }
    delete current;
    current = next;
  }
  optimize_soon_list_ = NULL;

  // Run through the JavaScript frames and collect them. If we already
  // have a sample of the function, we mark it for optimizations
  // (eagerly or lazily).
  JSFunction* samples[kSamplerFrameCount];
  int count = 0;
  for (JavaScriptFrameIterator it;
       count < kSamplerFrameCount && !it.done();
       it.Advance()) {
    JavaScriptFrame* frame = it.frame();
    JSFunction* function = JSFunction::cast(frame->function());
    int function_size = function->shared()->SourceSize();
    int threshold_size_factor;
    if (function_size > kSizeLimit) {
      threshold_size_factor = sampler_threshold_size_factor_;
    } else {
      threshold_size_factor = 1;
    }

    int threshold = sampler_threshold_ * threshold_size_factor;
    samples[count++] = function;
    if (function->IsMarkedForLazyRecompilation()) {
      Code* unoptimized = function->shared()->code();
      int nesting = unoptimized->allow_osr_at_loop_nesting_level();
      if (nesting == 0) AttemptOnStackReplacement(function);
      int new_nesting = Min(nesting + 1, Code::kMaxLoopNestingMarker);
      unoptimized->set_allow_osr_at_loop_nesting_level(new_nesting);
    } else if (LookupSample(function) >= threshold) {
      if (IsOptimizable(function)) {
        Optimize(function, false, 0);
        isolate_->compilation_cache()->MarkForEagerOptimizing(
            Handle<JSFunction>(function, isolate_));
      }
    }
  }

  // Add the collected functions as samples. It's important not to do
  // this as part of collecting them because this will interfere with
  // the sample lookup in case of recursive functions.
  for (int i = 0; i < count; i++) {
    AddSample(samples[i], kSamplerFrameWeight[i]);
  }
}


void RuntimeProfiler::OptimizeSoon(JSFunction* function) {
  if (!IsOptimizable(function)) return;
  PendingListNode* node = new PendingListNode(function);
  node->set_next(optimize_soon_list_);
  optimize_soon_list_ = node;
}


void RuntimeProfiler::NotifyTick() {
  isolate_->stack_guard()->RequestRuntimeProfilerTick();
}


void RuntimeProfiler::MarkCompactPrologue(bool is_compacting) {
  if (is_compacting) {
    // Clear all samples before mark-sweep-compact because every
    // function might move.
    ClearSampleBuffer();
  } else {
    // Clear only new space entries on mark-sweep since none of the
    // old-space functions will move.
    ClearSampleBufferNewSpaceEntries();
  }
}


void RuntimeProfiler::Setup() {
  ClearSampleBuffer();
  // If the ticker hasn't already started, make sure to do so to get
  // the ticks for the runtime profiler.
  if (IsEnabled()) isolate_->logger()->EnsureTickerStarted();
}


void RuntimeProfiler::Reset() {
  sampler_threshold_ = kSamplerThresholdInit;
  sampler_threshold_size_factor_ = kSamplerThresholdSizeFactorInit;
}


void RuntimeProfiler::TearDown() {
  // Nothing to do.
}


Object** RuntimeProfiler::SamplerWindowAddress() {
  return sampler_window_;
}


int RuntimeProfiler::SamplerWindowSize() {
  return kSamplerWindowSize;
}


void RuntimeProfiler::HandleWakeUp(Isolate* isolate) {
  // The profiler thread must still be waiting.
  ASSERT(NoBarrier_Load(&state_) >= 0);
  // In IsolateEnteredJS we have already incremented the counter and
  // undid the decrement done by the profiler thread. Increment again
  // to get the right count of active isolates.
  NoBarrier_AtomicIncrement(&state_, 1);
  semaphore_->Signal();
  isolate->ResetEagerOptimizingData();
}


bool RuntimeProfiler::IsSomeIsolateInJS() {
  return NoBarrier_Load(&state_) > 0;
}


bool RuntimeProfiler::WaitForSomeIsolateToEnterJS() {
  Atomic32 old_state = NoBarrier_CompareAndSwap(&state_, 0, -1);
  ASSERT(old_state >= -1);
  if (old_state != 0) return false;
  semaphore_->Wait();
  return true;
}


void RuntimeProfiler::WakeUpRuntimeProfilerThreadBeforeShutdown() {
  semaphore_->Signal();
}


bool RuntimeProfilerRateLimiter::SuspendIfNecessary() {
  if (!RuntimeProfiler::IsEnabled()) return false;
  static const int kNonJSTicksThreshold = 100;
  if (RuntimeProfiler::IsSomeIsolateInJS()) {
    non_js_ticks_ = 0;
  } else {
    if (non_js_ticks_ < kNonJSTicksThreshold) {
      ++non_js_ticks_;
    } else {
      return RuntimeProfiler::WaitForSomeIsolateToEnterJS();
    }
  }
  return false;
}


} }  // namespace v8::internal
