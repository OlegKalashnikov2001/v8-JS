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

import ply.yacc as yacc
from types import ListType, TupleType
from regex_lexer import RegexLexer
from action import Term

class RegexParser:

  tokens = RegexLexer.tokens

  token_map = {
    '+': 'ONE_OR_MORE',
    '?': 'ZERO_OR_ONE',
    '*': 'ZERO_OR_MORE',
    '|': 'OR',
    '.': 'ANY',
  }

  def p_start(self, p):
    '''start : fragments OR fragments
             | fragments'''
    if len(p) == 2:
      p[0] = p[1]
    else:
      p[0] = Term(self.token_map[p[2]], p[1], p[3])

  def p_fragments(self, p):
    '''fragments : fragment
                 | fragment fragments'''
    if len(p) == 2:
      p[0] = p[1]
    else:
      p[0] = self.__cat(p[1], p[2])

  def p_fragment(self, p):
    '''fragment : literal maybe_modifier
                | class maybe_modifier
                | group maybe_modifier
                | any maybe_modifier
    '''
    if p[2] != None:
      if isinstance(p[2], tuple) and p[2][0] == 'REPEAT':
        p[0] = Term(p[2][0], p[2][1], p[2][2], p[1])
      else:
        p[0] = Term(p[2], p[1])
    else:
      p[0] = p[1]

  def p_maybe_modifier(self, p):
    '''maybe_modifier : ONE_OR_MORE
                      | ZERO_OR_ONE
                      | ZERO_OR_MORE
                      | repetition
                      | empty'''
    p[0] = p[1]
    if p[1] in self.token_map:
      p[0] = self.token_map[p[1]]

  def p_repetition(self, p):
    '''repetition : REPEAT_BEGIN NUMBER REPEAT_END
                  | REPEAT_BEGIN NUMBER COMMA NUMBER REPEAT_END'''
    if len(p) == 4:
      p[0] = ("REPEAT", p[2], p[2])
    else:
      p[0] = ("REPEAT", p[2], p[4])

  def p_literal(self, p):
    '''literal : LITERAL'''
    p[0] = Term('LITERAL', p[1])

  def p_any(self, p):
    '''any : ANY'''
    p[0] = Term(self.token_map[p[1]])

  def p_class(self, p):
    '''class : CLASS_BEGIN class_content CLASS_END
             | CLASS_BEGIN NOT class_content CLASS_END'''
    if len(p) == 4:
      p[0] = Term("CLASS", p[2])
    else:
      p[0] = Term("NOT_CLASS", p[3])

  def p_group(self, p):
    '''group : GROUP_BEGIN start GROUP_END'''
    p[0] = p[2]

  def p_class_content(self, p):
    '''class_content : CLASS_LITERAL RANGE CLASS_LITERAL maybe_class_content
                     | CLASS_LITERAL maybe_class_content
                     | CHARACTER_CLASS maybe_class_content
                     | CLASS_LITERAL_AS_OCTAL maybe_class_content
    '''
    if len(p) == 5:
      left = Term("RANGE", p[1], p[3])
    else:
      if len(p[1]) == 1:
        left = Term('LITERAL', p[1])
      elif p[1][0] == '\\':
        left = Term('LITERAL', chr(int(p[1][1:], 8)))
      else:
        left = Term('CHARACTER_CLASS', p[1][1:-1])
    p[0] = self.__cat(left, p[len(p)-1])

  def p_maybe_class_content(self, p):
    '''maybe_class_content : class_content
                           | empty'''
    p[0] = p[1]

  def p_empty(self, p):
    'empty :'

  def p_error(self, p):
    raise Exception("Syntax error in input '%s'" % str(p))

  @staticmethod
  def __cat(left, right):
    assert left
    return left if not right else Term('CAT', left, right)

  def build(self, **kwargs):
    self.parser = yacc.yacc(module=self, debug=0, write_tables=0, **kwargs)
    self.lexer = RegexLexer()
    self.lexer.build(**kwargs)

  __static_instance = None
  @staticmethod
  def parse(data):
    parser = RegexParser.__static_instance
    if not parser:
      parser = RegexParser()
      parser.build()
      RegexParser.__static_instance = parser
    try:
      return parser.parser.parse(data, lexer=parser.lexer.lexer)
    except Exception:
      RegexParser.__static_instance = None
      raise
