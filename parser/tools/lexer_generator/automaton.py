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
from transition_keys import TransitionKey

class Action(object):
  pass

class AutomatonState(object):

  def __init__(self, node_number):
    self.__node_number = node_number

  def node_number(self):
    return self.__node_number

class Automaton(object):

  @staticmethod
  def visit_edges(edge, compute_next_edge, visitor, state):
    visited = set()
    while edge:
      f = lambda (next_edge, state), node: (
        next_edge | compute_next_edge(node),
        visitor(node, state))
      (next_edge, state) = reduce(f, edge, (set(), state))
      visited |= edge
      edge = next_edge - visited
    return state

  @staticmethod
  def generate_dot(start_node, terminal_set, edge_iterator):

    def f(node, node_content):
      for key, values in node.transitions().items():
        if key == TransitionKey.epsilon():
          key = "&epsilon;"
        key = str(key).replace('\\', '\\\\')
        # TODO pass this action as parameter
        if type(values) == TupleType:
          values = [values]
        for (state, action) in values:
          if action:
            node_content.append(
                "  S_%s -> S_%s [ label = \"%s {%s} -> %s\" ];" %
                (node.node_number(), state.node_number(), key, action[1],
                 action[2]))
          else:
            node_content.append(
                "  S_%s -> S_%s [ label = \"%s\" ];" %
                (node.node_number(), state.node_number(), key))
      return node_content

    node_content = edge_iterator(f, [])
    terminals = ["S_%d;" % x.node_number() for x in terminal_set]
    start_number = start_node.node_number()
    start_shape = "circle"
    if start_node in terminal_set:
      start_shape = "doublecircle"

    return '''
digraph finite_state_machine {
  rankdir=LR;
  node [shape = %s, style=filled, bgcolor=lightgrey]; S_%s
  node [shape = doublecircle, style=unfilled]; %s
  node [shape = circle];
%s
}
    ''' % (start_shape,
           start_number,
           " ".join(terminals),
           "\n".join(node_content))
