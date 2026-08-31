'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
const htmlPath = path.resolve(__dirname, '../html/HtmlPasteGen.html');
const html = fs.readFileSync(htmlPath, 'utf8');
const context = { window: {} };
vm.createContext(context);
vm.runInContext(html.match(/<script id="core-logic">([\s\S]*?)<\/script>/)[1], context);
const core = context.window.HtmlPasteGenCore;
const plain = value => JSON.parse(JSON.stringify(value));
const project = core.createSampleDocument();
const first = project.groups[0].items[0];
const second = project.groups[1].items[0];
first.children.push(core.createItem({ title: '嵌套检查', content: 'echo nested' }));
const nested = first.children[0];

// Cross-group/tree selections retain click order without including unselected descendants.
const composition = core.compositionFromSelection(project, [second.id, nested.id, first.id, second.id]);
assert.deepEqual(plain(composition.parts.map(part => part.sourceId)), [second.id, nested.id, first.id]);
assert.equal(core.compositionText(composition), [second.content, nested.content, first.content].join('\n'));
assert.throws(() => core.compositionFromSelection(project, ['missing']), /不存在/);

const result = core.saveComposition(project, '', '  组合测试  ', composition);
assert.equal(result.group.title, '自定义组合');
assert.equal(result.item.title, '组合测试');
assert.equal(result.item.shortcut, '');
assert.equal(result.item.composition.parts.length, 3);
const savedText = result.item.content;
first.content = 'source changed';
project.groups[1].items.splice(0, 1);
composition.parts[0].content = 'draft changed';
assert.equal(result.item.content, savedText, 'source/draft changes cannot mutate saved commands');
assert.equal(core.compositionText(result.item.composition), savedText);

// Editing/adding/removing/reordering steps updates only the chosen combination.
const edited = core.normalizeComposition(result.item.composition);
edited.parts.splice(0, 1);
edited.parts.push({ id: 'custom', title: '收尾', content: 'echo done', sourceId: '' });
core.moveItem(edited.parts, 2, -1);
edited.separator = ' && ';
result.item.note = '保留备注';
result.item.shortcut = 'Alt+9';
core.saveComposition(project, result.item.id, '修改组合', edited);
assert.equal(result.item.content, 'echo nested && echo done && ss -lntp');
assert.equal(result.item.note, '保留备注');
assert.equal(result.item.shortcut, 'Alt+9');
assert.equal(result.group.items.length, 1);
const beforeInvalid = JSON.stringify(project);
assert.throws(() => core.saveComposition(project, result.item.id, '', edited), /组合名称/);
assert.throws(() => core.saveComposition(project, '', '空', { parts: [] }), /有内容/);
assert.throws(() => core.saveComposition(project, 'missing', '不存在', edited), /不存在/);
assert.equal(JSON.stringify(project), beforeInvalid, 'invalid saves are atomic');

const duplicate = core.cloneItemTree(result.item);
duplicate.composition.parts[0].content = 'independent clone';
assert.equal(result.item.composition.parts[0].content, 'echo nested');
assert.equal(core.compositionText({ parts: [{ content: '  a\n' }, { content: '\tb  ' }], separator: '' }), '  a\n\tb  ');

const roundtrip = core.normalizeDocument(core.migrateDocument(plain(project)));
assert.deepEqual(plain(core.findItemLocation(roundtrip, result.item.id).item), plain(result.item));
const generated = core.buildGeneratedHtml(project);
const embedded = JSON.parse(core.extractEmbeddedDocument(generated));
assert.deepEqual(core.findItemLocation(embedded, result.item.id).item.composition, plain(result.item.composition));
const script = generated.match(/<script id="generated-app-logic">([\s\S]*?)<\/script>/)[1];
assert.doesNotThrow(() => new vm.Script(script));
assert.doesNotThrow(() => new vm.Script(html.match(/<script id="app-logic">([\s\S]*?)<\/script>/)[1]));

