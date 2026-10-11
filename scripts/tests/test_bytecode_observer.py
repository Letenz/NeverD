#!/usr/bin/env python3
"""Independent synthetic language checks for the observation CLI."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ENGINE = str(Path(sys.argv.pop(1)).resolve())


def operand(space, offset, size=8):
    return {'space': space, 'size': size, 'value': {'addend': offset}}


def operation(name, inputs=(), output=None):
    result = {'op': name, 'inputs': list(inputs)}
    if output is not None:
        result['output'] = output
    return result


def profile():
    r = lambda n, size=8: operand('reg', n, size)
    c = lambda n, size=8: operand('const', n, size)
    copy = lambda n, target, size=8: operation('COPY', [c(n, size)], r(target, size))
    call = operation('CALL', [c(0x10000)])
    instructions = [
        [copy(0x1000, 0), copy(6, 8)],
        [operation('LOAD', [r(0)], r(16, 1)),
         operation('INT_XOR', [r(16, 1), c(0x55, 1)], r(16, 1)),
         operation('STORE', [r(0), r(16, 1)])],
        [operation('INT_ADD', [r(0), c(1)], r(0)),
         operation('INT_SUB', [r(8), c(1)], r(8)),
         operation('INT_NOTEQUAL', [r(8), c(0)], r(17, 1)),
         operation('COND_BR', [c(1), r(17, 1)])],
        [copy(0x1000, 0), call],
        [operation('RETURN')],
        [operation('INT_EQUAL', [r(24), c(0)], r(17, 1)),
         operation('COND_BR', [c(8), r(17, 1)])],
        [copy(0x2000, 0), call],
        [copy(0x3000, 0), call],
        [copy(0x123, 24, 4)],
        [copy(2, 24), operation('CALL', [c(0x10001)])],
    ]
    return {'version': 1, 'register_bytes': 32, 'temporary_bytes': 64,
            'byte_order': 'little', 'encodings': [
                {'size': 1, 'match': [{'offset': 0, 'value': i}], 'operations': ops}
                for i, ops in enumerate(instructions, 1)]}


class Observation(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='neverd-observation-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.language = profile()
        self.code = bytes([1, 2, 3, 4, 5, 6, 7, 5, 8, 5])
        self.functions = [{'entry': 0, 'end': 5, 'name': 'decode_loop'},
                          {'entry': 5, 'end': 10, 'name': 'unknown_branch'}]
        self.inputs = {'max_steps': 1000, 'max_paths': 8, 'max_visits': 8,
                       'registers': [], 'memory': [], 'calls': [
                           {'target': 0x10000, 'name': 'observe',
                            'argument_registers': [0]}]}
        for name, address, data in [('encoded', 0x1000, bytes(b ^ 0x55 for b in b'hello\0')),
                                    ('left', 0x2000, b'left\0'),
                                    ('right', 0x3000, b'right\0')]:
            path = self.root / name
            path.write_bytes(data)
            self.inputs['memory'].append({'address': address, 'path': str(path)})

    def run_engine(self, success=True):
        (self.root / 'code').write_bytes(self.code)
        for name, value in [('profile', self.language), ('functions', self.functions),
                            ('inputs', self.inputs)]:
            (self.root / name).write_text(json.dumps(value))
        out = self.root / 'result'
        out.write_text('retain previous result')
        proc = subprocess.run([ENGINE, *[str(self.root / name) for name in
                                        ('code', 'profile', 'functions', 'inputs', 'result')]],
                              capture_output=True, text=True, timeout=15)
        if success:
            self.assertEqual(proc.returncode, 0, proc.stderr)
            return json.loads(out.read_text())
        self.assertGreater(proc.returncode, 0, proc.stderr)
        self.assertEqual(out.read_text(), 'retain previous result')

    def test_computed_string_and_solver_branch(self):
        result = self.run_engine()
        first, second = result['functions']
        self.assertEqual(first['calls'][0]['arguments'][0]['string'], 'hello')
        self.assertEqual(first['stops'][0]['reason'], 'external_call')
        self.assertEqual({c['arguments'][0]['string'] for c in second['calls']},
                         {'left', 'right'})
        self.assertGreaterEqual(result['solver_queries'], 2)

    def test_missing_memory_is_not_invented(self):
        self.inputs['memory'] = []
        result = self.run_engine()
        for function in result['functions']:
            for call in function['calls']:
                self.assertNotIn('string', call['arguments'][0])

    def test_explicit_effects_and_default_havoc(self):
        self.code = bytes([7, 8, 5])
        self.functions = [{'entry': 0, 'end': 3, 'name': 'two_queries'}]
        spec = self.inputs['calls'][0]
        spec.update(continuation='register_effects', memory_effect='none')
        calls = self.run_engine()['functions'][0]['calls']
        self.assertEqual([c['arguments'][0]['string'] for c in calls], ['left', 'right'])
        self.assertEqual(calls[1]['prior_opaque_calls'], 1)
        self.assertIn('continuation_contract', calls[0])
        spec['continuation'] = 'havoc'
        calls = self.run_engine()['functions'][0]['calls']
        self.assertEqual(calls[0]['arguments'][0]['string'], 'left')
        self.assertNotIn('string', calls[1]['arguments'][0])
        self.assertEqual(calls[1]['arguments'][0]['initial_string_reference'], 'right')

    def test_selector_width_does_not_require_unknown_high_bytes(self):
        self.code = bytes([9, 7, 5])
        self.functions = [{'entry': 0, 'end': 3, 'name': 'narrow_selector'}]
        self.inputs['calls'][0].update(selector_register=24, selector_bytes=4,
                                      selector_names={'0x123': 'example_service'})
        call = self.run_engine()['functions'][0]['calls'][0]
        self.assertEqual(call['selector'], '0x123')
        self.assertEqual(call['service_name'], 'example_service')

    def test_explicit_dispatch_follows_declared_function(self):
        self.code = bytes([10, 5, 7, 5])
        self.functions = [{'entry': 0, 'end': 2, 'name': 'caller'},
                          {'entry': 2, 'end': 4, 'name': 'callee'}]
        self.inputs['entries'] = [0]
        self.inputs['calls'].append({'target': 0x10001, 'dispatch_register': 24})
        calls = self.run_engine()['functions'][0]['calls']
        self.assertEqual(calls[0]['dispatched_to'], '0x2')
        self.assertEqual(calls[1]['function'], '0x2')
        self.assertEqual(calls[1]['arguments'][0]['string'], 'left')

    def test_malformed_inputs_fail_without_overwriting(self):
        self.inputs['registers'] = [{'offset': 2**64-1, 'bytes': 8, 'value': 1}]
        self.run_engine(False)
        self.inputs['registers'] = [3]
        self.run_engine(False)
        self.inputs['registers'] = []
        self.functions.append(self.functions[0])
        self.run_engine(False)

    def test_annotation_preserves_body_and_escapes_comment_terminators(self):
        self.run_engine()
        report = json.loads((self.root / 'result').read_text())
        report['functions'][0]['calls'][0]['string'] = 'name*/injected'
        (self.root / 'result').write_text(json.dumps(report))
        source = 'uint64_t decode_loop(void *state) {\n    return 17;\n}\n'
        (self.root / 'source.c').write_text(source)
        proc = subprocess.run([ENGINE, '--annotate-c', str(self.root / 'source.c'),
                               str(self.root / 'result'), str(self.root / 'annotated.c')],
                              capture_output=True, text=True, timeout=15)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        output = (self.root / 'annotated.c').read_text()
        self.assertTrue(output.endswith(source))
        self.assertNotIn('name*/injected', output)
        self.assertIn('name* /injected', output)

    def test_big_endian_register_projection(self):
        self.language['byte_order'] = 'big'
        self.language['encodings'].append({
            'size': 1, 'match': [{'offset': 0, 'value': 11}],
            'operations': [operation('CALL', [operand('const', 0x10000)])]})
        self.code = bytes([11, 5])
        self.functions = [{'entry': 0, 'end': 2, 'name': 'big_endian'}]
        self.inputs['registers'] = [{'offset': 0, 'bytes': 8,
                                    'value': 0x1122334455667788}]
        self.inputs['calls'][0].update(selector_register=0, selector_bytes=4)
        call = self.run_engine()['functions'][0]['calls'][0]
        self.assertEqual(call['selector'], '0x11223344')

    def test_expression_display_is_bounded_before_rendering(self):
        ops = []
        for _ in range(25):
            ops.append(operation('INT_ADD', [operand('reg', 0), operand('const', 1)],
                                 operand('temp', 0)))
            ops.append(operation('INT_XOR', [operand('reg', 0), operand('temp', 0)],
                                 operand('reg', 0)))
        ops.append(operation('CALL', [operand('const', 0x10000)]))
        self.language['encodings'].append({
            'size': 1, 'match': [{'offset': 0, 'value': 11}], 'operations': ops})
        self.code = bytes([11, 5])
        self.functions = [{'entry': 0, 'end': 2, 'name': 'shared_expression'}]
        call = self.run_engine()['functions'][0]['calls'][0]
        self.assertEqual(call['arguments'][0]['value']['rendering_omitted'],
                         'expression budget')


if __name__ == '__main__':
    unittest.main()
