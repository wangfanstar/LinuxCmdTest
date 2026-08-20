#!/usr/bin/env bash
set -euo pipefail

node <<'NODE'
const fs = require('fs');
const vm = require('vm');

const html = fs.readFileSync('html/extract.html', 'utf8');
const scriptMatch = html.match(/<script>([\s\S]*?)<\/script>/);
if (!scriptMatch) {
  throw new Error('extract.html script block not found');
}

function makeClassList(initial = []) {
  const classes = new Set(initial);
  return {
    add(name) { classes.add(name); },
    remove(name) { classes.delete(name); },
    contains(name) { return classes.has(name); },
    toggle(name, force) {
      const shouldAdd = force === undefined ? !classes.has(name) : Boolean(force);
      if (shouldAdd) classes.add(name);
      else classes.delete(name);
      return shouldAdd;
    }
  };
}

const elements = new Map();
function element(id) {
  if (!elements.has(id)) {
    elements.set(id, {
      id,
      value: '',
      checked: false,
      textContent: '',
      className: '',
      innerHTML: '',
      style: {},
      files: [],
      classList: makeClassList(id === 'exclude-body' ? ['hidden'] : []),
      addEventListener() {},
      appendChild() {},
      click() {}
    });
  }
  return elements.get(id);
}

const context = {
  console,
  setTimeout,
  URL: {
    createObjectURL() { return 'blob:test'; },
    revokeObjectURL() {}
  },
  Blob: function Blob() {},
  FileReader: function FileReader() {},
  fetch: async () => { throw new Error('fetch not expected in this test'); },
  document: {
    getElementById: element,
    createElement: tag => ({
      tagName: tag,
      style: {},
      classList: makeClassList(),
      addEventListener() {},
      appendChild() {},
      click() {}
    })
  },
  window: { open() {} }
};
context.globalThis = context;
vm.createContext(context);
vm.runInContext(scriptMatch[1], context, { filename: 'html/extract.html' });

vm.runInContext(`
{
  fileMap.set('sample.log', { text: 'keep alpha\\nskip DEBUG\\nkeep beta' });
  document.getElementById('exclude-input').value = 'DEBUG';
  doSearch();
  const output = document.getElementById('output').value;
  if (output !== 'keep alpha\\nkeep beta') {
    throw new Error('exclude-only output mismatch: ' + JSON.stringify(output));
  }
  const resultCount = document.getElementById('result-count').textContent;
  if (resultCount !== '共匹配 2 行') {
    throw new Error('exclude-only count mismatch: ' + JSON.stringify(resultCount));
  }
}
`, context);

vm.runInContext(`
{
  fileMap.clear();
  patterns.length = 0;
  excludes.length = 0;
  renderPatternTags();
  renderExcludeTags();
  document.getElementById('search-input').value = 'match';
  document.getElementById('exclude-input').value = '';
  document.getElementById('output').value = '';
  document.getElementById('result-count').textContent = '';
  const largeText = Array.from({ length: 200000 }, (_, i) => 'match line ' + i).join('\\n');
  fileMap.set('large.log', { text: largeText });
  doSearch();
  const resultCount = document.getElementById('result-count').textContent;
  if (resultCount !== '共匹配 200000 行') {
    throw new Error('large extraction count mismatch: ' + JSON.stringify(resultCount));
  }
}
`, context);
NODE
