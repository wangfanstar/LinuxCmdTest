#!/usr/bin/env bash
set -euo pipefail

node <<'NODE'
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');

const html = fs.readFileSync('html/wiki/notewiki.html', 'utf8');

function extractFunction(source, name) {
  const marker = `function ${name}`;
  const start = source.indexOf(marker);
  if (start < 0) throw new Error(`function ${name} not found`);
  const open = source.indexOf('{', start);
  let depth = 0;
  let quote = '';
  let escaped = false;
  let lineComment = false;
  let blockComment = false;
  for (let i = open; i < source.length; i += 1) {
    const ch = source[i];
    const next = source[i + 1];
    if (lineComment) {
      if (ch === '\n') lineComment = false;
      continue;
    }
    if (blockComment) {
      if (ch === '*' && next === '/') {
        blockComment = false;
        i += 1;
      }
      continue;
    }
    if (quote) {
      if (escaped) escaped = false;
      else if (ch === '\\') escaped = true;
      else if (ch === quote) quote = '';
      continue;
    }
    if (ch === '/' && next === '/') {
      lineComment = true;
      i += 1;
      continue;
    }
    if (ch === '/' && next === '*') {
      blockComment = true;
      i += 1;
      continue;
    }
    if (ch === '\'' || ch === '"' || ch === '`') {
      quote = ch;
      continue;
    }
    if (ch === '{') depth += 1;
    if (ch === '}' && --depth === 0) return source.slice(start, i + 1);
  }
  throw new Error(`unterminated function ${name}`);
}

function makeElement(id) {
  return {
    id,
    value: '',
    disabled: false,
    style: {},
    textContent: '',
    focus() {},
    classList: { add() {}, remove() {}, toggle() {} }
  };
}

const elements = new Map();
const element = id => {
  if (!elements.has(id)) elements.set(id, makeElement(id));
  return elements.get(id);
};

const saveRequests = [];
const context = {
  console,
  Date,
  setTimeout(fn) { fn(); return 1; },
  clearTimeout() {},
  document: { getElementById: element },
  localStorage: { setItem() {}, getItem() { return null; }, removeItem() {} },
  S: {
    articles: [
      { id: 'article-a', title: 'Article A', category: '' },
      { id: 'article-b', title: 'Article B', category: '' }
    ],
    editId: null,
    editIsNew: false,
    editEngine: 'markdown',
    editBaseUpdated: '',
    editBaseContent: ''
  },
  Auth: { loggedIn: true, user: { role: 'author' } },
  _editLoadSeq: 0,
  _editBaseline: '',
  _editDirty: false,
  _autoSaveDirty: false,
  ensureWriteAuth: () => true,
  isCurrentEditLoad: () => true,
  isCurrentEditContext: () => true,
  setEditorLoadingState(loading) { element('edit-textarea').disabled = !!loading; },
  refreshEditDirtyState() {},
  populateCatSelect() {},
  showPane() {},
  setViewMode() {},
  applyWikiEditEngine() {},
  refreshVditorFromTextarea() {},
  updatePreview() {},
  updateEditorHighlight() {},
  histReset() {},
  markEditBaseline() {},
  updateEditHtmlLink() {},
  saveEditBackup() {},
  loadEditBackup() { return null; },
  updateAutoSaveLabel() {},
  refreshAll() {},
  clearEditBackup() {},
  openConflictModal() {},
  setStatus(message) { element('status-msg').textContent = message; },
  md2html(content) { return content; },
  fetch(url, options) {
    if (options && url.includes('/api/wiki-save')) saveRequests.push({ url, options });
    throw new Error(`unexpected fetch: ${url}`);
  }
};
context.globalThis = context;
vm.createContext(context);

const optionalFunctions = [
  'isCurrentEditLoad',
  'isCurrentEditContext',
  'setEditorLoadingState',
  'setEditDirty',
  'getVditorValue',
  'syncVditorToTextarea',
  'loadEditorFromArticleId',
  '_doSave'
];
const functions = optionalFunctions
  .filter(name => html.includes(`function ${name}`))
  .map(name => extractFunction(html, name))
  .join('\n');
vm.runInContext(functions, context, { filename: 'html/wiki/notewiki.html' });

async function flushPromises() {
  await new Promise(resolve => setImmediate(resolve));
  await new Promise(resolve => setImmediate(resolve));
}

function deferred() {
  let resolve;
  const promise = new Promise(done => { resolve = done; });
  return { promise, resolve };
}

(async () => {
  context.S.editId = 'article-a';
  context.S.editIsNew = false;
  context.S.editEngine = 'vditor';
  context._contentLoaded = false;
  context._vditor = { getValue: () => '' };
  element('edit-textarea').value = '加载中…';
  context._doSave({});
  assert.equal(element('edit-textarea').value, '加载中…', 'loading content must not be replaced by Vditor');
  assert.equal(saveRequests.length, 0, 'save must not start before article content is loaded');

  const firstRead = deferred();
  const secondRead = deferred();
  context.fetch = url => url.includes('article-a') ? firstRead.promise : secondRead.promise;
  context.loadEditorFromArticleId('article-a');
  context.loadEditorFromArticleId('article-b');
  secondRead.resolve({ ok: true, json: async () => ({ ok: true, content: 'B' }) });
  await flushPromises();
  firstRead.resolve({ ok: true, json: async () => ({ ok: true, content: 'A' }) });
  await flushPromises();
  assert.equal(element('edit-textarea').value, 'B', 'stale article response must not overwrite current editor');

  saveRequests.length = 0;
  context.S.editEngine = 'markdown';
  context.fetch = (url, options) => {
    if (options && url.includes('/api/wiki-save')) {
      saveRequests.push({ url, options });
      return Promise.resolve({ status: 200, json: async () => ({ ok: true, id: 'article-a' }) });
    }
    return Promise.resolve({ ok: true, json: async () => ({ ok: true, content: 'server content' }) });
  };
  context.loadEditorFromArticleId('article-a');
  await flushPromises();
  context._doSave({});
  await flushPromises();
  assert.equal(saveRequests.length, 1, 'loaded article should be saved once');
  assert.equal(JSON.parse(saveRequests[0].options.body).content, 'server content', 'save must submit loaded article content');

  console.log('NoteWiki edit flow regression tests passed');
})().catch(error => {
  console.error(error.stack || error);
  process.exitCode = 1;
});
NODE
