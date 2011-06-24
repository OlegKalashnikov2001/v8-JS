// Copyright 2011 the V8 project authors. All rights reserved.
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

#ifndef V8_INCREMENTAL_MARKING_H_
#define V8_INCREMENTAL_MARKING_H_


#include "execution.h"
#include "mark-compact.h"
#include "objects.h"

namespace v8 {
namespace internal {


// TODO(gc) rename into IncrementalMarker after merge.
class IncrementalMarking {
 public:
  enum State {
    STOPPED,
    SWEEPING,
    MARKING,
    COMPLETE
  };

  explicit IncrementalMarking(Heap* heap);

  void TearDown();

  State state() {
    ASSERT(state_ == STOPPED || FLAG_incremental_marking);
    return state_;
  }

  bool should_hurry() { return should_hurry_; }

  inline bool IsStopped() { return state() == STOPPED; }

  inline bool IsMarking() { return state() >= MARKING; }

  inline bool IsMarkingIncomplete() { return state() == MARKING; }

  bool WorthActivating();

  void Start();

  void Stop();

  void PrepareForScavenge();

  void UpdateMarkingDequeAfterScavenge();

  void Hurry();

  void Finalize();

  void Abort();

  void MarkingComplete();

  // It's hard to know how much work the incremental marker should do to make
  // progress in the face of the mutator creating new work for it.  We start
  // of at a moderate rate of work and gradually increase the speed of the
  // incremental marker until it completes.
  // Do some marking every time this much memory has been allocated.
  static const intptr_t kAllocatedThreshold = 8192;
  // Start off by marking this many times more memory than has been allocated.
  static const intptr_t kInitialAllocationMarkingFactor = 4;
  // After this many steps we increase the marking/allocating factor.
  static const intptr_t kAllocationMarkingFactorSpeedupInterval = 1024;
  // This is how much we increase the marking/allocating factor by.
  static const intptr_t kAllocationMarkingFactorSpeedup = 4;

  void Step(intptr_t allocated);

  inline void RestartIfNotMarking() {
    if (state_ == COMPLETE) {
      state_ = MARKING;
      if (FLAG_trace_incremental_marking) {
        PrintF("[IncrementalMarking] Restarting (new grey objects)\n");
      }
    }
  }

  static void RecordWriteFromCode(HeapObject* obj,
                                  Object* value,
                                  Isolate* isolate);

  inline void RecordWrite(HeapObject* obj, Object* value);
  inline void RecordWriteOf(HeapObject* value);
  inline void RecordWrites(HeapObject* obj);

  inline void BlackToGreyAndUnshift(HeapObject* obj, MarkBit mark_bit);

  inline void WhiteToGreyAndPush(HeapObject* obj, MarkBit mark_bit);

  inline void WhiteToGrey(HeapObject* obj, MarkBit mark_bit);

  // Does white->black or grey->grey
  inline void MarkBlackOrKeepGrey(MarkBit mark_bit) {
    ASSERT(!Marking::IsImpossible(mark_bit));
    if (mark_bit.Get()) return;
    mark_bit.Set();
    ASSERT(!Marking::IsWhite(mark_bit));
    ASSERT(!Marking::IsImpossible(mark_bit));
  }

  inline int steps_count() {
    return steps_count_;
  }

  inline double steps_took() {
    return steps_took_;
  }

  inline int delta_steps_count() {
    return delta_steps_count_;
  }

  inline double delta_steps_took() {
    return delta_steps_took_;
  }

  inline void SetOldSpacePageFlags(MemoryChunk* chunk) {
    SetOldSpacePageFlags(chunk, IsMarking());
  }

  inline void SetNewSpacePageFlags(NewSpacePage* chunk) {
    SetNewSpacePageFlags(chunk, IsMarking());
  }

  MarkingDeque* marking_deque() { return &marking_deque_; }

 private:
  void set_should_hurry(bool val) {
    should_hurry_ = val;
  }

  void ResetStepCounters() {
    steps_count_ = 0;
    steps_took_ = 0;
    delta_steps_count_ = 0;
    delta_steps_took_ = 0;
    allocation_marking_factor_ = kInitialAllocationMarkingFactor;
  }

  static void ClearMarkbits(PagedSpace* space);
  static void ClearMarkbits(NewSpace* space);
  void ClearMarkbits();

#ifdef DEBUG
  void VerifyMarkbitsAreClean();
  static void VerifyMarkbitsAreClean(PagedSpace* space);
  static void VerifyMarkbitsAreClean(NewSpace* space);
#endif

  void StartMarking();

  void DeactivateIncrementalWriteBarrierForSpace(PagedSpace* space);
  void DeactivateIncrementalWriteBarrierForSpace(NewSpace* space);
  void DeactivateIncrementalWriteBarrier();

  static void SetOldSpacePageFlags(MemoryChunk* chunk, bool is_marking);
  static void SetNewSpacePageFlags(NewSpacePage* chunk, bool is_marking);

  void EnsureMarkingDequeIsCommitted();

  Heap* heap_;

  State state_;

  VirtualMemory* marking_deque_memory_;
  MarkingDeque marking_deque_;

  int steps_count_;
  double steps_took_;
  int delta_steps_count_;
  double delta_steps_took_;
  bool should_hurry_;
  int allocation_marking_factor_;
  intptr_t allocated_;
};

} }  // namespace v8::internal

#endif  // V8_INCREMENTAL_MARKING_H_
