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

from types import TupleType
from inspect import getmembers
from action import *
from nfa import *

# Nfa is built in two stages:
# 1. Build a tree of operations on rules.
#    Each node in the tree is a tuple (operation, subtree1, ... subtreeN).
#    Rule parser builds this tree by invoking static methods of NfaBuilder.
# 2. For each node, perform the operation of the node to produce an Nfa.
#    If an operation is called X, then it is performed by the method
#    of NfaBuilder called __x(). See __process() for mapping from
#    operation to processing methods.

class NfaBuilder(object):

  def __init__(self, encoding, character_classes):
    self.__node_number = 0
    self.__operation_map = {}
    self.__members = getmembers(self)
    self.__encoding = encoding
    self.__character_classes = character_classes
    self.__states = []
    self.__global_end_node = None

  def __new_state(self):
    self.__node_number += 1
    return NfaState()

  def __global_end(self):
    if not self.__global_end_node:
      self.__global_end_node = self.__new_state()
    return self.__global_end_node

  def __or(self, args):
    start = self.__new_state()
    ends = []
    for x in [self.__process(args[0]), self.__process(args[1])]:
      start.add_epsilon_transition(x[0])
      ends += x[1]
    start.close(None)
    return (start, ends)

  def __one_or_more(self, args):
    (start, ends) = self.__process(args[0])
    end =  self.__new_state()
    end.add_epsilon_transition(start)
    self.__patch_ends(ends, end)
    return (start, [end])

  def __zero_or_more(self, args):
    (node, ends) = self.__process(args[0])
    start =  self.__new_state()
    start.add_epsilon_transition(node)
    self.__patch_ends(ends, start)
    return (start, [start])

  def __zero_or_one(self, args):
    (node, ends) = self.__process(args[0])
    start =  self.__new_state()
    start.add_epsilon_transition(node)
    return (start, ends + [start])

  def __repeat(self, args):
    param_min = int(args[0])
    param_max = int(args[1])
    subgraph = args[2]
    (start, ends) = self.__process(subgraph)
    for i in xrange(1, param_min):
      (start2, ends2) = self.__process(subgraph)
      self.__patch_ends(ends, start2)
      ends = ends2
    if param_min == param_max:
      return (start, ends)

    midpoints = []
    for i in xrange(param_min, param_max):
      midpoint =  self.__new_state()
      self.__patch_ends(ends, midpoint)
      (start2, ends) = self.__process(subgraph)
      midpoint.add_epsilon_transition(start2)
      midpoints.append(midpoint)

    return (start, ends + midpoints)

  def __cat(self, args):
    (left, right) = (self.__process(args[0]), self.__process(args[1]))
    self.__patch_ends(left[1], right[0])
    return (left[0], right[1])

  def __key_state(self, key):
    state =  self.__new_state()
    state.add_unclosed_transition(key)
    return (state, [state])

  def __literal(self, args):
    assert len(args) == 1
    return self.__key_state(
      TransitionKey.single_char(self.__encoding, args[0]))

  def __class(self, args):
    assert len(args) == 1
    return self.__key_state(TransitionKey.character_class(
      self.__encoding, Term('CLASS', args[0]), self.__character_classes))

  def __not_class(self, args):
    assert len(args) == 1
    return self.__key_state(TransitionKey.character_class(
      self.__encoding, Term('NOT_CLASS', args[0]), self.__character_classes))

  def __any(self, args):
    return self.__key_state(TransitionKey.any(self.__encoding))

  def __action(self, args):
    (start, ends) = self.__process(args[0])
    action = Action.from_term(args[1])
    end = self.__new_state()
    self.__patch_ends(ends, end)
    end.set_action(action)
    if action.match_action():
      global_end = self.__global_end()
      end.add_epsilon_transition(global_end)
    return (start, [end])

  def __continue(self, args):
    (start, ends) = self.__process(args[0])
    state = self.__peek_state()
    if not state['start_node']:
      state['start_node'] = self.__new_state()
    self.__patch_ends(ends, state['start_node'])
    return (start, [])

  def __unique_key(self, args):
    return self.__key_state(TransitionKey.unique(args[0]))

  def __join(self, (graph, name, subgraph)):
    subgraphs = self.__peek_state()['subgraphs']
    if not name in subgraphs:
      subgraphs[name] = self.__nfa(subgraph)
    (subgraph_start, subgraph_end, nodes_in_subgraph) = subgraphs[name]
    (start, ends) = self.__process(graph)
    self.__patch_ends(ends, subgraph_start)
    if subgraph_end:
      end = self.__new_state()
      subgraph_end.add_epsilon_transition(end)
      return (start, [end])
    else:
      return (start, [])

  def __process(self, term):
    assert isinstance(term, Term)
    method = "_NfaBuilder__" + term.name().lower()
    if not method in self.__operation_map:
      matches = filter(lambda (name, func): name == method, self.__members)
      assert len(matches) == 1
      self.__operation_map[method] = matches[0][1]
    return self.__operation_map[method](term.args())

  def __patch_ends(self, ends, new_end):
    for end in ends:
      end.close(new_end)

  def __push_state(self):
    self.__states.append({
      'start_node' : None,
      'subgraphs' : {},
      'unpatched_ends' : [],
    })

  def __pop_state(self):
    return self.__states.pop()

  def __peek_state(self):
    return self.__states[-1]

  def __nfa(self, term):
    start_node_number = self.__node_number
    self.__push_state()
    (start, ends) = self.__process(term)
    state = self.__pop_state()
    if state['start_node']:
      state['start_node'].close(start)
      start = state['start_node']
    for k, subgraph in state['subgraphs'].items():
      if subgraph[1]:
        subgraph[1].close(None)

    # Don't create an end node for the subgraph if it would be unused (ends can
    # be an empty list e.g., in the case when everything inside the subgraph is
    # "continue").
    end = None
    if ends:
      end = self.__new_state()
      self.__patch_ends(ends, end)

    return (start, end, self.__node_number - start_node_number)

  @staticmethod
  def __compute_epsilon_closures(start_state):
    def outer(node, state):
      def inner(node, closure):
        closure.add(node)
        return closure
      is_epsilon = lambda k: k == TransitionKey.epsilon()
      state_iter = lambda node : node.state_iter(key_filter = is_epsilon)
      edge = set(state_iter(node))
      closure = Automaton.visit_states(
          edge, inner, state_iter=state_iter, visit_state=set())
      node.set_epsilon_closure(closure)
    Automaton.visit_states(set([start_state]), outer)

  @staticmethod
  def __replace_catch_all(encoding, state):
    catch_all = TransitionKey.unique('catch_all')
    transitions = state.transitions()
    if not catch_all in transitions:
      return
    f = lambda acc, state: acc | set(state.epsilon_closure_iter())
    reachable_states = reduce(f, transitions[catch_all], set())
    f = lambda acc, state: acc | set(state.transitions().keys())
    keys = reduce(f, reachable_states, set())
    keys.discard(TransitionKey.epsilon())
    keys.discard(catch_all)
    keys.discard(TransitionKey.unique('eos'))
    inverse_key = TransitionKey.inverse_key(encoding, keys)
    if inverse_key:
      transitions[inverse_key] = transitions[catch_all]
    del transitions[catch_all]

  @staticmethod
  def nfa(encoding, character_classes, rule_term):

    self = NfaBuilder(encoding, character_classes)
    (start, end, nodes_created) = self.__nfa(rule_term)

    global_end_to_return = self.__global_end_node or end
    assert global_end_to_return
    if end and self.__global_end_node:
      end.close(self.__global_end_node)

    global_end_to_return.close(None)

    self.__compute_epsilon_closures(start)
    f = lambda node, state: self.__replace_catch_all(self.__encoding, node)
    Automaton.visit_states(set([start]), f)

    return Nfa(self.__encoding, start, global_end_to_return, nodes_created)

  @staticmethod
  def add_action(term, action):
    return Term('ACTION', term, action.to_term())

  @staticmethod
  def add_continue(term):
    return Term('CONTINUE', term)

  @staticmethod
  def unique_key(name):
    return Term('UNIQUE_KEY', name)

  @staticmethod
  def join_subgraph(term, name, subgraph_term):
    return Term('JOIN', term, name, subgraph_term)

  @staticmethod
  def or_terms(terms):
    return reduce(lambda acc, g: Term('OR', acc, g), terms)

  @staticmethod
  def cat_terms(terms):
    return reduce(lambda acc, g: Term('CAT', acc, g), terms)

  __modifer_map = {
    '+': 'ONE_OR_MORE',
    '?': 'ZERO_OR_ONE',
    '*': 'ZERO_OR_MORE',
  }

  @staticmethod
  def apply_modifier(modifier, term):
    return Term(NfaBuilder.__modifer_map[modifier], term)
