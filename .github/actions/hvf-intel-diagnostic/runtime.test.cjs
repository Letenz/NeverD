'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const {spawnSync} = require('node:child_process');
const {nativeUuid, captureRuntime} = require('./runtime.cjs');

test('Mach-O identity selects the actual native slice', () => {
  const arm = '12345678-1234-1234-1234-1234567890AB';
  const intel = 'ABCDEF01-2345-6789-ABCD-EF0123456789';
  const fat = `UUID: ${arm} (arm64) /node\nUUID: ${intel} (x86_64) /node\n`;
  assert.equal(nativeUuid(fat, 'x64'), intel.toLowerCase());
  assert.equal(nativeUuid(fat, 'arm64'), arm.toLowerCase());
  assert.throws(() => nativeUuid(fat, 'ia32'));
  assert.throws(() => nativeUuid(`UUID: ${arm} (arm64) /node\n`, 'x64'));
  assert.throws(() => nativeUuid(fat + fat, 'x64'));
  assert.throws(() => nativeUuid('UUID: BAD (x86_64) /node\n', 'x64'));
});

test('actual running Node identity agrees with an independent digest',
  {skip: process.platform !== 'darwin'}, () => {
    const record = captureRuntime();
    const digest = spawnSync('/usr/bin/shasum', ['-a', '256', process.execPath],
      {encoding: 'utf8', timeout: 10000});
    assert.equal(digest.status, 0);
    assert.equal(record.node_sha256, digest.stdout.split(/\s/)[0]);
    assert.equal(record.node_version, process.version);
    assert.equal(record.v8_version, process.versions.v8);
    assert.equal(record.host_architecture, process.arch);
    assert.ok(record.node_bytes > 0);
  });
