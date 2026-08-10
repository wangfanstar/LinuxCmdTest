'use strict';

const assert = require('assert');
const fs = require('fs');
const vm = require('vm');

const htmlPath = 'html/file2base64.html';
assert.ok(fs.existsSync(htmlPath), 'html/file2base64.html must exist');

const html = fs.readFileSync(htmlPath, 'utf8');
assert.match(html, /id="mode-encode"[^>]*aria-selected="true"/);
assert.match(html, /id="mode-decode"[^>]*aria-selected="false"/);
assert.match(html, /id="drop-zone"/);
assert.match(html, /id="base64-output"[^>]*readonly/);
assert.match(html, /id="copy-button"/);
assert.match(html, /id="decode-input"/);
assert.match(html, /id="download-name"[^>]*value="decoded\.xlsx"/);
assert.match(html, /id="status"[^>]*aria-live="polite"/);
assert.match(html, /id="cancel-button"/);
assert.match(html, /id="app-logic"/);
assert.doesNotMatch(html, /<(?:script|link)[^>]+(?:src|href)=["']https?:/i);

const match = html.match(/<script id="core-logic">([\s\S]*?)<\/script>/);
assert.ok(match, 'core-logic script must exist');

const context = {
  window: {},
  Uint8Array,
  Blob,
  btoa: value => Buffer.from(value, 'binary').toString('base64'),
  atob: value => Buffer.from(value, 'base64').toString('binary'),
  setTimeout,
  clearTimeout
};
context.window.window = context.window;
vm.createContext(context);
vm.runInContext(match[1], context, { filename: htmlPath });

const core = context.window.FileBase64Core;

(async () => {
  const bytes = Uint8Array.from({ length: 65537 }, (_, index) => index % 256);
  const encoded = await core.bytesToBase64(bytes);
  const restored = await core.base64ToBytes(encoded);

  assert.deepStrictEqual(Array.from(restored), Array.from(bytes));
  assert.strictEqual(await core.bytesToBase64(new Uint8Array()), '');
  assert.strictEqual(core.normalizeBase64(' YW Jj\nZA==\t'), 'YWJjZA==');
  assert.strictEqual(core.validateBase64('YWJjZA==').valid, true);
  assert.strictEqual(core.validateBase64('').valid, false);
  assert.strictEqual(core.validateBase64('abc').valid, false);
  assert.strictEqual(core.validateBase64('ab=c').valid, false);
  assert.strictEqual(core.validateBase64('****').valid, false);
  assert.strictEqual(core.safeDownloadName('  report.xlsx  '), 'report.xlsx');
  assert.strictEqual(core.safeDownloadName('../'), 'decoded.xlsx');
  assert.strictEqual(core.formatBytes(0), '0 B');
  assert.strictEqual(core.formatBytes(1536), '1.5 KB');

  const job = { cancelled: true };
  await assert.rejects(
    () => core.bytesToBase64(Uint8Array.of(1, 2, 3), null, job),
    error => error && error.code === 'CONVERSION_CANCELLED'
  );

  console.log('file2base64 core tests passed');
})().catch(error => {
  console.error(error);
  process.exit(1);
});
