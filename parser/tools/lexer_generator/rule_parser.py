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
from action import Term, Action
from rule_lexer import RuleLexer
from regex_parser import RegexParser
from nfa_builder import NfaBuilder
from dfa import Dfa
from dfa_optimizer import DfaOptimizer
from transition_keys import TransitionKey, KeyEncoding

class RuleParserState:

  def __init__(self, encoding):
    self.aliases = {}
    self.character_classes = {}
    self.current_state = None
    self.rules = {}
    self.transitions = set()
    self.encoding = encoding

  def parse(self, string):
    return RuleParser.parse(string, self)

class RuleParser:

  tokens = RuleLexer.tokens
  __rule_precedence_counter = 0
  __keyword_transitions = set(['continue'])

  def __init__(self):
    self.__state = None

  def p_statements(self, p):
    'statements : aliases rules'

  def p_aliases(self, p):
    '''aliases : alias_rule aliases
               | empty'''

  def p_alias_rule(self, p):
    'alias_rule : IDENTIFIER EQUALS composite_regex SEMICOLON'
    state = self.__state
    assert not p[1] in state.aliases
    term = p[3]
    state.aliases[p[1]] = term
    if term.name() == 'CLASS' or term.name() == 'NOT_CLASS':
      classes = state.character_classes
      assert not p[1] in classes, "cannot reassign alias"
      encoding = state.encoding
      classes[p[1]] = TransitionKey.character_class(encoding, term, classes)

  def p_rules(self, p):
    '''rules : state_change transition_rules rules
             | empty'''

  def p_state_change(self, p):
    'state_change : GRAPH_OPEN IDENTIFIER GRAPH_CLOSE'
    state = self.__state
    state.current_state = p[2]
    assert state.current_state
    if not state.current_state in state.rules:
      state.rules[state.current_state] = {
        'default_action': Term.empty_term(),
        'uniques' : {},
        'regex' : []
      }
    p[0] = state.current_state

  def p_transition_rules(self, p):
    '''transition_rules : transition_rule transition_rules
                        | empty'''

  def p_transition_rule(self, p):
    '''transition_rule : composite_regex action
                       | DEFAULT_ACTION default_action
                       | EOS eos
                       | CATCH_ALL action'''
    precedence = RuleParser.__rule_precedence_counter
    RuleParser.__rule_precedence_counter += 1
    action = p[2]
    (entry_action, match_action, transition) = action
    if transition and not transition in self.__keyword_transitions:
      assert not transition == 'default', "can't append default graph"
      self.__state.transitions.add(transition)
    rules = self.__state.rules[self.__state.current_state]
    if p[1] == 'default_action':
      assert self.__state.current_state == 'default'
      assert not rules['default_action']
      assert not entry_action
      rules['default_action'] = match_action
    elif p[1] == 'eos' or p[1] == 'catch_all':
      assert p[1] not in rules['uniques']
      rules['uniques'][p[1]] = True
      rules['regex'].append((NfaBuilder.unique_key(p[1]), precedence, action))
    else:
      regex = p[1]
      rules['regex'].append((regex, precedence, action))

  def p_action(self, p):
    '''action : ACTION_OPEN maybe_identifier_action OR maybe_identifier_action OR maybe_transition ACTION_CLOSE'''
    p[0] = (p[2], p[4], p[6])

  def p_default_action(self, p):
    'default_action : ACTION_OPEN identifier_action ACTION_CLOSE'
    p[0] = (Term.empty_term(), p[2], None)

  def p_eos(self, p):
    'eos : ACTION_OPEN identifier_action ACTION_CLOSE'
    p[0] = (Term.empty_term(), p[2], None)

  def p_maybe_identifier_action(self, p):
    '''maybe_identifier_action : identifier_action
                         | empty'''
    p[0] = p[1] if p[1] else Term.empty_term()

  def p_maybe_transition(self, p):
    '''maybe_transition : IDENTIFIER
                        | empty'''
    p[0] = p[1]

  def p_identifier_action(self, p):
    '''identifier_action : IDENTIFIER
                         | IDENTIFIER LEFT_PARENTHESIS RIGHT_PARENTHESIS
                         | IDENTIFIER LEFT_PARENTHESIS action_params RIGHT_PARENTHESIS'''
    if len(p) == 2 or len(p) == 4:
      p[0] = Term(p[1])
    elif len(p) == 5:
        p[0] = Term(p[1], *p[3])
    else:
      raise Exception()

  def p_action_params(self, p):
    '''action_params : IDENTIFIER
                     | IDENTIFIER COMMA action_params'''
    if len(p) == 2:
      p[0] = (p[1],)
    elif len(p) == 4:
      p[0] = tuple(([p[1]] + list(p[3])))
    else:
      raise Exception()

  def p_composite_regex(self, p):
    '''composite_regex : regex_parts OR regex_parts
                       | regex_parts'''
    if len(p) == 2:
      p[0] = p[1]
    else:
      p[0] = NfaBuilder.or_terms([p[1], p[3]])

  def p_regex_parts(self, p):
    '''regex_parts : regex_part
                   | regex_part regex_parts'''
    p[0] = NfaBuilder.cat_terms(p[1:])

  def p_regex_part(self, p):
    '''regex_part : LEFT_PARENTHESIS composite_regex RIGHT_PARENTHESIS modifier
                  | regex_string_literal modifier
                  | regex_class modifier
                  | regex modifier
                  | regex_alias modifier'''
    modifier = p[len(p)-1]
    term = p[2] if len(p) == 5 else p[1]
    if modifier:
      p[0] = NfaBuilder.apply_modifier(modifier, term)
    else:
      p[0] = term

  def p_regex_string_literal(self, p):
    'regex_string_literal : STRING'
    string = p[1][1:-1]
    escape_char = lambda string, char: string.replace(char, "\\" + char)
    string = reduce(escape_char, "+?*|.[](){}", string).replace("\\\"", "\"")
    p[0] = RegexParser.parse(string)
  def p_regex(self, p):
    'regex : REGEX'
    string = p[1][1:-1].replace("\\/", "/")
    p[0] = RegexParser.parse(string)

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

  def p_empty(self, p):
    'empty :'

  def p_error(self, p):
    raise Exception("Syntax error in input '%s'" % str(p))

  def build(self, **kwargs):
    self.parser = yacc.yacc(module=self, debug=0, write_tables=0, **kwargs)
    self.lexer = RuleLexer()
    self.lexer.build(**kwargs)

  __static_instance = None
  @staticmethod
  def parse(data, parser_state):
    parser = RuleParser.__static_instance
    if not parser:
      parser = RuleParser()
      parser.build()
      RuleParser.__static_instance = parser
    parser.__state = parser_state
    try:
      parser.parser.parse(data, lexer=parser.lexer.lexer)
    except Exception:
      RuleParser.__static_instance = None
      raise
    parser.__state = None
    assert parser_state.transitions <= set(parser_state.rules.keys())

