// Copyright 2023 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_SANDBOX_ISOLATE_INL_H_
#define V8_SANDBOX_ISOLATE_INL_H_

#include "src/execution/isolate.h"

namespace v8 {
namespace internal {

template <typename IsolateT>
IsolateForSandbox::IsolateForSandbox(IsolateT* isolate)
#ifdef V8_ENABLE_SANDBOX
    : isolate_(isolate->ForSandbox()) {
}
#else
{
}
#endif

#ifdef V8_ENABLE_SANDBOX
ExternalPointerTable& IsolateForSandbox::GetExternalPointerTableFor(
    ExternalPointerTag tag) {
  IsolateForPointerCompression isolate(isolate_);
  return isolate.GetExternalPointerTableFor(tag);
}

ExternalPointerTable::Space* IsolateForSandbox::GetExternalPointerTableSpaceFor(
    ExternalPointerTag tag, Address host) {
  IsolateForPointerCompression isolate(isolate_);
  return isolate.GetExternalPointerTableSpaceFor(tag, host);
}

ExternalBufferTable& IsolateForSandbox::GetExternalBufferTableFor(
    ExternalBufferTag tag) {
  DCHECK_NE(tag, kExternalBufferNullTag);
  return IsSharedExternalBufferType(tag)
             ? isolate_->shared_external_buffer_table()
             : isolate_->external_buffer_table();
}

ExternalBufferTable::Space* IsolateForSandbox::GetExternalBufferTableSpaceFor(
    ExternalBufferTag tag, Address host) {
  DCHECK_NE(tag, kExternalBufferNullTag);

  if (V8_UNLIKELY(IsSharedExternalBufferType(tag))) {
    DCHECK(!ReadOnlyHeap::Contains(host));
    return isolate_->shared_external_buffer_space();
  }

  return isolate_->heap()->external_buffer_space();
}

CodePointerTable::Space* IsolateForSandbox::GetCodePointerTableSpaceFor(
    Address owning_slot) {
  return ReadOnlyHeap::Contains(owning_slot)
             ? isolate_->read_only_heap()->code_pointer_space()
             : isolate_->heap()->code_pointer_space();
}

TrustedPointerTable& IsolateForSandbox::GetTrustedPointerTable() {
  return isolate_->trusted_pointer_table();
}

TrustedPointerTable::Space* IsolateForSandbox::GetTrustedPointerTableSpace() {
  return isolate_->heap()->trusted_pointer_space();
}

#endif  // V8_ENABLE_SANDBOX

template <typename IsolateT>
IsolateForPointerCompression::IsolateForPointerCompression(IsolateT* isolate)
#ifdef V8_COMPRESS_POINTERS
    : isolate_(isolate->ForSandbox()) {
}
#else
{
}
#endif

#ifdef V8_COMPRESS_POINTERS

ExternalPointerTable& IsolateForPointerCompression::GetExternalPointerTableFor(
    ExternalPointerTag tag) {
  DCHECK_NE(tag, kExternalPointerNullTag);
  return IsSharedExternalPointerType(tag)
             ? isolate_->shared_external_pointer_table()
             : isolate_->external_pointer_table();
}

ExternalPointerTable::Space*
IsolateForPointerCompression::GetExternalPointerTableSpaceFor(
    ExternalPointerTag tag, Address host) {
  DCHECK_NE(tag, kExternalPointerNullTag);
  DCHECK_IMPLIES(tag != kArrayBufferExtensionTag, V8_ENABLE_SANDBOX_BOOL);

  if (V8_UNLIKELY(IsSharedExternalPointerType(tag))) {
    DCHECK(!ReadOnlyHeap::Contains(host));
    return isolate_->shared_external_pointer_space();
  }

  if (V8_UNLIKELY(IsMaybeReadOnlyExternalPointerType(tag) &&
                  ReadOnlyHeap::Contains(host))) {
    return isolate_->heap()->read_only_external_pointer_space();
  }

  if (MemoryChunk::FromAddress(host)->InYoungGeneration()) {
    return isolate_->heap()->young_external_pointer_space();
  }

  return isolate_->heap()->old_external_pointer_space();
}

ExternalPointerTable& IsolateForPointerCompression::GetCppHeapPointerTable() {
  return isolate_->cpp_heap_pointer_table();
}

ExternalPointerTable::Space*
IsolateForPointerCompression::GetCppHeapPointerTableSpace() {
  return isolate_->heap()->cpp_heap_pointer_space();
}

#endif  // V8_COMPRESS_POINTERS

}  // namespace internal
}  // namespace v8

#endif  // V8_SANDBOX_ISOLATE_INL_H_
