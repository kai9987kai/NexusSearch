"""Regression tests for the bounded MCP stdio protocol subset."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
EXE = ROOT / 'build' / ('nexus.exe' if os.name == 'nt' else 'nexus')

class McpTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='nexus-mcp-')
        cls.snapshot = Path(cls.temp.name) / 'test.nxs'
        subprocess.run([str(EXE), 'build', str(ROOT/'examples/documents.jsonl'), str(cls.snapshot)], check=True, capture_output=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def exchange(self, requests=None, raw=None):
        wire = raw if raw is not None else ''.join(json.dumps(r, ensure_ascii=True)+'\n' for r in requests)
        result = subprocess.run([str(EXE), 'mcp', str(self.snapshot)], input=wire.encode(), capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr.decode(errors='replace'))
        return [json.loads(line) for line in result.stdout.splitlines()]

    def request(self, method, ident=1, **extra):
        return {'jsonrpc':'2.0', 'id':ident, 'method':method, **extra}

    def call(self, name, arguments):
        return self.request('tools/call', params={'name':name, 'arguments':arguments})

    def test_ids_and_discovery(self):
        ids = [1, 9007199254740993, 'quoted"\\\nCafé', None, '']
        replies = self.exchange([self.request('ping', i) for i in ids])
        self.assertEqual([r['id'] for r in replies], ids)
        self.assertTrue(all(r['result'] == {} for r in replies))
        init, listing = self.exchange([self.request('initialize'), self.request('tools/list')])
        self.assertEqual(init['result']['protocolVersion'], '2024-11-05')
        self.assertEqual(len(listing['result']['tools']), 4)

    def test_notifications_have_no_response(self):
        replies = self.exchange([{'jsonrpc':'2.0','method':method} for method in ('notifications/initialized','ping','unknown')] + [self.request('ping')])
        self.assertEqual(len(replies),1)
        self.assertEqual(replies[0]['result'], {})

    def test_quoted_unicode_search_and_tool_errors(self):
        replies = self.exchange([self.call('nexus_search', {'query':q}) for q in ('title="Local search in C"','body:"Café"','unknown:123')])
        for reply, expected in zip(replies[:2], ('paper-01','paper-05')):
            self.assertFalse(reply['result']['isError'])
            data=json.loads(reply['result']['content'][0]['text'])
            self.assertEqual([h['_id'] for h in data['hits']], [expected])
        self.assertTrue(replies[2]['result']['isError'])
        self.assertIn('error',json.loads(replies[2]['result']['content'][0]['text']))

    def test_invalid_tool_parameters(self):
        invalid = [self.call('nexus_search',args) for args in ({'query':''},{'query':'*\0 OR invalid:1'},{'query':'a'*2048},{'query':12},{'query':'*','scan':'false'})]
        invalid += [self.call('nexus_get_document', {'row':row}) for row in (-1,4294967296,2**80,1.5,'0')]
        invalid += [self.call('unknown', {}), self.request('tools/call', params={}), self.request('tools/call', params=[])]
        for reply in self.exchange(invalid):
            self.assertEqual(reply['error']['code'],-32602)
            self.assertNotIn('result',reply)

    def test_unknown_method_and_invalid_envelopes(self):
        requests = [self.request('unknown'), {'method':'ping','id':1}, self.request('ping', ident=[]), self.request(123), 12]
        replies = self.exchange(requests)
        self.assertEqual([r['error']['code'] for r in replies],[-32601,-32600,-32600,-32600,-32600])

    def test_malformed_and_oversized_frame_recovery(self):
        ping=json.dumps(self.request('ping'))+'\n'
        replies=self.exchange(raw='{broken}\n'+' '*70000+ping+ping)
        self.assertEqual([r.get('error',{}).get('code') for r in replies],[-32700,-32700,None])
        self.assertEqual(replies[-1]['result'],{})
        self.assertEqual(self.exchange(raw=ping.rstrip('\n'))[0]['error']['code'],-32700)

    def test_document_and_explain(self):
        doc,plan=self.exchange([self.call('nexus_get_document',{'row':0}),self.call('nexus_explain',{'query':'year:>=2025'})])
        self.assertEqual(json.loads(doc['result']['content'][0]['text'])['_id'],'paper-01')
        data=json.loads(plan['result']['content'][0]['text'])
        self.assertTrue(data['explain_only'])
        self.assertEqual(data['execution']['work'],0)

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe',default=str(EXE))
    args,remaining=parser.parse_known_args()
    EXE=Path(args.exe).resolve()
    unittest.main(argv=[sys.argv[0],*remaining],verbosity=2)
