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
assert.match(html, /id="capacity-note"/);
assert.match(html, /id="download-name"[^>]*value="decoded\.xlsx"/);
assert.match(html, /id="status"[^>]*aria-live="polite"/);
assert.match(html, /id="cancel-button"/);
assert.match(html, /id="app-logic"/);
assert.doesNotMatch(html, /<(?:script|link)[^>]+(?:src|href)=["']https?:/i);
// 离线单文件适配: file:// 打开时隐藏返回首页链接
assert.match(html, /id="back-link"/);
assert.match(html, /location\.protocol\s*===\s*'file:'[\s\S]*?hidden\s*=\s*true/);
// 离线/旧版浏览器粘贴兼容: 不能只依赖 input 事件
assert.match(html, /decodeInput\.addEventListener\('paste'/);
assert.match(html, /decodeInput\.addEventListener\('change'/);
assert.match(html, /decodeInput\.addEventListener\('propertychange'/);
assert.match(html, /jsHeapSizeLimit/);
assert.doesNotMatch(html, /file\.size\s*>\s*50\s*\*\s*1024\s*\*\s*1024/);
assert.match(html, /file\.slice\(/);

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
  const largeBase64 = 'A'.repeat(5 * 1024 * 1024);
  assert.doesNotThrow(() => core.validateBase64(largeBase64));
  assert.strictEqual(core.validateBase64(largeBase64).valid, true);
  const largeRestored = await core.base64ToBytes(largeBase64);
  assert.strictEqual(largeRestored.length, 5 * 1024 * 1024 / 4 * 3);
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
