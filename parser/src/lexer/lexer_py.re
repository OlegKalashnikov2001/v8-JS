# Copyright 2013 the V8 project authors. All rights reserved.
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met:
#
#     * Redistributions of source code must retain the above copyright
#       notice, this list of conditions and the following disclaimer.
#     * Redistributions in binary form must reproduce the above
#       copyright notice, this list of conditions and the following
#       disclaimer in the documentation and/or other materials provided
#       with the distribution.
#     * Neither the name of Google Inc. nor the names of its
#       contributors may be used to endorse or promote products derived
#       from this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

# Character classes and auxiliary regexps.

line_terminator = [:line_terminator:];  # TODO(dcarney): just include these
identifier_start = [$_:letter:];
identifier_char = [:identifier_start::identifier_part_not_letter:];
digit = [0-9];
hex_digit = [0-9a-fA-F];
unicode_escape = /\\u[:hex_digit:]{4}/;
single_escape_char = ['"\\bfnrtv];
maybe_exponent = /([eE][\-+]?[:digit:]+)?/;
octal_number = /0[0-7]+/;

# Octal numbers are pretty complicated. For example, 01.0 is invalid, since it's
# parsed as 2 numbers (01 and .0), since 01 is a valid octal number. However,
# 09.0 is a valid, since 09 cannot be octal. In addition, 0 and 0.1 are valid
# numbers starting with 0.

non_octal_whole_part = "0" | (
  /0[:digit:]*[8-9][:digit:]*/ |
  /[1-9][:digit:]*/ );

number =
  /0[xX][:hex_digit:]+/ | (
  /\.[:digit:]+/ maybe_exponent |
  non_octal_whole_part /(\.[:digit:]*)?/ maybe_exponent );
harmony_number = "0"[bBoO][:digit:]+;
line_terminator_sequence = /[:line_terminator:]|(\r\n|\n\r)/;

# Rules.

# Grammar is
#   regex <entry_action|match_action|transition>
#
# Actions are identifiers to be passed to codegen.
#
# Entry action is executed when we enter the corresponding automaton state, that
# is, right after seeing something that matches the regex. Match action is
# executed when we have matched the regex but cannot continue to match something
# bigger (there is no legal transition out with the next character we're
# lexing).
#
# Transition must be 'continue' or the name of a subgraph.

<<default>>

"|="          <|token(ASSIGN_BIT_OR)|>
"^="          <|token(ASSIGN_BIT_XOR)|>
"&="          <|token(ASSIGN_BIT_AND)|>
"+="          <|token(ASSIGN_ADD)|>
"-="          <|token(ASSIGN_SUB)|>
"*="          <|token(ASSIGN_MUL)|>
"/="          <|token(ASSIGN_DIV)|>
"%="          <|token(ASSIGN_MOD)|>

"==="         <|token(EQ_STRICT)|>
"=="          <|token(EQ)|>
"="           <|token(ASSIGN)|>
"!=="         <|token(NE_STRICT)|>
"!="          <|token(NE)|>
"!"           <|token(NOT)|>

"//"          <||SingleLineComment>
"/*"          <set_marker(2)||MultiLineComment>
"<!--"        <||SingleLineComment>

"-->"  <if_line_terminator_backtrack(1, DEC)||SingleLineComment>

">>>="        <|token(ASSIGN_SHR)|>
">>>"         <|token(SHR)|>
"<<="         <|token(ASSIGN_SHL)|>
">>="         <|token(ASSIGN_SAR)|>
"<="          <|token(LTE)|>
">="          <|token(GTE)|>
"<<"          <|token(SHL)|>
">>"          <|token(SAR)|>
"<"           <|token(LT)|>
">"           <|token(GT)|>

octal_number            <|octal_number|>
number                  <|token(NUMBER)|>
number identifier_char  <|token(ILLEGAL)|>
number "\\"             <|token(ILLEGAL)|>

harmony_number                  <|harmony_token(numeric_literals, NUMBER, ILLEGAL)|>
harmony_number identifier_char  <|token(ILLEGAL)|>
harmony_number "\\"             <|token(ILLEGAL)|>

"("           <|token(LPAREN)|>
")"           <|token(RPAREN)|>
"["           <|token(LBRACK)|>
"]"           <|token(RBRACK)|>
"{"           <|token(LBRACE)|>
"}"           <|token(RBRACE)|>
":"           <|token(COLON)|>
";"           <|token(SEMICOLON)|>
"."           <|token(PERIOD)|>
"?"           <|token(CONDITIONAL)|>
"++"          <|token(INC)|>
"--"          <|token(DEC)|>

"||"          <|token(OR)|>
"&&"          <|token(AND)|>

"|"           <|token(BIT_OR)|>
"^"           <|token(BIT_XOR)|>
"&"           <|token(BIT_AND)|>
"+"           <|token(ADD)|>
"-"           <|token(SUB)|>
"*"           <|token(MUL)|>
"/"           <|token(DIV)|>
"%"           <|token(MOD)|>
"~"           <|token(BIT_NOT)|>
","           <|token(COMMA)|>

line_terminator+   <|line_terminator|>
/[:whitespace:]+/  <|skip|>

"\""           <set_marker(1)|token(ILLEGAL)|DoubleQuoteString>
"'"            <set_marker(1)|token(ILLEGAL)|SingleQuoteString>

# all keywords
"break"       <|token(BREAK)|>
"case"        <|token(CASE)|>
"catch"       <|token(CATCH)|>
"class"       <|token(FUTURE_RESERVED_WORD)|>
"const"       <|token(CONST)|>
"continue"    <|token(CONTINUE)|>
"debugger"    <|token(DEBUGGER)|>
"default"     <|token(DEFAULT)|>
"delete"      <|token(DELETE)|>
"do"          <|token(DO)|>
"else"        <|token(ELSE)|>
"enum"        <|token(FUTURE_RESERVED_WORD)|>
"export"      <|harmony_token(modules, EXPORT, FUTURE_RESERVED_WORD)|>
"extends"     <|token(FUTURE_RESERVED_WORD)|>
"false"       <|token(FALSE_LITERAL)|>
"finally"     <|token(FINALLY)|>
"for"         <|token(FOR)|>
"function"    <|token(FUNCTION)|>
"if"          <|token(IF)|>
"implements"  <|token(FUTURE_STRICT_RESERVED_WORD)|>
"import"      <|harmony_token(modules, IMPORT, FUTURE_RESERVED_WORD)|>
"in"          <|token(IN)|>
"instanceof"  <|token(INSTANCEOF)|>
"interface"   <|token(FUTURE_STRICT_RESERVED_WORD)|>
"let"         <|harmony_token(scoping, LET, FUTURE_STRICT_RESERVED_WORD)|>
"new"         <|token(NEW)|>
"null"        <|token(NULL_LITERAL)|>
"package"     <|token(FUTURE_STRICT_RESERVED_WORD)|>
"private"     <|token(FUTURE_STRICT_RESERVED_WORD)|>
"protected"   <|token(FUTURE_STRICT_RESERVED_WORD)|>
"public"      <|token(FUTURE_STRICT_RESERVED_WORD)|>
"return"      <|token(RETURN)|>
"static"      <|token(FUTURE_STRICT_RESERVED_WORD)|>
"super"       <|token(FUTURE_RESERVED_WORD)|>
"switch"      <|token(SWITCH)|>
"this"        <|token(THIS)|>
"throw"       <|token(THROW)|>
"true"        <|token(TRUE_LITERAL)|>
"try"         <|token(TRY)|>
"typeof"      <|token(TYPEOF)|>
"var"         <|token(VAR)|>
"void"        <|token(VOID)|>
"while"       <|token(WHILE)|>
"with"        <|token(WITH)|>
"yield"       <|token(YIELD)|>

identifier_start  <|token(IDENTIFIER)|Identifier>
unicode_escape    <check_escaped_identifier_start|token(IDENTIFIER)|Identifier>
"\\"              <|token(ILLEGAL)|>  # ambiguous backtracking otherwise

eos             <terminate>
default_action  <default>

<<DoubleQuoteString>>
epsilon                       <StringSubgraph>
"\""                          <|token(STRING)|>
catch_all                     <||continue>
eos                           <terminate_illegal>

<<SingleQuoteString>>
epsilon                       <StringSubgraph>
"'"                           <|token(STRING)|>
catch_all                     <||continue>
eos                           <terminate_illegal>

<<StringSubgraph>>
"\\" line_terminator_sequence <set_has_escapes||continue(1)>
/\\[x][:hex_digit:]{2}/       <set_has_escapes||continue(1)>
unicode_escape                <set_has_escapes||continue(1)>
/\\[1-7]/                     <octal_inside_string||continue(1)>
/\\[0-7][0-7]+/               <octal_inside_string||continue(1)>
"\\0"                         <set_has_escapes||continue(1)>
/\\[^xu0-7:line_terminator:]/ <set_has_escapes||continue(1)>
"\\"                          <|token(ILLEGAL)|>
line_terminator               <|token(ILLEGAL)|>

<<Identifier>>
identifier_char  <|token(IDENTIFIER)|continue>
unicode_escape   <check_escaped_identifier_part|token(IDENTIFIER)|continue>
"\\"             <|token(ILLEGAL)|>  # ambiguous backtracking otherwise

<<SingleLineComment>>
line_terminator  <|line_terminator|>
catch_all        <||continue>
eos              <skip_and_terminate>

<<MultiLineComment>>
/\*+\//          <|skip|>
/\*+[^\/*]/      <||continue>
line_terminator  <line_terminator_in_multiline_comment||continue>
catch_all        <||continue>
eos              <terminate_illegal>
