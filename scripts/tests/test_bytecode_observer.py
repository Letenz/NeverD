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

    def test_copy_disjointness_uses_symbolic_offsets_and_path_constraints(self):
        # r24 is an unknown base. Store one byte in base+32, then copy it to
        # base+8. The absolute addresses are unknown, but non-overlap is exact.
        r = lambda n: operand('reg', n)
        c = lambda n: operand('const', n)
        ops = [operation('INT_ADD', [r(24), c(8)], r(0)),
               operation('INT_ADD', [r(24), c(32)], r(8)),
               operation('STORE', [r(8), operand('const', 0x6a, 1)]),
               operation('COPY', [c(1)], r(16)),
               operation('CALL', [c(0x10001)])]
        self.language['encodings'] += [
            {'size': 1, 'match': [{'offset': 0, 'value': 11}], 'operations': ops},
            {'size': 1, 'match': [{'offset': 0, 'value': 12}], 'operations': [
                operation('LOAD', [r(0)], operand('reg', 16, 1)),
                operation('CALL', [c(0x10000)])]}]
        self.code = bytes([11, 12, 5])
        self.functions = [{'entry': 0, 'end': 3, 'name': 'relative_copy'}]
        self.inputs['calls'][0]['argument_registers'] = [16]
        self.inputs['registers'] = [{'offset': 16, 'bytes': 8, 'value': 0}]
        self.inputs['calls'].append({'target': 0x10001,
                                    'continuation': 'memory_copy',
                                    'destination_register': 0, 'source_register': 8,
                                    'size_register': 16, 'require_disjoint': True})
        function = self.run_engine()['functions'][0]
        self.assertTrue(function['calls'][0]['disjoint_ranges_proved'])
        self.assertEqual(function['calls'][1]['arguments'][0]['value'], '0x6a')
        # Unknown unrelated pointers may alias; equal pointers do alias.
        saved = ops[:2]
        ops[:2] = [operation('COPY', [r(24)], r(0)),
                   operation('COPY', [r(8)], r(8))]
        self.assertEqual(self.run_engine()['functions'][0]['stops'][0]['reason'],
                         'unresolved_memory_effect')
        ops[1] = operation('COPY', [r(24)], r(8))
        self.assertEqual(self.run_engine()['functions'][0]['stops'][0]['reason'],
                         'unresolved_memory_effect')
        ops[:2] = saved
        # Multi-byte ranges: touching is disjoint, overlapping is not.
        ops[3] = operation('COPY', [c(24)], r(16))
        self.assertEqual(len(self.run_engine()['functions'][0]['calls']), 2)
        ops[3] = operation('COPY', [c(25)], r(16))
        self.assertEqual(self.run_engine()['functions'][0]['stops'][0]['reason'],
                         'unresolved_memory_effect')
        # A guard can establish disjointness for otherwise unrelated pointers.
        ops[:2] = [operation('COPY', [r(24)], r(0)),
                   operation('COPY', [r(8)], r(8))]
        ops[3] = operation('COPY', [c(1)], r(16))
        self.language['encodings'].append({
            'size': 1, 'match': [{'offset': 0, 'value': 13}], 'operations': [
                operation('INT_NOTEQUAL', [r(24), r(8)], operand('temp', 0, 1)),
                operation('COND_BR', [c(2), operand('temp', 0, 1)])]})
        self.code = bytes([13, 5, 11, 12, 5])
        self.functions[0]['end'] = 5
        report = self.run_engine()
        self.assertEqual(len(report['functions'][0]['calls']), 2)
        self.assertGreaterEqual(report['solver_queries'], 2)

    def test_memory_contracts_use_exact_ranges(self):
        def load(value, register):
            return operation('COPY', [operand('const', value)], operand('reg', register))
        self.language['encodings'] += [
            {'size': 1, 'match': [{'offset': 0, 'value': 11}], 'operations': [
                load(0x4000, 0), load(0x3000, 8), load(6, 16), load(0, 24),
                operation('CALL', [operand('const', 0x10001)])]},
            {'size': 1, 'match': [{'offset': 0, 'value': 12}], 'operations': [
                operation('CALL', [operand('const', 0x10000)])]}]
        self.code = bytes([11, 12, 5])
        self.functions = [{'entry': 0, 'end': 3, 'name': 'memory_calls'}]
        spec = {'target': 0x10001, 'continuation': 'memory_copy',
                'destination_register': 0, 'source_register': 8, 'size_register': 16,
                'result_offset_register': 24, 'return_destination': True}
        self.inputs['calls'].append(spec)
        calls = self.run_engine()['functions'][0]['calls']
        self.assertEqual(calls[1]['arguments'][0]['string'], 'right')
        ops = self.language['encodings'][-2]['operations']
        for destination, count in ((0x4000, 4097), (0xffffffffffffffff, 6)):
            ops[0] = load(destination, 0)
            ops[2] = load(count, 16)
            stopped = self.run_engine()['functions'][0]
            self.assertEqual(stopped['stops'][0]['reason'], 'unresolved_memory_effect')
        ops[2] = load(6, 16)
        ops[0] = load(0x3001, 0)
        calls = self.run_engine()['functions'][0]['calls']
        self.assertEqual(calls[1]['arguments'][0]['string'], 'right')
        spec['require_disjoint'] = True
        stopped = self.run_engine()['functions'][0]
        self.assertEqual(stopped['stops'][0]['reason'], 'unresolved_memory_effect')
        del spec['require_disjoint']
        ops[0] = load(0x4000, 0)
        # Fill four bytes only: the adjacent 't' and terminator must survive.
        data = self.root / 'destination'
        data.write_bytes(b'right\0')
        self.inputs['memory'].append({'address': 0x4000, 'path': str(data)})
        ops = self.language['encodings'][-2]['operations']
        ops[1] = load(ord('A'), 8)
        ops[2] = load(4, 16)
        spec['continuation'] = 'memory_fill'
        calls = self.run_engine()['functions'][0]['calls']
        self.assertEqual(calls[1]['arguments'][0]['string'], 'AAAAt')
        spec['continuation'] = 'memory_havoc'
        calls = self.run_engine()['functions'][0]['calls']
        self.assertNotIn('string', calls[1]['arguments'][0])
        self.assertEqual(calls[1]['arguments'][0]['initial_string_reference'], 'right')
        spec['return_destination'] = 'true'
        self.run_engine(success=False)
        del spec['return_destination']
        del spec['size_register']
        self.run_engine(success=False)


if __name__ == '__main__':
    unittest.main()
