// Copyright 2008 the V8 project authors. All rights reserved.
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

// A simple interpreter for the Regexp2000 byte code.


#include "v8.h"
#include "utils.h"
#include "ast.h"
#include "bytecodes-re2k.h"
#include "interpreter-re2k.h"


namespace v8 { namespace internal {


#ifdef DEBUG
# define BYTECODE(name) break;                                            \
                        case BC_##name:                                   \
                          if (FLAG_trace_regexp_bytecodes) {              \
                            PrintF("pc = %d, current = %d, bc = "         \
                                    #name "\n", pc - code_base, current); \
                          }
#else
# define BYTECODE(name) break;                                            \
                        case BC_##name:
#endif



static bool RawMatch(const byte* code_base,
                     Vector<const uc16> subject,
                     int* registers,
                     int current) {
  const byte* pc = code_base;
  int backtrack_stack[1000];
  int backtrack_stack_space = 1000;
  int* backtrack_sp = backtrack_stack;
  int current_char = -1;
#ifdef DEBUG
  if (FLAG_trace_regexp_bytecodes) {
    PrintF("\n\nStart bytecode interpreter\n\n");
  }
#endif
  while (true) {
    switch (*pc) {
      BYTECODE(BREAK)
        UNREACHABLE();
        return false;
      BYTECODE(PUSH_CP)
        if (--backtrack_stack_space < 0) {
          return false;  // No match on backtrack stack overflow.
        }
        *backtrack_sp++ = current + Load32(pc + 1);
        pc += 5;
      BYTECODE(PUSH_BT)
        if (--backtrack_stack_space < 0) {
          return false;  // No match on backtrack stack overflow.
        }
        *backtrack_sp++ = Load32(pc + 1);
        pc += 5;
      BYTECODE(PUSH_REGISTER)
        if (--backtrack_stack_space < 0) {
          return false;  // No match on backtrack stack overflow.
        }
        *backtrack_sp++ = registers[pc[1]];
        pc += 2;
      BYTECODE(SET_REGISTER)
        registers[pc[1]] = Load32(pc + 2);
        pc += 6;
      BYTECODE(ADVANCE_REGISTER)
        registers[pc[1]] += Load32(pc + 2);
        pc += 6;
      BYTECODE(SET_REGISTER_TO_CP)
        registers[pc[1]] = current + Load32(pc + 2);
        pc += 6;
      BYTECODE(POP_CP)
        backtrack_stack_space++;
        --backtrack_sp;
        current = *backtrack_sp;
        pc += 1;
      BYTECODE(POP_BT)
        backtrack_stack_space++;
        --backtrack_sp;
        pc = code_base + *backtrack_sp;
      BYTECODE(POP_REGISTER)
        backtrack_stack_space++;
        --backtrack_sp;
        registers[pc[1]] = *backtrack_sp;
        pc += 2;
      BYTECODE(FAIL)
        return false;
      BYTECODE(SUCCEED)
        return true;
      BYTECODE(ADVANCE_CP)
        current += Load32(pc + 1);
        pc += 5;
      BYTECODE(GOTO)
        pc = code_base + Load32(pc + 1);
      BYTECODE(LOAD_CURRENT_CHAR) {
        int pos = current + Load32(pc + 1);
        if (pos >= subject.length()) {
          pc = code_base + Load32(pc + 5);
        } else {
          current_char = subject[pos];
          pc += 9;
        }
      }
      BYTECODE(CHECK_CHAR) {
        int c = Load16(pc + 1);
        if (c != current_char) {
          pc = code_base + Load32(pc + 3);
        } else {
          pc += 7;
        }
      }
      BYTECODE(CHECK_NOT_CHAR) {
        int c = Load16(pc + 1);
        if (c == current_char) {
          pc = code_base + Load32(pc + 3);
        } else {
          pc += 7;
        }
      }
      BYTECODE(CHECK_RANGE) {
        int start = Load16(pc + 1);
        int end = Load16(pc + 3);
        if (current_char < start || current_char > end) {
          pc = code_base + Load32(pc + 5);
        } else {
          pc += 9;
        }
      }
      BYTECODE(CHECK_NOT_RANGE) {
        int start = Load16(pc + 1);
        int end = Load16(pc + 3);
        if (current_char >= start && current_char <= end) {
          pc = code_base + Load32(pc + 5);
        } else {
          pc += 9;
        }
      }
      BYTECODE(CHECK_REGISTER_LT)
        if (registers[pc[1]] < Load16(pc + 2)) {
          pc = code_base + Load32(pc + 4);
        } else {
          pc += 8;
        }
      BYTECODE(CHECK_REGISTER_GE)
        if (registers[pc[1]] >= Load16(pc + 2)) {
          pc = code_base + Load32(pc + 4);
        } else {
          pc += 8;
        }
      BYTECODE(LOOKUP_MAP1) {
        // Look up character in a bitmap.  If we find a 0, then jump to the
        // location at pc + 7.  Otherwise fall through!
        int index = current_char - Load16(pc + 1);
        byte map = code_base[Load32(pc + 3) + (index >> 3)];
        map = ((map >> (index & 7)) & 1);
        if (map == 0) {
          pc = code_base + Load32(pc + 7);
        } else {
          pc += 11;
        }
      }
      BYTECODE(LOOKUP_MAP2) {
        // Look up character in a half-nibble map.  If we find 00, then jump to
        // the location at pc + 7.   If we find 01 then jump to location at
        // pc + 11, etc.
        int index = (current_char - Load16(pc + 1)) << 1;
        byte map = code_base[Load32(pc + 3) + (index >> 3)];
        map = ((map >> (index & 7)) & 3);
        if (map < 2) {
          if (map == 0) {
            pc = code_base + Load32(pc + 7);
          } else {
            pc = code_base + Load32(pc + 11);
          }
        } else {
          if (map == 2) {
            pc = code_base + Load32(pc + 15);
          } else {
            pc = code_base + Load32(pc + 19);
          }
        }
      }
      BYTECODE(LOOKUP_MAP8) {
        // Look up character in a byte map.  Use the byte as an index into a
        // table that follows this instruction immediately.
        int index = current_char - Load16(pc + 1);
        byte map = code_base[Load32(pc + 3) + index];
        const byte* new_pc = code_base + Load32(pc + 7) + (map << 2);
        pc = code_base + Load32(new_pc);
      }
      BYTECODE(LOOKUP_HI_MAP8) {
        // Look up high byte of this character in a byte map.  Use the byte as
        // an index into a table that follows this instruction immediately.
        int index = (current_char >> 8) - pc[1];
        byte map = code_base[Load32(pc + 2) + index];
        const byte* new_pc = code_base + Load32(pc + 6) + (map << 2);
        pc = code_base + Load32(new_pc);
      }
      BYTECODE(CHECK_BACKREF)
        UNREACHABLE();
      BYTECODE(CHECK_NOT_BACKREF)
        UNREACHABLE();
        break;  // Last one doesn't have break in macro.
      default:
        UNREACHABLE();
        break;
    }
  }
}


bool Re2kInterpreter::Match(Handle<ByteArray> code_array,
                            Handle<String> subject,
                            int* registers,
                            int start_position) {
  const byte* code_base = code_array->GetDataStartAddress();
  ASSERT(subject->IsFlat(StringShape(*subject)));
  Handle<String> flat_two_byte = RegExpImpl::CachedStringToTwoByte(subject);
  ASSERT(StringShape(*flat_two_byte).IsTwoByteRepresentation());
  return RawMatch(code_base,
                  flat_two_byte->ToUC16Vector(),
                  registers,
                  start_position);
}

} }  // namespace v8::internal
