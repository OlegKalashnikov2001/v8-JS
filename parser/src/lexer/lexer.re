// Portions of this code based on re2c:
//   (re2c/examples/push.re)
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

#include "lexer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// FIXME: some of this is probably not needed.
#include "allocation.h"
#include "ast.h"
#include "preparse-data-format.h"
#include "preparse-data.h"
#include "scopes.h"
#include "preparser.h"
#include "api.h"
#include "ast.h"
#include "bootstrapper.h"
#include "char-predicates-inl.h"
#include "codegen.h"
#include "compiler.h"
#include "func-name-inferrer.h"
#include "messages.h"
#include "parser.h"
#include "platform.h"
#include "preparser.h"
#include "runtime.h"
#include "scanner-character-streams.h"
#include "scopeinfo.h"
#include "string-stream.h"

#include "experimental-scanner.h"

// TODO:
// - Run-time lexing modifications: harmony number literals, keywords depending
//   on harmony_modules, harmony_scoping
// - Escaping the string literals (like the baseline does)
// - Error recovery after illegal tokens.

enum Condition {
  kConditionNormal,
  kConditionDoubleQuoteString,
  kConditionSingleQuoteString,
  kConditionIdentifier,
  kConditionIdentifierIllegal,
  kConditionSingleLineComment,
  kConditionMultiLineComment,
  kConditionHtmlComment
};

using namespace v8::internal;

#define PUSH_TOKEN(T) { send(T); SKIP(); }
#define PUSH_TOKEN_LOOKAHEAD(T) { --cursor_; send(T); SKIP(); }
#define PUSH_EOF_AND_RETURN() { send(Token::EOS); eof_ = true; return 1;}
#define PUSH_LINE_TERMINATOR() { just_seen_line_terminator_ = true; SKIP(); }
#define TERMINATE_ILLEGAL() { send(Token::ILLEGAL); send(Token::EOS); return 1; }

PushScanner::PushScanner(ExperimentalScanner* sink, UnicodeCache* unicode_cache)
: unicode_cache_(unicode_cache),
  eof_(false),
  state_(-1),
  condition_(kConditionNormal),
  limit_(NULL),
  start_(NULL),
  cursor_(NULL),
  marker_(NULL),
  real_start_(0),
  buffer_(NULL),
  buffer_end_(NULL),
  yych(0),
  yyaccept(0),
  just_seen_line_terminator_(true),
  sink_(sink) {

}

PushScanner::~PushScanner() {
}


uc32 PushScanner::ScanHexNumber(int length) {
  // We have seen \uXXXX, let's see what it is.
  // FIXME: we never end up in here if only a subset of the 4 chars are valid
  // hex digits -> handle the case where they're not.
  uc32 x = 0;
  for (YYCTYPE* s = cursor_ - length; s != cursor_; ++s) {
    int d = HexValue(*s);
    if (d < 0) {
      return -1;
    }
    x = x * 16 + d;
  }
  return x;
}


bool PushScanner::ValidIdentifierPart() {
  return unicode_cache_->IsIdentifierPart(ScanHexNumber(4));
}

bool PushScanner::ValidIdentifierStart() {
  return unicode_cache_->IsIdentifierStart(ScanHexNumber(4));
}

void PushScanner::send(Token::Value token) {
  int beg = (start_ - buffer_) + real_start_;
  int end = (cursor_ - buffer_) + real_start_;
  if (FLAG_trace_lexer) {
    printf("got %s at (%d, %d): ", Token::Name(token), beg, end);
    for (YYCTYPE* s = start_; s != cursor_; s++) printf("%c", (char)*s);
    printf(".\n");
  }
  just_seen_line_terminator_ = false;
  sink_->Record(token, beg, end);
}

