'use strict';

const assert = require('assert');
const fs = require('fs');
const vm = require('vm');

const html = fs.readFileSync('html/file2base64.html', 'utf8');

function makeElement(id) {
  const element = {
    id,
    value: '',
    textContent: '',
    className: '',
    hidden: false,
    disabled: false,
    tabIndex: 0,
    listeners: {},
    attributes: {},
    addEventListener(type, listener) {
      (this.listeners[type] ||= []).push(listener);
    },
    dispatch(type, event = {}) {
      (this.listeners[type] || []).forEach(listener => listener({
        preventDefault() {},
        stopPropagation() {},
        ...event
      }));
    },
    setAttribute(name, value) { this.attributes[name] = String(value); },
    removeAttribute(name) { delete this.attributes[name]; },
    hasAttribute(name) { return Object.prototype.hasOwnProperty.call(this.attributes, name); },
    focus() {},
    select() {},
    click() { this.dispatch('click'); }
  };
  element.classList = {
    add(name) { element.className = `${element.className} ${name}`.trim(); },
    remove(name) { element.className = element.className.split(' ').filter(item => item !== name).join(' '); }
  };
  return element;
}

const elements = new Map();
const document = {
  getElementById(id) {
    if (!elements.has(id)) elements.set(id, makeElement(id));
    return elements.get(id);
  },
  addEventListener() {},
  createElement: makeElement,
  body: { appendChild() {} },
  execCommand: () => true
};

const window = { location: { protocol: 'file:' } };
window.window = window;

function FakeFileReader() {
  this.readyState = 0;
  this.result = null;
  this.onload = null;
  this.onprogress = null;
}
FakeFileReader.LOADING = 1;
FakeFileReader.prototype.readAsArrayBuffer = function (blob) {
  this.readyState = FakeFileReader.LOADING;
  this.result = new ArrayBuffer(blob.size);
  new Uint8Array(this.result).fill(65);
  if (this.onprogress) this.onprogress({ lengthComputable: true, loaded: blob.size, total: blob.size });
  this.readyState = 2;
  setTimeout(() => { if (this.onload) this.onload(); }, 0);
};

const context = {
  window,
  document,
  navigator: {},
  Uint8Array,
  Blob,
  performance: { memory: { jsHeapSizeLimit: 4 * (1024 ** 3) } },
  btoa: value => Buffer.from(value, 'binary').toString('base64'),
  atob: value => Buffer.from(value, 'base64').toString('binary'),
  setTimeout,
  clearTimeout,
  URL: { createObjectURL: () => 'blob:test', revokeObjectURL() {} },
  FileReader: FakeFileReader,
  console
};

vm.createContext(context);
for (const id of ['core-logic', 'app-logic']) {
  const match = html.match(new RegExp(`<script id="${id}">([\\s\\S]*?)<\\/script>`));
  assert.ok(match, `${id} script must exist`);
  vm.runInContext(match[1], context, { filename: 'html/file2base64.html' });
}

(async () => {
  const decodeTab = document.getElementById('mode-decode');
  const input = document.getElementById('decode-input');
  decodeTab.dispatch('click');

  input.value = 'YWJjZA==';
  input.dispatch('paste');
  await new Promise(resolve => setTimeout(resolve, 10));

  assert.strictEqual(document.getElementById('decode-count').textContent, '8 字符');
  assert.strictEqual(document.getElementById('decode-validation').textContent, '格式有效，可以生成文件');
  assert.strictEqual(document.getElementById('download-file-button').disabled, false);
  assert.strictEqual(document.getElementById('back-link').hidden, true);
  assert.match(document.getElementById('capacity-note').textContent, /文件转 Base64 建议不超过/);
  assert.match(document.getElementById('capacity-note').textContent, /Base64 转文件建议原文件不超过/);

  input.value = 'A'.repeat(5 * 1024 * 1024);
  input.dispatch('paste');
  await new Promise(resolve => setTimeout(resolve, 10));
  assert.strictEqual(document.getElementById('decode-count').textContent, '5,242,880 字符');
  assert.strictEqual(document.getElementById('download-file-button').disabled, false);

  document.getElementById('mode-encode').dispatch('click');
  const fileSize = 3 * 1024 * 1024 + 123;
  const fileInput = document.getElementById('file-input');
  fileInput.files = [{
    name: 'sample.bin',
    size: fileSize,
    slice(start, end) { return { size: end - start }; }
  }];
  fileInput.dispatch('change');
  await new Promise(resolve => setTimeout(resolve, 1000));
  assert.strictEqual(document.getElementById('base64-output').hasAttribute('data-has-result'), true);
  assert.strictEqual(document.getElementById('base64-output').value.length, Math.ceil(fileSize / 3) * 4);
  console.log('file2base64 offline paste UI test passed');
})().catch(error => {
  console.error(error);
  process.exit(1);
});
