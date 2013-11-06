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

import unittest
from rule_parser import RuleParserState
from regex_parser import RegexParser

class RuleParserTestCase(unittest.TestCase):

   def setUp(self):
     self.state = RuleParserState()

   def parse(self, string):
    return self.state.parse(string)

   def test_basic(self):
     self.parse('''
alias = /regex/;
<cond1> /regex/ :=> cond2
<cond1> alias :=> cond2
<cond2> /regex/ {body}
<cond2> alias {body}
<cond3> /regex/ => cond4 {body}
<cond3> alias => cond4 {body}''')

     self.assertTrue(len(self.state.aliases), 1)
     self.assertTrue('alias' in self.state.aliases)
     self.assertEquals(self.state.aliases['alias'], RegexParser.parse('regex'))

     self.assertTrue(len(self.state.rules), 2)
     self.assertTrue('cond1' in self.state.rules)
     self.assertEquals(len(self.state.rules['cond1']), 2)
     # self.assertTrue('regex2' in self.state.rules['cond1'])
     # self.assertEquals(self.state.rules['cond1']['regex2'],
     #                   ('condition', 'cond2'))

     self.assertTrue('cond2' in self.state.rules)
     self.assertEquals(len(self.state.rules['cond2']), 2)
     # self.assertTrue('regex3' in self.state.rules['cond2'])
     # self.assertEquals(self.state.rules['cond2']['regex3'],
     #                   ('body', 'body'))

     self.assertTrue('cond3' in self.state.rules)
     self.assertEquals(len(self.state.rules['cond3']), 2)
     # self.assertTrue('regex4' in self.state.rules['cond3'])
     # self.assertEquals(self.state.rules['cond3']['regex4'],
     #                   ('condition_and_body', 'cond4', 'body'))

   def test_more_complicated(self):
     self.parse('alias = regex;with;semicolon;')
     self.parse('<cond1> regex2 :=> with :=> arrow :=> cond2')
     self.parse('<cond1> regex3}with}braces} {body {with} braces } }')
     self.parse('<cond1> regex4{with{braces} {body {with} braces } }')

     self.assertEquals(self.parse['alias'], 'regex;with;semicolon')

     self.assertEquals(
         self.parse['cond1']['regex2 :=> with :=> arrow'],
         ('condition', 'cond2'))

     self.assertEquals(
         self.parse['cond1']['regex3}with}braces}'],
         ('body', 'body {with} braces }'))

     self.assertEquals(
         self.parse['cond1']['regex4{with{braces}'],
         ('body', 'body {with} braces }'))

   def test_body_with_if(self):
     self.parse('<cond> regex { if (foo) { bar } }')
     self.assertEquals(
         self.parse['cond']['regex'],
         ('body', 'if (foo) { bar }'))

   def test_regexp_with_count(self):
     self.parse('<cond> regex{1,3} { if (foo) { bar } }')
     self.assertEquals(
         self.parse['cond']['regex{1,3}'],
         ('body', 'if (foo) { bar }'))

if __name__ == '__main__':
    unittest.main()
