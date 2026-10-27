#!/usr/bin/env python3
"""Compare supplied DEX objects/arrays and bidirectional JNI fields to Java.

The independent JVM oracle runs during preparation only. NeverD consumes the
compiled DEX and freestanding JNI caller, with no host Java runtime or device.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

from test_android_dex_runtime import run


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--jdk', type=Path, required=True)
    parser.add_argument('--d8', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--backend', choices=('unicorn', 'hvf'), default='unicorn')
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    source = root / 'scripts/tests/fixtures/dalvik-objects'
    args.output.mkdir(parents=True, exist_ok=False)
    classes, dex_dir = args.output / 'classes', args.output / 'dex'
    classes.mkdir()
    dex_dir.mkdir()
    java = args.jdk / 'bin/java'
    run([args.jdk / 'bin/javac', '--release', '8', '-g', '-d', classes,
         *sorted(source.glob('*.java'))])
    run([java, '-cp', args.d8, 'com.android.tools.r8.D8', '--debug', '--min-api', '28',
         '--output', dex_dir, classes / 'runtime/objects/Box.class'])
    dex = (dex_dir / 'classes.dex').read_bytes().hex()
    fixtures = list(args.build.rglob('jni-O2-relr.so'))
    if len(fixtures) != 1:
        raise RuntimeError(f'expected one compiled JNI fixture: {fixtures}')
    probe = args.build / 'bin/neverd-process-probe'
    records = []

    def execute(seed, form, scenario, expected, stop='returned', diagnostic=''):
        label = f'{seed}-{form}-{scenario}'
        out = 0x65000000
        options = {'backend': args.backend, 'instruction_limit': 200000,
                   'timeout_microseconds': 10000000,
                   'android': {'entry_symbol': 'jni_dex_objects', 'initialize': False,
                               'jni': True, 'dex_images': [dex],
                               'arguments': [seed & 0xffffffff, form, scenario, out],
                               'memory': [{'address': out, 'size': 4096}],
                               'read_memory': [{'address': out, 'size': 256}]}}
        request = args.output / f'{label}.request.json'
        report_path = args.output / f'{label}.report.json'
        request.write_text(json.dumps(options))
        run([probe, fixtures[0], 'android-aarch64-api28-v1', request, report_path])
        report = json.loads(report_path.read_text())
        if report['stop_reason'] != stop or diagnostic not in report.get('diagnostic', ''):
            raise AssertionError((label, report['stop_reason'], report.get('diagnostic')))
        if stop == 'returned':
            if int(report['return_value'], 16):
                raise AssertionError((label, report['return_value'], report.get('diagnostic')))
            actual = bytes.fromhex(report['android']['memory'][0]['bytes_hex'])
            values = list(struct.unpack('<32Q', actual))
            if values != expected:
                raise AssertionError((label, [(i, a, b) for i, (a, b) in
                                             enumerate(zip(values, expected)) if a != b]))
        records.append({'case': label, 'stop': stop, 'compared_words': len(expected) if scenario == 0 else 0,
                        'diagnostic': report.get('diagnostic', '')})

    for seed in (0, 1, -1, -2147483648, 2147483647, 65535, -32769):
        oracle = run([java, '-cp', classes, 'runtime.objects.ObjectsOracle', seed]).stdout
        (args.output / f'{seed}.oracle.txt').write_text(oracle)
        expected = [int(line, 16) for line in oracle.splitlines()]
        if len(expected) != 32:
            raise AssertionError('unexpected JVM oracle shape')
        for form in range(4):
            execute(seed, form, 0, expected)
    for scenario, stop, diagnostic in (
        (1, 'runtime_failure', 'accessor type'),
        (2, 'runtime_failure', 'final field write'),
        (3, 'runtime_failure', 'receiver type'),
        (4, 'runtime_failure', 'field ID'),
        (5, 'unsupported_service', 'array storage budget')):
        execute(1, 2, scenario, [], stop, diagnostic)
    execute(1, 2, 6, [0] * 32)
    identities = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in
                  [*sorted(source.glob('*.java')), dex_dir / 'classes.dex', fixtures[0], probe,
                   Path(__file__), root / 'unittests/emulation/fixtures/android_jni.c']}
    summary = {'backend': args.backend, 'runs': len(records),
               'compared_words': sum(row['compared_words'] for row in records),
               'inputs_sha256': identities, 'records': records}
    (args.output / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps({key: summary[key] for key in ('backend', 'runs', 'compared_words')}))


if __name__ == '__main__':
    main()
