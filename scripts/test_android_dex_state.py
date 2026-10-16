#!/usr/bin/env python3
"""Compare persistent supplied DEX class state with an independent JVM run.

The JVM and D8 prepare the oracle and explicit DEX input. NeverD executes only
the DEX in its workload VM; the freestanding native fixture just calls JNI.
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
    source = root / 'scripts/tests/fixtures/dalvik-state'
    args.output.mkdir(parents=True, exist_ok=False)
    classes, dex_dir = args.output / 'classes', args.output / 'dex'
    classes.mkdir()
    dex_dir.mkdir()
    java = args.jdk / 'bin/java'
    run([args.jdk / 'bin/javac', '--release', '8', '-g', '-d', classes,
         *sorted(source.glob('*.java'))])
    class_files = sorted(p for p in classes.rglob('*.class') if p.stem != 'StateOracle')
    run([java, '-cp', args.d8, 'com.android.tools.r8.D8', '--debug', '--min-api', '28',
         '--output', dex_dir, *class_files])
    dex = (dex_dir / 'classes.dex').read_bytes().hex()
    fixtures = list(args.build.rglob('jni-O2-relr.so'))
    if len(fixtures) != 1:
        raise RuntimeError(f'expected one compiled JNI fixture: {fixtures}')
    probe = args.build / 'bin/neverd-process-probe'
    records = []
    compared = 0

    def execute(label, cases, detach, stop='returned', instructions=100000):
        base, out = 0x65000000, 0x65002000
        data = b''.join(name.encode().ljust(64, b'\0') + struct.pack('<Q', value)
                        for name, value, _, _ in cases)
        options = {'backend': args.backend, 'instruction_limit': instructions,
                   'timeout_microseconds': 10000000,
                   'android': {'entry_symbol': 'jni_dex_state', 'initialize': False,
                               'jni': True, 'dex_images': [dex],
                               'thread_limit': 64 if detach == 2 else 1,
                               'arguments': [base, len(cases), out, detach],
                               'memory': [{'address': base, 'size': 8192,
                                           'bytes_hex': data.hex()},
                                          {'address': out, 'size': 4096}],
                               'read_memory': [{'address': out, 'size': 16 * len(cases)}]}}
        request = args.output / f'{label}.request.json'
        report_path = args.output / f'{label}.report.json'
        request.write_text(json.dumps(options))
        run([probe, fixtures[0], 'android-aarch64-api28-v1', request, report_path])
        report = json.loads(report_path.read_text())
        if report['stop_reason'] != stop:
            raise AssertionError((label, report['stop_reason'], report.get('diagnostic')))
        if stop == 'returned':
            if int(report['return_value'], 16):
                raise AssertionError((label, report['return_value']))
            actual = bytes.fromhex(report['android']['memory'][0]['bytes_hex'])
            expected = b''.join(struct.pack('<QQ', status, value)
                                for _, _, status, value in cases)
            if actual != expected:
                words = list(struct.iter_unpack('<QQ', actual))
                mismatches = [(i, case, got) for i, (case, got) in enumerate(zip(cases, words))
                              if case[2:] != got]
                raise AssertionError((label, mismatches))
        elif stop == 'instruction_limit' and report['instructions'] != instructions:
            raise AssertionError((label, report['instructions'], instructions))
        records.append({'case': label, 'stop': stop, 'compared_steps': len(cases) if stop == 'returned' else 0,
                        'diagnostic': report.get('diagnostic', '')})

    for order in ('normal', 'reversed'):
        oracle = run([java, '-cp', classes, 'runtime.state.StateOracle',
                      *([] if order == 'normal' else ['reverse'])]).stdout
        (args.output / f'{order}.oracle.txt').write_text(oracle)
        cases = []
        for line in oracle.splitlines():
            name, value, status, result = line.split()
            cases.append((name, int(value, 16), int(status), int(result, 16)))
        for detach in (0, 1, 2):
            execute(f'{order}-detach-{detach}', cases, detach)
            compared += len(cases)
    for name, stop, limit in [('VolatileField', 'runtime_failure', 100000),
                              ('UnsupportedInitializer', 'unsupported_service', 100000),
                              ('EndlessInitializer', 'instruction_limit', 2000)]:
        execute(name, [(f'runtime/state/{name}', 0, 0, 0)], 0, stop, limit)
    identities = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in
                  [*sorted(source.glob('*.java')), dex_dir / 'classes.dex', fixtures[0], probe,
                   Path(__file__)]}
    summary = {'backend': args.backend, 'runs': len(records), 'compared_steps': compared,
               'inputs_sha256': identities, 'records': records}
    (args.output / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps({key: summary[key] for key in ('backend', 'runs', 'compared_steps')}))


if __name__ == '__main__':
    main()
