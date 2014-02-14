# Copyright 2014 the V8 project authors. All rights reserved.
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

from types import StringType, IntType

class Term(object):
  '''An immutable class representing a function and its arguments.
  f(a,b,c) would be represented as ('f', a, b, c) where
  a, b, and c are strings, integers or Terms.'''

  __empty_term = None

  @staticmethod
  def empty_term():
    if Term.__empty_term == None:
      Term.__empty_term = Term('')
    return Term.__empty_term

  def __init__(self, name, *args):
    assert type(name) == StringType
    assert name or not args, 'empty term must not have args'
    for v in args:
      assert type(v) == IntType or type(v) == StringType or isinstance(v, Term)
    self.__tuple = tuple([name] + list(args))
    self.__str = None

  def name(self):
    return self.__tuple[0]

  def args(self):
    return self.__tuple[1:]

  def  __hash__(self):
    return hash(self.__tuple)

  def __nonzero__(self):
    'true <==> self == empty_term'
    return bool(self.__tuple[0])

  def __eq__(self, other):
    return (isinstance(other, self.__class__) and self.__tuple == other.__tuple)

  # TODO(dcarney): escape '(', ')' and ',' in strings
  def __str__(self):
    if self.__str == None:
      self.__str = '(%s)' % ','.join(map(str, self.__tuple))
    return self.__str

class Action(object):

  __empty_action = None

  @staticmethod
  def empty_action():
    if Action.__empty_action == None:
      Action.__empty_action = Action(Term.empty_term(), -1)
    return Action.__empty_action

  @staticmethod
  def dominant_action(actions):
    dominant = Action.empty_action()
    for action in actions:
      if not action:
        continue
      if not dominant:
        dominant = action
        continue
      if action.precedence() == dominant.precedence():
        assert action.__term == dominant.__term
      elif action.precedence() < dominant.precedence():
        dominant = action
    return dominant

  def __init__(self, term, precedence):
    assert isinstance(term, Term)
    assert type(precedence) == IntType
    assert not term or precedence >= 0, 'action must have positive precedence'
    self.__term = term
    self.__precedence = precedence

  def name(self):
    return self.__term.name()

  def term(self):
    return self.__term

  def precedence(self):
    return self.__precedence

  def __nonzero__(self):
    'true <==> self == empty_action'
    return bool(self.__term)

  def __eq__(self, other):
    return isinstance(other, self.__class__) and self.__term == other.__term

  def __str__(self):
    return "action <%s>" % ('' if not self.__term else str(self.__term))