class RuleProcessor(object):

  def __init__(self, parser_state):
    self.__automata = {}
    self.__rule_trees = {}
    self.__default_action = None
    self.__process_parser_state(parser_state)

  @staticmethod
  def parse(string, encoding_name):
    parser_state = RuleParserState(KeyEncoding.get(encoding_name))
    RuleParser.parse(string, parser_state)
    return RuleProcessor(parser_state)

  def automata_iter(self):
    return iter(self.__automata.items())

  def default_automata(self):
    return self.__automata['default']

  def default_action(self):
    return self.__default_action

  class Automata(object):

    def __init__(self, encoding, character_classes, rule_term):
      self.__encoding = encoding
      self.__character_classes = character_classes
      self.__rule_term = rule_term
      self.__nfa = None
      self.__dfa = None
      self.__minimial_dfa = None

    def rule_term(self):
      return self.__rule_term

    def nfa(self):
      if not self.__nfa:
        self.__nfa = NfaBuilder.nfa(
          self.__encoding, self.__character_classes, self.__rule_term)
      return self.__nfa

    def dfa(self):
      if not self.__dfa:
        (start, dfa_nodes) = self.nfa().compute_dfa()
        self.__dfa = Dfa(self.nfa().encoding(), start, dfa_nodes)
      return self.__dfa

    def optimize_dfa(self, log = False):
      assert not self.__dfa
      self.__dfa = DfaOptimizer.optimize(self.dfa(), log)

    def minimal_dfa(self):
      if not self.__minimial_dfa:
        self.__minimial_dfa = self.dfa().minimize()
      return self.__minimial_dfa

  def __process_parser_state(self, parser_state):
    rule_map = {}
    assert 'default' in parser_state.rules
    def process(subgraph, v):
      graphs = []
      for graph, precedence, action in v['regex']:
        (entry_action, match_action, transition) = action
        if entry_action or match_action:
          graph = NfaBuilder.add_action(
            graph, Action(entry_action, match_action, precedence))
        if not transition:
          pass
        elif transition == 'continue':
          assert not subgraph == 'default', 'unimplemented'
          graph = NfaBuilder.add_continue(graph)
        else:
          assert subgraph == 'default', 'unimplemented'
          graph = NfaBuilder.join_subgraph(
            graph, transition, rule_map[transition])
        graphs.append(graph)
      graph = NfaBuilder.or_terms(graphs)
      rule_map[subgraph] = graph
    # process first the subgraphs, then the default graph
    for k, v in parser_state.rules.items():
      if k == 'default': continue
      process(k, v)
    process('default', parser_state.rules['default'])
    # build the automata
    for rule_name, graph in rule_map.items():
      self.__automata[rule_name] = RuleProcessor.Automata(
        parser_state.encoding, parser_state.character_classes, graph)
      self.__rule_trees[rule_name] = graph
    # process default_action
    default_action = parser_state.rules['default']['default_action']
    self.__default_action = Action(Term.empty_term(), default_action)