uint32_t PushScanner::push(const void *input, int input_size) {
  if (FLAG_trace_lexer) {
    printf(
        "scanner is receiving a new data batch of length %d\n"
        "scanner continues with saved state_ = %d\n",
        input_size,
        state_);
  }

  //  Data source is signaling end of file when batch size
  //  is less than max_fill. This is slightly annoying because
  //  max_fill is a value that can only be known after re2c does
  //  its thing. Practically though, max_fill is never bigger than
  //  the longest keyword, so given our grammar, 32 is a safe bet.

  YYCTYPE null[64];
  const int max_fill = 32;
  if (input_size < max_fill) { // FIXME: do something about this!!!
    eof_ = true;
    input = null;
    input_size = sizeof(null);
    memset(null, 0, sizeof(null));
  }


  //  When we get here, we have a partially
  //  consumed buffer_ which is in the following state_:
  //                                 last valid char    last valid buffer_ spot
  //                                 v           v
  //  +-------------------+-------------+---------------+-------------+----------------------+
  //  ^          ^       ^        ^       ^           ^
  //  buffer_       start_     marker_     cursor_    limit_         buffer_end_
  //
  //  We need to stretch the buffer_ and concatenate the new chunk of input to it

  size_t used = limit_ - buffer_;
  size_t needed = used + input_size;
  size_t allocated = buffer_end_ - buffer_;
  if (allocated < needed) {
    size_t limit__offset = limit_ - buffer_;
    size_t start_offset = start_ - buffer_;
    size_t marker__offset = marker_ - buffer_;
    size_t cursor__offset = cursor_ - buffer_;

    buffer_ = (YYCTYPE*)realloc(buffer_, needed);
    buffer_end_ = needed + buffer_;

    marker_ = marker__offset + buffer_;
    cursor_ = cursor__offset + buffer_;
    start_ = buffer_ + start_offset;
    limit_ = limit__offset + buffer_;
  }
  memcpy(limit_, input, input_size);
  limit_ += input_size;

#define SKIP()                { start_ = cursor_; YYSETCONDITION(kConditionNormal); goto yyc_Normal; }
#define YYFILL(n)             { goto fill;        }

#define YYGETSTATE()          state_
#define YYSETSTATE(x)         { state_ = (x); }

#define YYGETCONDITION()      condition_
#define YYSETCONDITION(x)     { condition_ = (x); }
#define YYDEBUG(state, current) {printf("%d: |%c| (%d)\n", state, current, (int)(current));}

start_:
  if (FLAG_trace_lexer) {
    printf("Starting a round; state_: %d, condition_: %d\n", state_, condition_);
  }

  /*!re2c
    re2c:indent:top = 1;
    re2c:yych:conversion = 0;
    re2c:condenumprefix = kCondition;
    re2c:define:YYCONDTYPE = Condition;
    re2c:define:YYCURSOR = cursor_;
    re2c:define:YYLIMIT = limit_;
    re2c:define:YYMARKER = marker_;

    eof = "\000";
    any = [\000-\377];
    whitespace_char = [ \t\v\f\r\240];
    whitespace = whitespace_char+;
    identifier_start = [$_a-zA-Z\300-\377];
    identifier_char = [$_a-zA-Z0-9\300-\377];
    not_identifier_char = any\identifier_char\[\\];
    line_terminator = [\n\r]+;
    digit = [0-9];
    hex_digit = [0-9a-fA-F];
    maybe_exponent = ('e' [-+]? digit+)?;
    number = ('0x' hex_digit+) | ("." digit+ maybe_exponent) | (digit+ ("." digit*)? maybe_exponent);

    <Normal> "break" not_identifier_char      { PUSH_TOKEN_LOOKAHEAD(Token::BREAK); }
    <Normal> "case" not_identifier_char       { PUSH_TOKEN_LOOKAHEAD(Token::CASE); }
    <Normal> "catch" not_identifier_char      { PUSH_TOKEN_LOOKAHEAD(Token::CATCH); }
    <Normal> "class" not_identifier_char      { PUSH_TOKEN_LOOKAHEAD(Token::FUTURE_RESERVED_WORD); }
    <Normal> "const" not_identifier_char      { PUSH_TOKEN_LOOKAHEAD(Token::CONST); }
    <Normal> "continue" not_identifier_char   { PUSH_TOKEN_LOOKAHEAD(Token::CONTINUE); }
    <Normal> "debugger" not_identifier_char   { PUSH_TOKEN_LOOKAHEAD(Token::DEBUGGER); }
    <Normal> "default" not_identifier_char    { PUSH_TOKEN_LOOKAHEAD(Token::DEFAULT); }
    <Normal> "delete" not_identifier_char     { PUSH_TOKEN_LOOKAHEAD(Token::DELETE); }
    <Normal> "do" not_identifier_char         { PUSH_TOKEN_LOOKAHEAD(Token::DO); }
    <Normal> "else" not_identifier_char       { PUSH_TOKEN_LOOKAHEAD(Token::ELSE); }
    <Normal> "enum" not_identifier_char       { PUSH_TOKEN_LOOKAHEAD(Token::FUTURE_RESERVED_WORD); }
    <Normal> "export" not_identifier_char     { PUSH_TOKEN_LOOKAHEAD(Token::FUTURE_RESERVED_WORD); }
    <Normal> "extends" not_identifier_char    { PUSH_TOKEN_LOOKAHEAD(Token::FUTURE_RESERVED_WORD); }
    <Normal> "false" not_identifier_char      { PUSH_TOKEN_LOOKAHEAD(Token::FALSE_LITERAL); }
    <Normal> "finally" not_identifier_char    { PUSH_TOKEN_LOOKAHEAD(Token::FINALLY); }
    <Normal> "for" not_identifier_char        { PUSH_TOKEN_LOOKAHEAD(Token::FOR); }
    <Normal> "function" not_identifier_char   { PUSH_TOKEN_LOOKAHEAD(Token::FUNCTION); }
    <Normal> "if" not_identifier_char         { PUSH_TOKEN_LOOKAHEAD(Token::IF); }
    <Normal> "implements" not_identifier_char { PUSH_TOKEN_LOOKAHEAD(Token::FUTURE_STRICT_RESERVED_WORD); }
    <Normal> "import" not_identifier_char     { PUSH_TOKEN_LOOKAHEAD(Token::FUTURE_RESERVED_WORD); }
    <Normal> "in" not_identifier_char         { PUSH_TOKEN_LOOKAHEAD(Token::IN); }
    <Normal> "instanceof" not_identifier_char { PUSH_TOKEN_LOOKAHEAD(Token::INSTANCEOF); }
    <Normal> "interface" not_identifier_char  { PUSH_TOKEN_LOOKAHEAD(Token::FUTURE_STRICT_RESERVED_WORD); }
    <Normal> "let" not_identifier_char        { PUSH_TOKEN_LOOKAHEAD(Token::FUTURE_STRICT_RESERVED_WORD); }
    <Normal> "new" not_identifier_char        { PUSH_TOKEN_LOOKAHEAD(Token::NEW); }
    <Normal> "null" not_identifier_char       { PUSH_TOKEN_LOOKAHEAD(Token::NULL_LITERAL); }
    <Normal> "package" not_identifier_char    { PUSH_TOKEN_LOOKAHEAD(Token::FUTURE_STRICT_RESERVED_WORD); }
    <Normal> "private" not_identifier_char    { PUSH_TOKEN_LOOKAHEAD(Token::FUTURE_STRICT_RESERVED_WORD); }
    <Normal> "protected" not_identifier_char  { PUSH_TOKEN_LOOKAHEAD(Token::FUTURE_STRICT_RESERVED_WORD); }
    <Normal> "public" not_identifier_char     { PUSH_TOKEN_LOOKAHEAD(Token::FUTURE_STRICT_RESERVED_WORD); }
    <Normal> "return" not_identifier_char     { PUSH_TOKEN_LOOKAHEAD(Token::RETURN); }
    <Normal> "static" not_identifier_char     { PUSH_TOKEN_LOOKAHEAD(Token::FUTURE_STRICT_RESERVED_WORD); }
    <Normal> "super" not_identifier_char      { PUSH_TOKEN_LOOKAHEAD(Token::FUTURE_RESERVED_WORD); }
    <Normal> "switch" not_identifier_char     { PUSH_TOKEN_LOOKAHEAD(Token::SWITCH); }
    <Normal> "this" not_identifier_char       { PUSH_TOKEN_LOOKAHEAD(Token::THIS); }
    <Normal> "throw" not_identifier_char      { PUSH_TOKEN_LOOKAHEAD(Token::THROW); }
    <Normal> "true" not_identifier_char       { PUSH_TOKEN_LOOKAHEAD(Token::TRUE_LITERAL); }
    <Normal> "try" not_identifier_char        { PUSH_TOKEN_LOOKAHEAD(Token::TRY); }
    <Normal> "typeof" not_identifier_char     { PUSH_TOKEN_LOOKAHEAD(Token::TYPEOF); }
    <Normal> "var" not_identifier_char        { PUSH_TOKEN_LOOKAHEAD(Token::VAR); }
    <Normal> "void" not_identifier_char       { PUSH_TOKEN_LOOKAHEAD(Token::VOID); }
    <Normal> "while" not_identifier_char      { PUSH_TOKEN_LOOKAHEAD(Token::WHILE); }
    <Normal> "with" not_identifier_char       { PUSH_TOKEN_LOOKAHEAD(Token::WITH); }
    <Normal> "yield" not_identifier_char      { PUSH_TOKEN_LOOKAHEAD(Token::YIELD); }

    <Normal> "|="          { PUSH_TOKEN(Token::ASSIGN_BIT_OR); }
    <Normal> "^="          { PUSH_TOKEN(Token::ASSIGN_BIT_XOR); }
    <Normal> "&="          { PUSH_TOKEN(Token::ASSIGN_BIT_AND); }
    <Normal> "+="          { PUSH_TOKEN(Token::ASSIGN_ADD); }
    <Normal> "-="          { PUSH_TOKEN(Token::ASSIGN_SUB); }
    <Normal> "*="          { PUSH_TOKEN(Token::ASSIGN_MUL); }
    <Normal> "/="          { PUSH_TOKEN(Token::ASSIGN_DIV); }
    <Normal> "%="          { PUSH_TOKEN(Token::ASSIGN_MOD); }

    <Normal> "==="         { PUSH_TOKEN(Token::EQ_STRICT); }
    <Normal> "=="          { PUSH_TOKEN(Token::EQ); }
    <Normal> "="           { PUSH_TOKEN(Token::ASSIGN); }
    <Normal> "!=="         { PUSH_TOKEN(Token::NE_STRICT); }
    <Normal> "!="          { PUSH_TOKEN(Token::NE); }
    <Normal> "!"           { PUSH_TOKEN(Token::NOT); }

    <Normal> "//"          :=> SingleLineComment
    <Normal> whitespace* "-->" { if (just_seen_line_terminator_) { YYSETCONDITION(kConditionSingleLineComment); goto yyc_SingleLineComment; } else { --cursor_; send(Token::DEC); start_ = cursor_; goto yyc_Normal; } }
    <Normal> "/*"          :=> MultiLineComment
    <Normal> "<!--"        :=> HtmlComment

    <Normal> ">>>="        { PUSH_TOKEN(Token::ASSIGN_SHR); }
    <Normal> ">>>"         { PUSH_TOKEN(Token::SHR); }
    <Normal> "<<="         { PUSH_TOKEN(Token::ASSIGN_SHL); }
    <Normal> ">>="         { PUSH_TOKEN(Token::ASSIGN_SAR); }
    <Normal> "<="          { PUSH_TOKEN(Token::LTE); }
    <Normal> ">="          { PUSH_TOKEN(Token::GTE); }
    <Normal> "<<"          { PUSH_TOKEN(Token::SHL); }
    <Normal> ">>"          { PUSH_TOKEN(Token::SAR); }
    <Normal> "<"           { PUSH_TOKEN(Token::LT); }
    <Normal> ">"           { PUSH_TOKEN(Token::GT); }

    <Normal> number not_identifier_char { PUSH_TOKEN_LOOKAHEAD(Token::NUMBER); }
    <Normal> number any   { PUSH_TOKEN_LOOKAHEAD(Token::ILLEGAL); }

    <Normal> "("           { PUSH_TOKEN(Token::LPAREN); }
    <Normal> ")"           { PUSH_TOKEN(Token::RPAREN); }
    <Normal> "["           { PUSH_TOKEN(Token::LBRACK); }
    <Normal> "]"           { PUSH_TOKEN(Token::RBRACK); }
    <Normal> "{"           { PUSH_TOKEN(Token::LBRACE); }
    <Normal> "}"           { PUSH_TOKEN(Token::RBRACE); }
    <Normal> ":"           { PUSH_TOKEN(Token::COLON); }
    <Normal> ";"           { PUSH_TOKEN(Token::SEMICOLON); }
    <Normal> "."           { PUSH_TOKEN(Token::PERIOD); }
    <Normal> "?"           { PUSH_TOKEN(Token::CONDITIONAL); }
    <Normal> "++"          { PUSH_TOKEN(Token::INC); }
    <Normal> "--"          { PUSH_TOKEN(Token::DEC); }

    <Normal> "||"          { PUSH_TOKEN(Token::OR); }
    <Normal> "&&"          { PUSH_TOKEN(Token::AND); }

    <Normal> "|"           { PUSH_TOKEN(Token::BIT_OR); }
    <Normal> "^"           { PUSH_TOKEN(Token::BIT_XOR); }
    <Normal> "&"           { PUSH_TOKEN(Token::BIT_AND); }
    <Normal> "+"           { PUSH_TOKEN(Token::ADD); }
    <Normal> "-"           { PUSH_TOKEN(Token::SUB); }
    <Normal> "*"           { PUSH_TOKEN(Token::MUL); }
    <Normal> "/"           { PUSH_TOKEN(Token::DIV); }
    <Normal> "%"           { PUSH_TOKEN(Token::MOD); }
    <Normal> "~"           { PUSH_TOKEN(Token::BIT_NOT); }
    <Normal> ","           { PUSH_TOKEN(Token::COMMA); }

    <Normal> line_terminator  { PUSH_LINE_TERMINATOR(); }
    <Normal> whitespace       { SKIP(); }

    <Normal> ["]           :=> DoubleQuoteString
    <Normal> [']           :=> SingleQuoteString

    <Normal> identifier_start     :=> Identifier
    <Normal> "\\u" [0-9a-fA-F]{4} { if (ValidIdentifierStart()) { YYSETCONDITION(kConditionIdentifier); goto yyc_Identifier; } send(Token::ILLEGAL); start_ = cursor_; goto yyc_Normal; }
    <Normal> "\\"                 { PUSH_TOKEN(Token::ILLEGAL); }

    <Normal> eof           { PUSH_EOF_AND_RETURN();}
    <Normal> any           { PUSH_TOKEN(Token::ILLEGAL); }

    <DoubleQuoteString> "\\\\"  { goto yyc_DoubleQuoteString; }
    <DoubleQuoteString> "\\\""  { goto yyc_DoubleQuoteString; }
    <DoubleQuoteString> '"'     { PUSH_TOKEN(Token::STRING);}
    <DoubleQuoteString> "\\" "\n" "\r"? { goto yyc_DoubleQuoteString; }
    <DoubleQuoteString> "\\" "\r" "\n"? { goto yyc_DoubleQuoteString; }
    <DoubleQuoteString> "\n"    => Normal { PUSH_TOKEN_LOOKAHEAD(Token::ILLEGAL); }
    <DoubleQuoteString> "\r"    => Normal { PUSH_TOKEN_LOOKAHEAD(Token::ILLEGAL); }
    <DoubleQuoteString> eof     { TERMINATE_ILLEGAL(); }
    <DoubleQuoteString> any     { goto yyc_DoubleQuoteString; }

    <SingleQuoteString> "\\\\"  { goto yyc_SingleQuoteString; }
    <SingleQuoteString> "\\'"   { goto yyc_SingleQuoteString; }
    <SingleQuoteString> "'"     { PUSH_TOKEN(Token::STRING);}
    <SingleQuoteString> "\\" "\n" "\r"? { goto yyc_SingleQuoteString; }
    <SingleQuoteString> "\\" "\r" "\n"? { goto yyc_SingleQuoteString; }
    <SingleQuoteString> "\n"    => Normal { PUSH_TOKEN_LOOKAHEAD(Token::ILLEGAL); }
    <SingleQuoteString> "\r"    => Normal { PUSH_TOKEN_LOOKAHEAD(Token::ILLEGAL); }
    <SingleQuoteString> eof     { TERMINATE_ILLEGAL(); }
    <SingleQuoteString> any     { goto yyc_SingleQuoteString; }

    <Identifier> identifier_char+  { goto yyc_Identifier; }
    <Identifier> "\\u" [0-9a-fA-F]{4} { if (ValidIdentifierPart()) { goto yyc_Identifier; } YYSETCONDITION(kConditionNormal); send(Token::ILLEGAL); start_ = cursor_; goto yyc_Normal; }
    <Identifier> "\\"              { PUSH_TOKEN(Token::ILLEGAL); }
    <Identifier> any               { PUSH_TOKEN_LOOKAHEAD(Token::IDENTIFIER); }

    <SingleLineComment> line_terminator { PUSH_LINE_TERMINATOR();}
    <SingleLineComment> eof             { start_ = cursor_ - 1; PUSH_TOKEN(Token::EOS); }
    <SingleLineComment> any             { goto yyc_SingleLineComment; }

    <MultiLineComment> [*][//]  { PUSH_LINE_TERMINATOR();}
    <MultiLineComment> eof      { start_ = cursor_ - 1; PUSH_TOKEN(Token::EOS); }
    <MultiLineComment> any      { goto yyc_MultiLineComment; }

    <HtmlComment> eof        { start_ = cursor_ - 1; PUSH_TOKEN(Token::EOS); }
    <HtmlComment> "-->"      { PUSH_LINE_TERMINATOR();}
    <HtmlComment> line_terminator+ { PUSH_LINE_TERMINATOR();}
    <HtmlComment> any        { goto yyc_HtmlComment; }
    */

fill:
  int unfinished_size = cursor_ - start_;
  if (FLAG_trace_lexer) {
    printf(
        "scanner needs a refill. Exiting for now with:\n"
        "  saved fill state_ = %d\n"
        "  unfinished token size = %d\n",
        state_,
        unfinished_size);
    if (0 < unfinished_size && start_ < limit_) {
      printf("  unfinished token is: ");
      fwrite(start_, 1, cursor_ - start_, stdout);
      putchar('\n');
    }
    putchar('\n');
  }

  if (eof_) goto start_;

  //  Once we get here, we can get rid of
  //  everything before start_ and after limit_.

  if (buffer_ < start_) {
    size_t start_offset = start_ - buffer_;
    memmove(buffer_, start_, limit_ - start_);
    marker_ -= start_offset;
    cursor_ -= start_offset;
    limit_ -= start_offset;
    start_ -= start_offset;
    real_start_ += start_offset;
  }
  return 0;
}
