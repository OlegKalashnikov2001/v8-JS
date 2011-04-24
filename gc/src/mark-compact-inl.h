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

#ifndef V8_MARK_COMPACT_INL_H_
#define V8_MARK_COMPACT_INL_H_

#include "isolate.h"
#include "memory.h"
#include "mark-compact.h"

namespace v8 {
namespace internal {

MarkBit Marking::MarkBitFromNewSpace(HeapObject* obj) {
  ASSERT(heap_->InNewSpace(obj));
  uint32_t index = heap_->new_space()->AddressToMarkbitIndex(
      reinterpret_cast<Address>(obj));
  return new_space_bitmap_->MarkBitFromIndex(index);
}


MarkBit Marking::MarkBitFromOldSpace(HeapObject* obj) {
  ASSERT(!HEAP->InNewSpace(obj));
  ASSERT(obj->IsHeapObject());
  Address addr = reinterpret_cast<Address>(obj);
  Page *p = Page::FromAddress(addr);
  return p->markbits()->MarkBitFromIndex(p->AddressToMarkbitIndex(addr));
}


MarkBit Marking::MarkBitFrom(Address addr) {
  if (heap_->InNewSpace(addr)) {
    uint32_t index = heap_->new_space()->AddressToMarkbitIndex(addr);
    return new_space_bitmap_->MarkBitFromIndex(index);
  } else {
    Page *p = Page::FromAddress(addr);
    return p->markbits()->MarkBitFromIndex(p->AddressToMarkbitIndex(addr),
                                           p->ContainsOnlyData());
  }
}


void Marking::ClearRange(Address addr, int size) {
  if (heap_->InNewSpace(addr)) {
    uint32_t index = heap_->new_space()->AddressToMarkbitIndex(addr);
    new_space_bitmap_->ClearRange(index, size >> kPointerSizeLog2);
  } else {
    Page *p = Page::FromAddress(addr);
    p->markbits()->ClearRange(p->FastAddressToMarkbitIndex(addr),
                              size >> kPointerSizeLog2);
  }
}


void MarkCompactCollector::SetFlags(int flags) {
  force_compaction_ = ((flags & Heap::kForceCompactionMask) != 0);
  sweep_precisely_ = ((flags & Heap::kMakeHeapIterableMask) != 0);
}


void MarkCompactCollector::MarkObject(HeapObject* obj, MarkBit mark_bit) {
  ASSERT(heap()->marking()->MarkBitFrom(obj) == mark_bit);
  if (!mark_bit.Get()) {
    mark_bit.Set();
    tracer_->increment_marked_count();
#ifdef DEBUG
    UpdateLiveObjectCount(obj);
#endif
    ProcessNewlyMarkedObject(obj);
  }
}

void MarkCompactCollector::SetMark(HeapObject* obj, MarkBit mark_bit) {
  ASSERT(heap()->marking()->MarkBitFrom(obj) == mark_bit);
  mark_bit.Set();
  tracer_->increment_marked_count();
#ifdef DEBUG
  UpdateLiveObjectCount(obj);
#endif
}


bool MarkCompactCollector::IsMarked(Object* obj) {
  ASSERT(obj->IsHeapObject());
  HeapObject* heap_object = HeapObject::cast(obj);
  return heap_->marking()->MarkBitFrom(heap_object).Get();
}


} }  // namespace v8::internal

#endif  // V8_MARK_COMPACT_INL_H_
