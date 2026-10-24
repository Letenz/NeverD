#!/usr/bin/env python3
"""Owned compiler fixtures: retain finite regions and expose patch residuals.

This small source family is a functional regression, not a held-out accuracy or
performance benchmark. Artifacts record exact inputs, tool versions and flags.
No produced input is executed. GCC profiles are optional when a native x64 Linux
GCC is absent; Clang's x64 ELF and Windows COFF profiles are required.
"""
import argparse
import hashlib
import json
import pathlib
import subprocess
import tempfile


def digest(path):
    return hashlib.sha256(pathlib.Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--neverd', required=True)
    parser.add_argument('--clang', required=True)
    parser.add_argument('--gcc')
    parser.add_argument('--fixture', required=True)
    parser.add_argument('--artifacts', required=True)
    args = parser.parse_args()
    output = pathlib.Path(args.artifacts)
    output.mkdir(parents=True, exist_ok=True)
    root = pathlib.Path(tempfile.mkdtemp(prefix='run-', dir=output)).resolve()
    manifest = {'schema': 'neverd.bindiff.compiler-fixtures.v1',
                'source_sha256': digest(args.fixture), 'compilers': {},
                'variants': {}, 'comparisons': [], 'skipped': [], 'status': 'running'}

    def save_manifest():
        (root / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')

    def run(name, command, expected=(0,)):
        result = subprocess.run(command, capture_output=True, text=True, timeout=60)
        (root / (name + '.stdout')).write_text(result.stdout)
        (root / (name + '.stderr')).write_text(result.stderr)
        assert result.returncode in expected, (name, result.returncode, root, result.stderr[-3000:])
        return result

    def compiler(name, path):
        result = run(name + '-version', [path, '--version'])
        manifest['compilers'][name] = {'path': str(pathlib.Path(path).resolve()),
                                       'version': result.stdout.splitlines()[0]}

    compiler('clang', args.clang)
    gcc = None
    if args.gcc:
        compiler('gcc', args.gcc)
        target = run('gcc-target', [args.gcc, '-dumpmachine']).stdout.strip()
        if target.startswith('x86_64') and 'linux' in target and 'clang' not in manifest['compilers']['gcc']['version'].lower():
            gcc = args.gcc
    if not gcc:
        manifest['skipped'].append('GCC profiles: no native x86_64 Linux GCC')

    def build(name, path, flags, source=None):
        source = source or args.fixture
        target = root / (name + ('.obj' if 'coff' in name else '.o'))
        run('build-' + name, [path, '-c', *flags, str(source), '-o', str(target)])
        manifest['variants'][name] = {'path': str(target), 'sha256': digest(target),
                                      'compiler': str(path), 'flags': flags,
                                      'source_sha256': digest(source)}
        return target

    variants = {}
    for level in ('0', '1', '2', '3', 's'):
        variants['clang-elf-O' + level] = build('clang-elf-O' + level, args.clang,
            ['--target=x86_64-unknown-linux-gnu', '-O' + level, '-fno-asynchronous-unwind-tables'])
        if gcc:
            variants['gcc-elf-O' + level] = build('gcc-elf-O' + level, gcc,
                ['-m64', '-O' + level, '-fno-asynchronous-unwind-tables'])
    for level in ('0', '2'):
        variants['clang-coff-O' + level] = build('clang-coff-O' + level, args.clang,
            ['--target=x86_64-pc-windows-msvc', '-O' + level])
    patched = build('clang-elf-patched', args.clang,
                    ['--target=x86_64-unknown-linux-gnu', '-O2', '-fno-asynchronous-unwind-tables', '-DFINAL_ADD=5u'])
    save_manifest()

    def compare(name, a, b, extra=(), require_anchor=True):
        command = [args.neverd, 'diff', '--structural', '-a', str(a), '-b', str(b),
                   '--ignore-names', '--flow-regions', '--json', *extra]
        result = run(name, command, (0, 2))
        document = json.loads(result.stdout)
        assert document['state'] == 'complete', (name, document['stopped_phase'])
        regions = document['flow_regions']
        assert regions['state'] == 'complete' and regions['primary_scope_complete'] and regions['secondary_scope_complete'], (name, regions)
        if require_anchor:
            assert regions['anchor_count'] > 0 and regions['hypothesis_count'] == 1, (name, regions)
        assert regions['semantic_validation'] == 'not_performed'
        assert all(h['open_control_frontier'] for h in regions['hypotheses'])
        assert not document['settings']['use_names']
        for side in ('primary', 'secondary'):
            assert len(document['dataflow'][side]['entries']) == 1
            assert document['dataflow'][side]['entries'][0]['availability'] == 'available'
        manifest['comparisons'].append({'name': name, 'primary_sha256': digest(a),
            'secondary_sha256': digest(b), 'exit': result.returncode,
            'anchors': regions['anchor_count'], 'hypotheses': regions['hypothesis_count'],
            'primary_coverage': [h['primary_coverage'] for h in regions['hypotheses']],
            'secondary_coverage': [h['secondary_coverage'] for h in regions['hypotheses']]})
        save_manifest()
        return document

    baseline = variants['clang-elf-O2']
    for name, target in variants.items():
        if '-elf-' in name:
            compare('same-source-' + name, baseline, target)
    compare('same-source-coff', variants['clang-coff-O2'], variants['clang-coff-O0'])
    saved = root / 'patched.nddiff'
    changed = compare('constant-patch', baseline, patched, ['--save', str(saved)])
    assert any('constants' in pair['changes'] for pair in changed['matches']), changed['matches']
    assert any(pair['similarity'] < 1 for pair in changed['matches'])
    reopened = compare('reopen-patch', baseline, patched, ['--reopen', str(saved)])
    assert reopened == changed
    run('incompatible-abi', [args.neverd, 'diff', '--structural', '-a', str(baseline),
                            '-b', str(variants['clang-coff-O2']), '--flow-regions', '--json'], (1,))
    # The optimized profile uses the ordinary machine LLVM optimizer. Keep the
    # original x64 Med corpus above intact; qualify AArch64 O0/O2 separately.
    profile = ['--dataflow-profile=llvm-ssa-v1']
    for fmt, triple in (('elf', 'aarch64-unknown-linux-gnu'),
                        ('coff', 'aarch64-pc-windows-msvc'),
                        ('macho', 'arm64-apple-macos11')):
        compiled = {}
        for level in ('0', '1', '2', '3', 's'):
            name = f'aarch64-{fmt}-O{level}'
            compiled[level] = build(name, args.clang,
                [f'--target={triple}', '-O' + level, '-fno-asynchronous-unwind-tables'])
        for level, target in compiled.items():
            doc = compare(f'optimized-{fmt}-O2-O{level}', compiled['2'], target, profile)
            assert doc['settings']['dataflow']['profile'] == 'llvm-ssa-v1'
            assert doc['flow_regions']['profile'] == 'llvm-neighborhoods-v1'
        patch = build(f'aarch64-{fmt}-patched', args.clang,
            [f'--target={triple}', '-O2', '-fno-asynchronous-unwind-tables', '-DFINAL_ADD=5u'])
        report = root / f'optimized-{fmt}.nddiff'
        changed = compare(f'optimized-{fmt}-patch', compiled['2'], patch,
                          [*profile, '--save', str(report)])
        assert any('constants' in pair['changes'] for pair in changed['matches'])
        reopened = compare(f'optimized-{fmt}-reopen', compiled['2'], patch,
                           ['--reopen', str(report)])
        assert reopened == changed
        crossed = compare(f'optimized-{fmt}-cross-level-patch', compiled['0'], patch, profile)
        # Native function pairing can abstain across O0/O2. Require the exact
        # changed scalar in both retained SSA graphs instead of inventing a
        # native alignment solely to label the patch.
        for side, value in (('primary', '0x3'), ('secondary', '0x5')):
            graph = crossed['dataflow'][side]['entries'][0]['graph']
            assert any(n['opcode'] == 'add' and any(i[0] == 1 and i[3] == value
                       for i in n['inputs']) for n in graph['nodes']), (fmt, side, graph)
    control = pathlib.Path(args.fixture).with_name('control_regions_fixture.c')
    fragments = [*profile, '--flow-fragments']
    branch_inputs = {}
    for fmt, triple in (('elf', 'aarch64-unknown-linux-gnu'),
                        ('coff', 'aarch64-pc-windows-msvc'),
                        ('macho', 'arm64-apple-macos11')):
        compiled = {}
        for level, value in ((0, 3), (2, 3), (2, 5)):
            name = f'branch-{fmt}-O{level}-{value}'
            compiled[level, value] = build(name, args.clang,
                [f'--target={triple}', f'-O{level}', '-fno-asynchronous-unwind-tables',
                 f'-DFINAL_ADD={value}u'], control)
        a, b, patch = compiled[0, 3], compiled[2, 3], compiled[2, 5]
        branch_inputs[fmt] = a
        report = root / f'fragments-{fmt}.nddiff'
        document = compare(f'fragments-{fmt}', a, b, [*fragments, '--save', str(report)])
        regions = document['flow_regions']
        assert regions['profile'] == 'llvm-fragments-v1'
        assert document['settings']['flow_regions']['fragments'] is True
        assert any(n['opcode'] == 'phi' for n in document['dataflow']['primary']['entries'][0]['graph']['nodes'])
        assert any(n['opcode'] == 'select' for n in document['dataflow']['secondary']['entries'][0]['graph']['nodes'])
        anchor = next(i for i, a in enumerate(regions['anchors']) if a['node_count'] >= 4 and a['frontier_count'])
        boundary = json.loads(run(f'fragments-{fmt}-frontier', [args.neverd, 'diff',
            '--structural', '-a', str(a), '-b', str(b), '--reopen', str(report),
            '--flow-region-anchor', str(anchor), '--flow-region-coordinates', 'frontier',
            '--json'], (0, 2)).stdout)
        assert boundary['view'] == 'frontier' and boundary['boundary_only']
        assert boundary['total'] == regions['anchors'][anchor]['frontier_count']
        assert boundary['rows']
        assert compare(f'fragments-{fmt}-reopen', a, b, ['--reopen', str(report)]) == document
        changed = compare(f'fragments-{fmt}-same-level-patch', b, patch, fragments)
        assert any('constants' in p['changes'] for p in changed['matches'])
        crossed = compare(f'fragments-{fmt}-cross-level-patch', a, patch, fragments, False)
        for side, value in (('primary', '0x3'), ('secondary', '0x5')):
            graph = crossed['dataflow'][side]['entries'][0]['graph']
            assert any(n['opcode'] == 'add' and any(i[0] == 1 and i[3] == value
                       for i in n['inputs']) for n in graph['nodes']), (fmt, side, graph)
    run('fragments-incompatible-abi', [args.neverd, 'diff', '--structural',
        '-a', str(branch_inputs['elf']), '-b', str(branch_inputs['coff']), *fragments, '--json'], (1,))
    # Keep legacy profiles and unresolved loop attribute cells observable.
    for arch, triple in (('x64', 'x86_64-unknown-linux-gnu'),
                         ('aarch64', 'aarch64-unknown-linux-gnu')):
        for family in (1, 2):
            paths = [build(f'control-{arch}-family{family}-O{level}', args.clang,
                     [f'--target={triple}', f'-O{level}', '-fno-asynchronous-unwind-tables',
                      f'-DFAMILY={family}'], control) for level in (0, 2)]
            compare(f'control-{arch}-family{family}', *paths, fragments, False)
    parameter_profile = ['--dataflow-profile=llvm-parameter-ssa-v1']
    deeper_options = root / 'parameter-deeper-options.json'
    deeper_options.write_text(json.dumps({'dataflow': {'profile': 'llvm-parameter-ssa-v1',
                                                       'graph': {'rounds': 16}}}))
    for fmt, triple in (('elf', 'x86_64-unknown-linux-gnu'),
                        ('coff', 'x86_64-pc-windows-msvc'),
                        ('macho', 'x86_64-apple-darwin')):
        def parameter_input(name, level, definitions=()):
            return build(f'parameter-{fmt}-{name}', args.clang,
                         [f'--target={triple}', f'-O{level}',
                          '-fno-asynchronous-unwind-tables', '-DFAMILY=1',
                          *definitions], control)
        a = parameter_input('O0', 0)
        b = parameter_input('O2', 2)
        patch = parameter_input('patch', 2, ['-DFINAL_ADD=5u'])
        branch = parameter_input('branch', 2, ['-DCHANGED_BRANCH'])
        high = parameter_input('high', 2, ['-DHIGH_INPUT'])
        report = root / f'parameter-{fmt}.nddiff'
        document = compare(f'parameter-{fmt}', a, b,
                           [*parameter_profile, '--save', str(report)])
        graphs = [document['dataflow'][side]['entries'][0]['graph']
                  for side in ('primary', 'secondary')]
        assert all(g['schema_version'] == 4 for g in graphs)
        assert any(v['source_bits'] == 64 and v['view_bits'] == 32 and
                   v['elided_truncations'] > 0 for v in graphs[1]['parameter_views'])
        assert len(graphs[0]['nodes']) == len(graphs[1]['nodes'])
        assert any(anchor['phi_block_count'] and anchor['node_count'] >= 4 and
                   min(anchor['primary_origins'], anchor['secondary_origins']) >= 2
                   for anchor in document['flow_regions']['anchors'])
        assert compare(f'parameter-{fmt}-reopen', a, b,
                       ['--reopen', str(report)]) == document
        changed = compare(f'parameter-{fmt}-patch', a, patch, parameter_profile)
        assert any('constants' in pair['changes'] for pair in changed['matches'])
        # The changed final add is not included in a correspondence, while the
        # genuinely shared prefix remains available for review.
        patched_graph = changed['dataflow']['secondary']['entries'][0]['graph']
        final_adds = {i for i, n in enumerate(patched_graph['nodes'])
                      if n['opcode'] == 'add' and
                      any(v[0] == 1 and v[3] == '0x5' for v in n['inputs'])}
        assert final_adds
        assert all(pair[1] not in final_adds for anchor in changed['flow_regions']['anchors']
                   for pair in anchor['nodes'])
        changed_branch = compare(f'parameter-{fmt}-branch', a, branch,
                                 parameter_profile, False)
        branch_graph = changed_branch['dataflow']['secondary']['entries'][0]['graph']
        changed_conditions = {i for i, n in enumerate(branch_graph['nodes'])
                              if n['opcode'] == 'icmp'}
        assert changed_conditions
        assert all(pair[1] not in changed_conditions
                   for anchor in changed_branch['flow_regions']['anchors']
                   for pair in anchor['nodes'])
        assert all(anchor['open_control_frontier'] and anchor['open_data_frontier']
                   for anchor in changed_branch['flow_regions']['anchors'])
        deeper = compare(f'parameter-{fmt}-branch-deeper', a, branch,
                         [*parameter_profile, '--options', str(deeper_options)], False)
        assert deeper['flow_regions']['anchor_count'] == 0
        changed_high = compare(f'parameter-{fmt}-high', a, high,
                               parameter_profile, False)
        high_graph = changed_high['dataflow']['secondary']['entries'][0]['graph']
        assert all(v['elided_truncations'] == 0 for v in high_graph['parameter_views'])
        assert changed_high['flow_regions']['anchor_count'] == 0
    progress_profile = ['--dataflow-profile=llvm-progress-ssa-v1', '--flow-fragments']
    for arch, targets in (
            ('x64', (('elf', 'x86_64-unknown-linux-gnu'),
                     ('coff', 'x86_64-pc-windows-msvc'),
                     ('macho', 'x86_64-apple-darwin'))),
            ('aarch64', (('elf', 'aarch64-unknown-linux-gnu'),
                         ('coff', 'aarch64-pc-windows-msvc'),
                         ('macho', 'arm64-apple-macos11')))):
        for fmt, triple in targets:
            prefix = f'progress-{arch}-{fmt}'

            def loop_input(name, level, definitions=()):
                return build(f'{prefix}-{name}', args.clang,
                             [f'--target={triple}', f'-O{level}',
                              '-fno-asynchronous-unwind-tables', '-DFAMILY=2',
                              *definitions], control)

            a, b = loop_input('O0', 0), loop_input('O2', 2)
            patch = loop_input('patch', 2, ['-DFINAL_ADD=5u'])
            bound = loop_input('bound', 2, ['-DLOOP_MASK=3u'])
            body = loop_input('body', 2, ['-DLOOP_MUL=11u'])
            report = root / f'{prefix}.nddiff'
            document = compare(prefix, a, b,
                               [*progress_profile, '--save', str(report)])
            graphs = [document['dataflow'][side]['entries'][0]['graph']
                      for side in ('primary', 'secondary')]
            assert all(g['schema_version'] == 5 for g in graphs)
            assert graphs[0]['progress']['kind'] == 'counter_loops'
            assert graphs[0]['progress']['loop_count'] == 1
            assert graphs[1]['progress']['kind'] == 'acyclic'
            assert 'willreturn' not in graphs[0]['progress']['source_contract']
            assert 'willreturn' in graphs[0]['progress']['observed_contract']
            regions = document['flow_regions']
            assert regions['profile'] == 'llvm-progress-fragments-v1'
            assert regions['semantic_validation'] == 'not_performed'
            assert any(anchor['node_count'] >= 4 and anchor['frontier_count']
                       for anchor in regions['anchors'])
            assert all(anchor['open_control_frontier'] for anchor in regions['anchors'])
            assert compare(prefix + '-reopen', a, b,
                           ['--reopen', str(report)]) == document
            changed = compare(prefix + '-patch', a, patch, progress_profile, False)
            patched = changed['dataflow']['secondary']['entries'][0]['graph']
            final_adds = {i for i, n in enumerate(patched['nodes'])
                          if n['opcode'] == 'add' and
                          any(v[0] == 1 and v[3] == '0x5' for v in n['inputs'])}
            assert final_adds
            assert all(pair[1] not in final_adds for anchor in changed['flow_regions']['anchors']
                       for pair in anchor['nodes'])
            changed_bound = compare(prefix + '-bound', a, bound, progress_profile, False)
            smaller = changed_bound['dataflow']['secondary']['entries'][0]['graph']
            assert any(n['opcode'] == 'and' and any(v[0] == 1 and v[3] == '0x3'
                       for v in n['inputs']) for n in smaller['nodes'])
            assert len(smaller['nodes']) != len(graphs[1]['nodes'])
            assert all(anchor['open_control_frontier']
                       for anchor in changed_bound['flow_regions']['anchors'])
            changed_body = compare(prefix + '-body', a, body, progress_profile, False)
            altered = changed_body['dataflow']['secondary']['entries'][0]['graph']
            multiplications = {i for i, n in enumerate(altered['nodes'])
                               if n['opcode'] == 'mul' and
                               any(v[0] == 1 and v[3] == '0xb' for v in n['inputs'])}
            assert multiplications
            assert all(pair[1] not in multiplications
                       for anchor in changed_body['flow_regions']['anchors']
                       for pair in anchor['nodes'])
    manifest['status'] = 'passed'
    save_manifest()
    print(f"{len(manifest['comparisons'])} compiler/patch fixture comparisons; artifacts: {root}")
    for skipped in manifest['skipped']:
        print('SKIP:', skipped)


if __name__ == '__main__':
    main()
