#!/usr/bin/env bash
set -euo pipefail

node <<'NODE'
const fs = require('fs');
const vm = require('vm');
const { TextDecoder, TextEncoder } = require('util');

const html = fs.readFileSync('html/paste-web-tool.html', 'utf8');
const coreMatch = html.match(/<script id="core-logic">([\s\S]*?)<\/script>/);
const scripts = [...html.matchAll(/<script(?:\s+id="[^"]+")?>([\s\S]*?)<\/script>/g)].map((m) => m[1]);
if (!coreMatch || scripts.length < 2) {
  throw new Error('paste-web-tool scripts not found');
}

function makeClassList() {
  const classes = new Set();
  return {
    add(name) { classes.add(name); },
    remove(name) { classes.delete(name); },
    contains(name) { return classes.has(name); },
    toggle(name, force) {
      const enabled = force === undefined ? !classes.has(name) : Boolean(force);
      if (enabled) classes.add(name);
      else classes.delete(name);
      return enabled;
    }
  };
}

function makeElement(id) {
  const listeners = new Map();
  return {
    id,
    value: '',
    textContent: '',
    className: '',
    innerHTML: '',
    checked: false,
    hidden: false,
    disabled: false,
    title: '',
    dataset: {},
    style: {},
    files: [],
    classList: makeClassList(),
    setAttribute(name, value) { this[name] = String(value); },
    appendChild() {},
    remove() {},
    select() {},
    click() {
      const callbacks = listeners.get('click') || [];
      callbacks.forEach((callback) => callback({ target: this, currentTarget: this }));
    },
    addEventListener(type, callback) {
      if (!listeners.has(type)) listeners.set(type, []);
      listeners.get(type).push(callback);
    },
    querySelector() { return null; },
    querySelectorAll() { return []; },
    closest() { return null; }
  };
}

function makeContext(options = {}) {
  const elements = new Map();
  const windowListeners = new Map();
  const localStorageData = new Map();

  function element(id) {
    if (!elements.has(id)) elements.set(id, makeElement(id));
    return elements.get(id);
  }

  const context = {
    console,
    setTimeout,
    clearTimeout,
    URL,
    URLSearchParams,
    TextEncoder,
    TextDecoder,
    Blob,
    btoa: (value) => Buffer.from(value, 'binary').toString('base64'),
    atob: (value) => Buffer.from(value, 'base64').toString('binary'),
    crypto: { randomUUID: () => `id-${Math.random().toString(36).slice(2)}` },
    location: { protocol: 'file:', origin: 'null' },
    navigator: { clipboard: null },
    confirm: () => true,
    prompt: () => '',
    alert: () => {},
    document: {
      getElementById: element,
      createElement: (tag) => makeElement(tag),
      body: makeElement('body'),
      execCommand: () => true
    },
    localStorage: {
      getItem(key) { return localStorageData.has(key) ? localStorageData.get(key) : null; },
      setItem(key, value) { localStorageData.set(key, String(value)); },
      removeItem(key) { localStorageData.delete(key); }
    },
    window: {
      addEventListener(type, callback) {
        if (!windowListeners.has(type)) windowListeners.set(type, []);
        windowListeners.get(type).push(callback);
      },
      dispatchMessage(data, origin = 'https://paste.centos.org') {
        const callbacks = windowListeners.get('message') || [];
        callbacks.forEach((callback) => callback({ data, origin }));
      },
      open: () => null
    },
    fetch: options.fetch
  };
  context.window.window = context.window;
  context.window.document = context.document;
  context.window.localStorage = context.localStorage;
  context.window.location = context.location;
  context.window.navigator = context.navigator;
  context.window.URL = URL;
  context.window.URLSearchParams = URLSearchParams;
  context.window.Blob = Blob;
  context.window.TextEncoder = TextEncoder;
  context.window.TextDecoder = TextDecoder;
  context.globalThis = context;
  return { context, element, localStorageData };
}

async function testPasteFetchShortCircuitsCors() {
  let fetchCalls = 0;
  const { context } = makeContext({
    fetch: async () => {
      fetchCalls += 1;
      throw new Error('fetch should not be called');
    }
  });
  vm.createContext(context);
  vm.runInContext(coreMatch[1], context, { filename: 'html/paste-web-tool.html' });

  const result = await context.window.PasteUbuntuCore.fetchPasteContent('https://paste.centos.org/view/raw/d1f50de3');
  if (fetchCalls !== 0) {
    throw new Error(`expected direct paste fetch to short-circuit, got ${fetchCalls} fetch call(s)`);
  }
  if (result.ok || !/CORS|采集/.test(result.error)) {
    throw new Error(`expected manual-collect CORS error, got ${JSON.stringify(result)}`);
  }
}

async function testLargeCollectedContentIsNotPersisted() {
  const { context, element, localStorageData } = makeContext({ fetch: async () => { throw new Error('fetch not expected'); } });
  vm.createContext(context);
  scripts.forEach((script, index) => vm.runInContext(script, context, { filename: `html/paste-web-tool.html#script${index}` }));

  element('importText').value = 'file_path,paste_url\nlarge.txt,https://paste.centos.org/view/raw/d1f50de3\n';
  element('importTextBtn').click();
  await Promise.resolve();
  const imported = JSON.parse(localStorageData.get('pasteUbuntuUnifiedTool.v1') || '{}');
  const recordId = imported.downloads && imported.downloads[0] && imported.downloads[0].id;
  if (!recordId) {
    throw new Error('download record id was not persisted after import');
  }

  const largeContent = 'x'.repeat(21 * 1024 * 1024);
  context.window.dispatchMessage({
    source: 'paste-web-tool',
    type: 'paste-content',
    id: recordId,
    content: largeContent
  });

  const savedState = localStorageData.get('pasteUbuntuUnifiedTool.v1') || '';
  if (savedState.includes(largeContent.slice(0, 1024))) {
    throw new Error('large recovered content was persisted into localStorage');
  }
  const parsed = JSON.parse(savedState);
  if (!parsed.downloads || parsed.downloads[0].status !== 'collected') {
    throw new Error(`download status was not persisted: ${savedState.slice(0, 500)}`);
  }
}

(async () => {
  await testPasteFetchShortCircuitsCors();
  await testLargeCollectedContentIsNotPersisted();
})();
NODE
