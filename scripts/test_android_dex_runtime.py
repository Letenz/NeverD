#!/usr/bin/env python3
"""Compile original Java with javac/D8, compare its JVM values with JNI DEX execution.

No Android device or host JVM is used by the tested NeverD runtime. The JVM is
only an independent oracle before the native workloads start. All intermediates
and reports remain in the caller-selected output directory.
"""
import argparse
import json
import hashlib
from pathlib import Path
import subprocess
import struct
import ctypes
import sys
from types import SimpleNamespace


def run(argv, **kwargs):
    return subprocess.run([str(x) for x in argv], check=True, capture_output=True,
                          text=True, timeout=90, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--jdk', type=Path, required=True)
    parser.add_argument('--d8', type=Path, required=True, help='D8 distribution jar')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--backend', choices=('unicorn', 'hvf'), default='unicorn')
    parser.add_argument('--forms', default='0,1,2')
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    source = root / 'scripts/tests/fixtures/dalvik-runtime'
    args.output.mkdir(parents=True, exist_ok=False)
    classes, dex_dir = args.output / 'classes', args.output / 'dex'
    classes.mkdir()
    dex_dir.mkdir()
    java = args.jdk / 'bin/java'
    run([args.jdk / 'bin/javac', '--release', '8', '-g', '-d', classes,
         *sorted(source.glob('*.java'))])
    oracle = run([java, '-cp', classes, 'runtime.Oracle']).stdout
    (args.output / 'oracle.txt').write_text(oracle)
    run([java, '-cp', args.d8, 'com.android.tools.r8.D8', '--debug', '--min-api', '28',
         '--output', dex_dir, classes / 'runtime/Arithmetic.class', classes / 'runtime/Helper.class'])
    dex = (dex_dir / 'classes.dex').read_bytes().hex()
    fixtures = list(args.build.rglob('jni-O2-relr.so'))
    if len(fixtures) != 1:
        raise RuntimeError(f'expected one compiled JNI fixture: {fixtures}')
    records = []
    receipts = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in
                [*sorted(source.glob('*.java')), dex_dir / 'classes.dex', *fixtures,
                 args.build / 'bin/neverd-process-probe']}

    kinds = {'I': 0, 'J': 1, 'Z': 2, 'B': 3, 'C': 4, 'S': 5}
    probe = args.build / 'bin/neverd-process-probe'

    def execute(name, signature, values, form, expected=None, stop='returned',
                instructions=100000, images=None, jni=True, error=None, kind=None):
        label = f'{len(records):04d}-{name}-{form}'
        data = bytearray(512)
        for at, text in [(0, 'runtime/Arithmetic'), (64, name), (128, signature)]:
            encoded = text.encode() + b'\0'
            data[at:at + len(encoded)] = encoded
        for i, value in enumerate(values):
            struct.pack_into('<Q', data, 256 + i * 8, value & ((1 << 64) - 1))
        address = 0x65000000
        options = {'backend': args.backend, 'instruction_limit': instructions,
                   'timeout_microseconds': 10000000,
                   'android': {'entry_symbol': 'jni_dex_call', 'initialize': False,
                               'jni': jni, 'dex_images': [dex] if images is None else images,
                               'arguments': [address, address + 64, address + 128,
                                             address + 256, kinds[signature[-1]] if kind is None else kind, form],
                               'memory': [{'address': address, 'size': 4096,
                                           'bytes_hex': data.hex()}]}}
        request = args.output / f'{label}.request.json'
        report = args.output / f'{label}.report.json'
        request.write_text(json.dumps(options))
        result = subprocess.run([str(probe), str(fixtures[0]), 'android-aarch64-api28-v1',
                                 str(request), str(report)], text=True, capture_output=True, timeout=45)
        if error is not None:
            if result.returncode == 0 or error not in result.stderr:
                raise AssertionError((label, result.returncode, result.stderr))
            records.append({'case': label, 'rejected': error})
            return
        if result.returncode:
            raise AssertionError((label, result.stderr))
        observed = json.loads(report.read_text())
        if observed['stop_reason'] != stop:
            raise AssertionError((label, observed['stop_reason'], observed.get('diagnostic')))
        if expected is not None and int(observed['return_value'], 16) != expected:
            raise AssertionError((label, observed['return_value'], hex(expected)))
        if not records:
            cli = run([args.build / 'bin/neverd', 'emulate', fixtures[0],
                       '--profile=android-aarch64-api28-v1', '--options=' + json.dumps(options)])
            cli_result = json.loads(cli.stdout)
            if cli_result != observed:
                raise AssertionError('CLI and direct process API disagree')
            if args.backend == 'unicorn':
                sys.path.insert(0, str(root / 'pluginsdk/python'))
                from neverd_plugin import Session
                from neverd_plugin.ffi import HostAPI
                libraries = [args.build / 'bin' / name for name in
                             ('libneverd.dylib', 'libneverd.so', 'neverd.dll')]
                host = HostAPI(ctypes.CDLL(str(next(p for p in libraries if p.is_file()))))
                address = int(host.call('neverd_session_create') or 0)
                if not address:
                    raise AssertionError('SDK session creation failed')
                handle = ctypes.c_void_p(address)
                try:
                    session = Session(handle, _native=SimpleNamespace(session_address=lambda _: address), _host=host)
                    sdk = session.emulate_process(str(fixtures[0]), 'android-aarch64-api28-v1', json.dumps(options))
                    if sdk != observed:
                        raise AssertionError('Python/C ABI and process API disagree')
                finally:
                    host.call('neverd_session_destroy', handle)
        records.append({'case': label, 'stop': stop, 'value': observed.get('return_value')})

    for line in oracle.splitlines():
        name, signature, x, y, z, expected = line.split()
        values = [int(x, 16), int(y, 16), int(z, 16)]
        for form in map(int, args.forms.split(',')):
            execute(name, signature, values, form, int(expected, 16))
    for form in map(int, args.forms.split(',')):
        execute('divide', '(II)I', [7, 0], form, 0xdec0de)
        execute('wideDivide', '(JIJ)J', [7, 1, 0], form, 0xdec0de)
        execute('forever', '(II)I', [0, 1], form, stop='instruction_limit', instructions=2000)
        execute('recurse', '(II)I', [10000, 1], form, stop='unsupported_service')
        execute('objectState', '(II)I', [0, 1], form, stop='unsupported_service')
        execute('handled', '(II)I', [7, 0], form, expected=77)
    execute('mix', '(II)I', [1, 2], 2, kind=1, stop='runtime_failure')
    execute('absent', '(II)I', [1, 2], 2, stop='unsupported_service')
    execute('mix', '(II)I', [1, 2], 2, images=[dex, dex], error='duplicate supplied DEX class')
    execute('mix', '(II)I', [1, 2], 2, jni=False, error='DEX images require JNI')
    corrupt = bytearray.fromhex(dex)
    corrupt[-1] ^= 1
    execute('mix', '(II)I', [1, 2], 2, images=[corrupt.hex()], error='invalid supplied DEX')
    execute('mix', '(II)I', [1, 2], 2, images=['00'], error='requires DEX version')
    execute('mix', '(II)I', [1, 2], 2, images=['x'], error='dex_images')
    (args.output / 'summary.json').write_text(json.dumps({'backend': args.backend,
        'oracle_cases': len(oracle.splitlines()), 'runs': len(records), 'inputs_sha256': receipts, 'records': records}, indent=2) + '\n')
    print(json.dumps({'backend': args.backend, 'oracle_cases': len(oracle.splitlines()), 'runs': len(records)}))


if __name__ == '__main__':
    main()
