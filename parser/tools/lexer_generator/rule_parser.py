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
from rule_lexer import RuleLexer
from regex_parser import RegexParser
from nfa import NfaBuilder
from transition_keys import TransitionKey

class RuleParserState:

  def __init__(self):
    self.aliases = {
      'eof' : RegexParser.parse("[\\0]"),
      'any' : RegexParser.parse("."),
    }
    self.character_classes = {}
    self.current_transition = None
    self.rules = {}

  def parse(self, string):
    return RuleParser.parse(string, self)

class RuleParser:

  tokens = RuleLexer.tokens

  def __init__(self):
    self.__state = None

  def p_statements(self, p):
    'statements : statement maybe_statements'

  def p_maybe_statement(self, p):
    '''maybe_statements : statements
                        | empty'''

  def p_statement(self, p):
    '''statement : alias_rule
                 | transition_rule'''

  def p_alias_rule(self, p):
    'alias_rule : IDENTIFIER EQUALS composite_regex SEMICOLON'
    state = self.__state
    assert not p[1] in state.aliases
    graph = p[3]
    state.aliases[p[1]] = graph
    if graph[0] == 'CLASS' or graph[0] == 'NOT_CLASS':
      classes = state.character_classes
      assert not p[1] in classes
      classes[p[1]] = TransitionKey.character_class(graph, classes)

  def p_transition_rule(self, p):
    '''transition_rule : transition composite_regex code
         | transition composite_regex TRANSITION IDENTIFIER
         | transition composite_regex TRANSITION_WITH_CODE IDENTIFIER code'''
    transition = p[0]
    regex = p[2]
    rules = self.__state.rules[self.__state.current_transition]
    if len(p) == 4:
      rules.append(('simple', regex, None, p[3]))
    elif len(p) == 5:
      rules.append(('transition', regex, p[4], None))
    elif len(p) == 6:
      rules.append(('transition_with_code', regex, p[4], p[5]))
    else:
      raise Exception()

  def p_transition(self, p):
    '''transition : LESS_THAN IDENTIFIER GREATER_THAN'''
                  # | empty''' TODO skipping transition without sr conflict
    state = self.__state
    if p[1]:
      state.current_transition = p[2]
    assert state.current_transition
    if not state.current_transition in state.rules:
      state.rules[state.current_transition] = []
    p[0] = state.current_transition

  def p_composite_regex(self, p):
    '''composite_regex : regex_parts OR regex_parts
                       | regex_parts'''
    if len(p) == 2:
      p[0] = p[1]
    else:
      p[0] = NfaBuilder.or_graphs([p[1], p[3]])

  def p_regex_parts(self, p):
    '''regex_parts : regex_part
                   | regex_part regex_parts'''
    p[0] = NfaBuilder.cat_graphs(p[1:])

  def p_regex_part(self, p):
    '''regex_part : LEFT_PARENTHESIS composite_regex RIGHT_PARENTHESIS modifier
                  | regex_string_literal modifier
                  | regex_class modifier
                  | regex modifier
                  | regex_alias modifier'''
    modifier = p[len(p)-1]
    graph = p[2] if len(p) == 5 else p[1]
    if modifier:
      p[0] = NfaBuilder.apply_modifier(modifier, graph)
    else:
      p[0] = graph

  def p_regex_string_literal(self, p):
    'regex_string_literal : STRING'
    escape_char = lambda string, char: string.replace(char, "\\" + char)
    string = reduce(escape_char, "\+?*|.[](){}", p[1][1:-1])
    p[0] = RegexParser.parse(string)

  def p_regex(self, p):
    'regex : REGEX'
    p[0] = RegexParser.parse(p[1][1:-1])

  def p_regex_class(self, p):
    'regex_class : CHARACTER_CLASS_REGEX'
    p[0] = RegexParser.parse(p[1])

  def p_regex_alias(self, p):
    'regex_alias : IDENTIFIER'
    p[0] = self.__state.aliases[p[1]]

  def p_modifier(self, p):
    '''modifier : PLUS
                | QUESTION_MARK
                | STAR
                | empty'''
    p[0] = p[1]

  def p_code(self, p):
    'code : LEFT_BRACKET code_fragments RIGHT_BRACKET'
    p[0] = p[2].strip()

  def p_code_fragments(self, p):
    '''code_fragments : CODE_FRAGMENT code_fragments
                      | empty'''
    p[0] = p[1]
    if len(p) == 3 and p[2]:
      p[0] = p[1] + p[2]

  def p_empty(self, p):
    'empty :'

  def p_error(self, p):
    raise Exception("Syntax error in input '%s'" % p)

  def build(self, **kwargs):
    self.parser = yacc.yacc(module=self, debug=0, write_tables=0, **kwargs)
    self.lexer = RuleLexer()
    self.lexer.build(**kwargs)

  __static_instance = None
  @staticmethod
  def parse(data, parser_state):
    if not RuleParser.__static_instance:
      RuleParser.__static_instance = RuleParser()
      RuleParser.__static_instance.build()
    parser = RuleParser.__static_instance
    parser.__state = parser_state
    parser.parser.parse(data, lexer=parser.lexer.lexer)
    parser.__state = None
    rule_map = {}
    builder = NfaBuilder()
    builder.set_character_classes(parser_state.character_classes)
    for k, v in parser_state.rules.items():
      graphs = []
      for (rule_type, graph, identifier, action) in v:
        graphs.append(graph)
      rule_map[k] = builder.nfa(NfaBuilder.or_graphs(graphs))
    return rule_map
