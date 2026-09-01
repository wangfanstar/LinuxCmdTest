const assert = require('assert');
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const htmlPath = path.join(__dirname, '..', 'html', 'register-viewer.html');
const html = fs.readFileSync(htmlPath, 'utf8');

function extractFunction(source, name) {
  const marker = `function ${name}(`;
  const start = source.indexOf(marker);
  assert.notStrictEqual(start, -1, `missing function ${name}`);
  const bodyStart = source.indexOf('{', start);
  let depth = 0;
  let quote = '';
  let escaped = false;
  for (let i = bodyStart; i < source.length; i += 1) {
    const ch = source[i];
    if (quote) {
      if (escaped) escaped = false;
      else if (ch === '\\') escaped = true;
      else if (ch === quote) quote = '';
      continue;
    }
    if (ch === '"' || ch === "'" || ch === '`') {
      quote = ch;
      continue;
    }
    if (ch === '{') depth += 1;
    else if (ch === '}') {
      depth -= 1;
      if (depth === 0) return source.slice(start, i + 1);
    }
  }
  throw new Error(`unterminated function ${name}`);
}

const functionNames = [
  'parseRegisterInput',
  'registerValueWidth',
  'registerBitMask',
  'decodeRegisterFields',
  'findFieldValueDefinition'
];
const context = { BigInt };
vm.createContext(context);
vm.runInContext(functionNames.map(name => extractFunction(html, name)).join('\n'), context);

let parsed = context.parseRegisterInput('0xFEDC_BA98_7654_3210');
assert.strictEqual(parsed.ok, true);
assert.strictEqual(parsed.radix, 16);
assert.strictEqual(parsed.value, 0xFEDCBA9876543210n, 'hex input must stay exact');

parsed = context.parseRegisterInput('18446744073709551615');
assert.strictEqual(parsed.ok, true);
assert.strictEqual(parsed.radix, 10);
assert.strictEqual(parsed.value, 0xFFFFFFFFFFFFFFFFn, 'decimal uint64 must stay exact');

assert.strictEqual(context.parseRegisterInput('1234h').value, 0x1234n);
assert.strictEqual(context.parseRegisterInput('DEAD_BEEF').value, 0xDEADBEEFn);

assert.strictEqual(context.parseRegisterInput('10', 'auto').value, 10n);
assert.strictEqual(context.parseRegisterInput('10', 'hex').value, 16n);
assert.strictEqual(context.parseRegisterInput('0x10', 'hex').value, 16n);
assert.strictEqual(context.parseRegisterInput('10h', 'hex').value, 16n);
assert.strictEqual(context.parseRegisterInput('10', 'dec').value, 10n);
assert.strictEqual(context.parseRegisterInput('0x10', 'dec').ok, false);
assert.strictEqual(context.parseRegisterInput('AB', 'dec').ok, false);
assert.strictEqual(context.parseRegisterInput('GG', 'hex').ok, false);

for (const bad of ['', '0x', '12.5', '-1', 'hello']) {
  assert.strictEqual(context.parseRegisterInput(bad).ok, false, `${bad} must be rejected`);
}

const reg = {
  fields: [
    { name: 'BIT0', startBit: 0, endBit: 0, desc: '0 - off\n1 - on' },
    { name: 'MODE', startBit: 1, endBit: 3, desc: 'Mode selection' },
    { name: 'HIGH', startBit: 4, endBit: 7, desc: '' }
  ]
};
assert.strictEqual(context.registerValueWidth(reg), 8);
const decoded = context.decodeRegisterFields(reg, 0xABn);
assert.deepStrictEqual(Array.from(decoded, item => item.field.name), ['HIGH', 'MODE', 'BIT0']);
assert.strictEqual(decoded[0].value, 10n);
assert.strictEqual(decoded[0].hex, '0xA');
assert.strictEqual(decoded[1].value, 5n);
assert.strictEqual(decoded[1].hex, '0x5');
assert.strictEqual(decoded[2].value, 1n);
assert.strictEqual(decoded[2].hex, '0x1');

assert.strictEqual(
  context.findFieldValueDefinition('0 - disabled\n1 - enabled', 1n),
  'enabled'
);
assert.strictEqual(
  context.findFieldValueDefinition('0x0: idle\n0xA: transmit', 10n),
  'transmit'
);
assert.strictEqual(context.findFieldValueDefinition('General field description', 2n), '');

assert.match(html, /class="reg-value-parse-btn"[^>]*onclick="openRegisterValueParser\(/);
assert.match(html, /id="value-parser-input"[^>]*oninput="renderRegisterValueParse\(\)"/);
assert.match(html, /id="value-parser-overlay"/);
assert.match(html, /data-radix="auto"[^>]*onclick="setRegisterParserRadix\('auto'\)"/);
assert.match(html, /data-radix="hex"[^>]*onclick="setRegisterParserRadix\('hex'\)"/);
assert.match(html, /data-radix="dec"[^>]*onclick="setRegisterParserRadix\('dec'\)"/);
assert.match(html, /function setRegisterParserRadix\(mode\)/);

console.log('register value parser tests passed');
