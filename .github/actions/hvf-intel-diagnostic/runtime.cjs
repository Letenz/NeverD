'use strict';
const fs = require('node:fs');
const {createHash} = require('node:crypto');
const {spawnSync} = require('node:child_process');

function nativeUuid(output, architecture) {
  const slice = {x64: 'x86_64', arm64: 'arm64'}[architecture];
  if (!slice) throw new Error('unsupported observer architecture');
  const records = [...output.matchAll(/^UUID: ([0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}) \(([^)]+)\) /gim)];
  const matching = records.filter(record => record[2] === slice);
  if (matching.length !== 1) throw new Error('observer native Mach-O UUID unavailable or ambiguous');
  return matching[0][1].toLowerCase();
}

function captureRuntime() {
  if (process.platform !== 'darwin') throw new Error('HVF observer requires macOS');
  // Upload children use this exact process.execPath. Capture it before the
  // first guest executes; a later crash report alone cannot prove byte identity.
  const executable = fs.realpathSync(process.execPath);
  const before = fs.statSync(executable, {bigint: true});
  const bytes = fs.readFileSync(executable);
  const macho = spawnSync('/usr/bin/dwarfdump', ['--uuid', executable],
    {encoding: 'utf8', timeout: 10000});
  if (macho.status !== 0 || macho.signal || macho.error)
    throw new Error('cannot inspect the observer executable');
  const after = fs.statSync(executable, {bigint: true});
  if (['dev', 'ino', 'size', 'mtimeNs', 'ctimeNs'].some(key => before[key] !== after[key]) ||
      BigInt(bytes.length) !== after.size)
    throw new Error('observer executable changed during inspection');
  return {kind: 'actual-hvf-observer-runtime', node_executable: process.execPath,
    node_realpath: executable, node_version: process.version, v8_version: process.versions.v8,
    host_system: process.platform, host_architecture: process.arch,
    node_native_uuid: nativeUuid(macho.stdout, process.arch),
    node_sha256: createHash('sha256').update(bytes).digest('hex'), node_bytes: bytes.length};
}

module.exports = {nativeUuid, captureRuntime};
