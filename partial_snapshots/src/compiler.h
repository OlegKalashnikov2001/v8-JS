// Copyright 2006-2008 the V8 project authors. All rights reserved.
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

#ifndef V8_COMPILER_H_
#define V8_COMPILER_H_

#include "frame-element.h"
#include "parser.h"
#include "zone.h"

namespace v8 {
namespace internal {

// CompilationInfo encapsulates some information known at compile time.
class CompilationInfo BASE_EMBEDDED {
 public:
  CompilationInfo(Handle<SharedFunctionInfo> shared_info,
                  Handle<Object> receiver,
                  int loop_nesting)
      : shared_info_(shared_info),
        receiver_(receiver),
        loop_nesting_(loop_nesting),
        has_this_properties_(false),
        has_globals_(false) {
  }

  Handle<SharedFunctionInfo> shared_info() { return shared_info_; }

  bool has_receiver() { return !receiver_.is_null(); }
  Handle<Object> receiver() { return receiver_; }

  int loop_nesting() { return loop_nesting_; }

  bool has_this_properties() { return has_this_properties_; }
  void set_has_this_properties(bool flag) { has_this_properties_ = flag; }

  bool has_globals() { return has_globals_; }
  void set_has_globals(bool flag) { has_globals_ = flag; }

 private:
  Handle<SharedFunctionInfo> shared_info_;
  Handle<Object> receiver_;
  int loop_nesting_;
  bool has_this_properties_;
  bool has_globals_;
};


// The V8 compiler
//
// General strategy: Source code is translated into an anonymous function w/o
// parameters which then can be executed. If the source code contains other
// functions, they will be compiled and allocated as part of the compilation
// of the source code.

// Please note this interface returns function boilerplates.
// This means you need to call Factory::NewFunctionFromBoilerplate
// before you have a real function with context.

class Compiler : public AllStatic {
 public:
  enum ValidationState { VALIDATE_JSON, DONT_VALIDATE_JSON };

  // All routines return a JSFunction.
  // If an error occurs an exception is raised and
  // the return handle contains NULL.

  // Compile a String source within a context.
  static Handle<JSFunction> Compile(Handle<String> source,
                                    Handle<Object> script_name,
                                    int line_offset, int column_offset,
                                    v8::Extension* extension,
                                    ScriptDataImpl* script_Data,
                                    NativesFlag is_natives_code);

  // Compile a String source within a context for Eval.
  static Handle<JSFunction> CompileEval(Handle<String> source,
                                        Handle<Context> context,
                                        bool is_global,
                                        ValidationState validation);

  // Compile from function info (used for lazy compilation). Returns
  // true on success and false if the compilation resulted in a stack
  // overflow.
  static bool CompileLazy(CompilationInfo* info);

  // Compile a function boilerplate object (the function is possibly
  // lazily compiled). Called recursively from a backend code
  // generator 'caller' to build the boilerplate.
  static Handle<JSFunction> BuildBoilerplate(FunctionLiteral* node,
                                             Handle<Script> script,
                                             AstVisitor* caller);

  // Set the function info for a newly compiled function.
  static void SetFunctionInfo(Handle<JSFunction> fun,
                              FunctionLiteral* lit,
                              bool is_toplevel,
                              Handle<Script> script);

 private:

#if defined ENABLE_LOGGING_AND_PROFILING || defined ENABLE_OPROFILE_AGENT
  static void LogCodeCreateEvent(Logger::LogEventsAndTags tag,
                                 Handle<String> name,
                                 Handle<String> inferred_name,
                                 int start_position,
                                 Handle<Script> script,
                                 Handle<Code> code);
#endif
};


// During compilation we need a global list of handles to constants
// for frame elements.  When the zone gets deleted, we make sure to
// clear this list of handles as well.
class CompilationZoneScope : public ZoneScope {
 public:
  explicit CompilationZoneScope(ZoneScopeMode mode) : ZoneScope(mode) { }
  virtual ~CompilationZoneScope() {
    if (ShouldDeleteOnExit()) {
      FrameElement::ClearConstantList();
      Result::ClearConstantList();
    }
  }
};


} }  // namespace v8::internal

#endif  // V8_COMPILER_H_