// Run the actual standalone import helpers, stopping before DOM event bindings.
const bindingsStart = "    byId('generated-search').addEventListener";
assert.ok(script.includes(bindingsStart));
const generatedContext = {
  window: {},
  document: { getElementById: id => id === 'paste-data' ? { textContent: JSON.stringify(embedded) } : null },
  localStorage: { getItem: () => null }
};
vm.createContext(generatedContext);
vm.runInContext(script.replace(bindingsStart,
  '    window.importHelpers = { normalizeImportedDocument, setItemContent }; return;\n' + bindingsStart), generatedContext);
const generatedImported = generatedContext.window.importHelpers.normalizeImportedDocument(plain(roundtrip));
assert.deepEqual(plain(core.findItemLocation(generatedImported, result.item.id).item.composition), plain(result.item.composition));
const generatedEditedItem = core.findItemLocation(generatedImported, result.item.id).item;
generatedContext.window.importHelpers.setItemContent(generatedEditedItem, 'standalone edit');
const returnedToEditor = core.normalizeDocument(plain(generatedImported));
assert.equal(core.compositionText(core.findItemLocation(returnedToEditor, result.item.id).item.composition), 'standalone edit');

core.setItemContent(result.item, 'edited in generated page');
assert.equal(result.item.composition.parts.length, 1);
assert.equal(core.compositionText(result.item.composition), result.item.content);
const location = core.findItemLocation(project, result.item.id);
location.siblings.splice(location.index, 1);
assert.equal(core.findItemLocation(project, result.item.id), null);
assert.equal(core.findItemLocation(project, first.id).item.content, 'source changed');
const legacy = { schemaVersion: 1, meta: { title: '旧项目' }, groups: [{ title: '旧组', items: [{ title: '旧命令', content: 'echo legacy' }] }] };
assert.equal(core.normalizeDocument(core.migrateDocument(legacy)).groups[0].items[0].content, 'echo legacy');

// Command text stays data even when it contains HTML/script delimiters.
const hostile = core.createSampleDocument();
core.saveComposition(hostile, '', '安全文本', { parts: [{ title: '</script>', content: '</script><script>alert(1)</script>\u2028' }] });
const safeGenerated = core.buildGeneratedHtml(hostile);
assert.equal((safeGenerated.match(/<script>/g) || []).length, 0);
assert.equal(core.findItemLocation(JSON.parse(core.extractEmbeddedDocument(safeGenerated)), hostile.groups.at(-1).items[0].id).item.content, '</script><script>alert(1)</script>\u2028');
console.log('HtmlPasteGen composition behavior tests passed');

// Optional isolated, loopback-only UI fixture. No writes and no user draft origin.
if (process.argv.includes('--serve')) {
  const fixture = core.createSampleDocument();
  core.saveComposition(fixture, '', '成品组合验收', { parts: [
    { title: '步骤一', content: 'echo first' }, { title: '步骤二', content: 'echo second' }
  ] });
  const http = require('node:http');
  const server = http.createServer((req, res) => {
    const pathname = new URL(req.url, 'http://localhost').pathname;
    res.setHeader('Cache-Control', 'no-store');
    if (pathname === '/api/html-paste/list') {
      res.setHeader('Content-Type', 'application/json');
      return res.end(JSON.stringify({ ok: true, files: [] }));
    }
    if (pathname === '/HtmlPasteGen.html' || pathname === '/') {
      res.setHeader('Content-Type', 'text/html;charset=utf-8');
      return res.end(fs.readFileSync(htmlPath));
    }
    if (pathname === '/composition-test.html') {
      res.setHeader('Content-Type', 'text/html;charset=utf-8');
      return res.end(core.buildGeneratedHtml(fixture));
    }
    res.statusCode = 404;
    res.end('Not found');
  });
  server.listen(0, '127.0.0.1', () => console.log(`UI fixture: http://127.0.0.1:${server.address().port}/HtmlPasteGen.html`));
}
