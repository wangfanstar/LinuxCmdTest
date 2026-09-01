# Register Value Radix Mode Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add automatic, forced hexadecimal, and forced decimal input modes to the register value parser with immediate re-parsing.

**Architecture:** Extend the existing pure `parseRegisterInput` boundary with a mode argument, keeping all bit-field decoding unchanged. Add one persistent page-level radix preference and a reusable segmented control in the existing parser modal; mode changes update ARIA state, hint text, and invoke the current live-render path.

**Tech Stack:** Plain HTML/CSS/ES5-style JavaScript with `BigInt`; Node.js assertion tests; existing shell regression tests and C `make` build.

---

### Task 1: Mode-aware input parsing

**Files:**
- Modify: `tests/register_value_parser_test.js`
- Modify: `html/register-viewer.html` (`parseRegisterInput`)

- [ ] **Step 1: Write the failing parser-mode tests**

Add assertions demonstrating the desired interpretation and rejection rules:

```js
assert.strictEqual(context.parseRegisterInput('10', 'auto').value, 10n);
assert.strictEqual(context.parseRegisterInput('10', 'hex').value, 16n);
assert.strictEqual(context.parseRegisterInput('0x10', 'hex').value, 16n);
assert.strictEqual(context.parseRegisterInput('10h', 'hex').value, 16n);
assert.strictEqual(context.parseRegisterInput('10', 'dec').value, 10n);
assert.strictEqual(context.parseRegisterInput('0x10', 'dec').ok, false);
assert.strictEqual(context.parseRegisterInput('AB', 'dec').ok, false);
assert.strictEqual(context.parseRegisterInput('GG', 'hex').ok, false);
```

- [ ] **Step 2: Run the focused test and verify RED**

Run: `node tests/register_value_parser_test.js`

Expected: FAIL because forced HEX still interprets `10` as decimal 10.

- [ ] **Step 3: Implement the minimal mode-aware parser**

Normalize unknown modes to `auto`, strip underscores, and branch before automatic detection:

```js
function parseRegisterInput(raw, mode) {
  var radixMode = /^(hex|dec)$/.test(mode || '') ? mode : 'auto';
  var original = raw === null || raw === undefined ? '' : String(raw).trim();
  var s = original.replace(/_/g, '');
  if (!s) return { ok: false, empty: true, error: '' };
  if (typeof BigInt !== 'function') {
    return { ok: false, empty: false, error: '当前浏览器不支持精确的大整数解析，请升级浏览器。' };
  }
  if (radixMode === 'hex') {
    var hexBody = s.replace(/^0x/i, '').replace(/h$/i, '');
    if (/^[0-9a-f]+$/i.test(hexBody)) {
      return { ok: true, value: BigInt('0x' + hexBody), radix: 16 };
    }
    return { ok: false, empty: false, error: '强制 HEX 模式仅接受 0-9、A-F，可带 0x 前缀或 h 后缀。' };
  }
  if (radixMode === 'dec') {
    if (/^[0-9]+$/.test(s)) return { ok: true, value: BigInt(s), radix: 10 };
    return { ok: false, empty: false, error: '强制 DEC 模式仅接受十进制数字。' };
  }
  try {
    if (/^0x[0-9a-f]+$/i.test(s)) return { ok: true, value: BigInt(s), radix: 16 };
    if (/^[0-9a-f]+h$/i.test(s)) return { ok: true, value: BigInt('0x' + s.slice(0, -1)), radix: 16 };
    if (/^[0-9]*[a-f][0-9a-f]*$/i.test(s)) return { ok: true, value: BigInt('0x' + s), radix: 16 };
    if (/^[0-9]+$/.test(s)) return { ok: true, value: BigInt(s), radix: 10 };
  } catch (ignore) {}
  return { ok: false, empty: false, error: '请输入非负整数：十六进制示例 0x1234、ABCDh，十进制示例 4660。' };
}
```

- [ ] **Step 4: Run the focused test and verify GREEN**

Run: `node tests/register_value_parser_test.js`

Expected: `register value parser tests passed`.

### Task 2: Segmented mode control and live re-parse

**Files:**
- Modify: `html/register-viewer.html` (parser CSS, state/functions, modal markup and help text)
- Modify: `tests/register_value_parser_test.js`

- [ ] **Step 1: Write failing UI contract assertions**

Add static assertions for three mode buttons and the switching handler:

```js
assert.match(html, /data-radix="auto"[^>]*onclick="setRegisterParserRadix\('auto'\)"/);
assert.match(html, /data-radix="hex"[^>]*onclick="setRegisterParserRadix\('hex'\)"/);
assert.match(html, /data-radix="dec"[^>]*onclick="setRegisterParserRadix\('dec'\)"/);
assert.match(html, /function setRegisterParserRadix\(mode\)/);
```

- [ ] **Step 2: Run the focused test and verify RED**

Run: `node tests/register_value_parser_test.js`

Expected: FAIL because mode-control markup and handler do not exist.

- [ ] **Step 3: Add persistent state and switching behavior**

Add page-level state and a handler that updates selected styling/ARIA without clearing input:

```js
var valueParserRadix = 'auto';

function setRegisterParserRadix(mode) {
  valueParserRadix = /^(hex|dec)$/.test(mode || '') ? mode : 'auto';
  var buttons = document.querySelectorAll('.rvp-radix-btn');
  for (var i = 0; i < buttons.length; i++) {
    var active = buttons[i].getAttribute('data-radix') === valueParserRadix;
    buttons[i].classList.toggle('active', active);
    buttons[i].setAttribute('aria-pressed', active ? 'true' : 'false');
  }
  var hint = document.getElementById('value-parser-hint');
  if (hint) hint.textContent = valueParserRadix === 'hex'
    ? '强制 HEX：纯数字也按十六进制解释；可带 0x 前缀、h 后缀或下划线。'
    : (valueParserRadix === 'dec'
      ? '强制 DEC：仅接受十进制数字，可用下划线分组。'
      : '自动识别：纯数字按十进制；0x、h 后缀或含 A-F 时按十六进制。');
  renderRegisterValueParse();
}
```

Update `renderRegisterValueParse` to call `parseRegisterInput(input.value, valueParserRadix)`. Add the control above the input:

```html
<div class="rvp-input-toolbar">
  <span>输入进制</span>
  <div class="rvp-radix-switch" role="group" aria-label="输入进制">
    <button type="button" class="rvp-radix-btn active" data-radix="auto" aria-pressed="true" onclick="setRegisterParserRadix('auto')">自动</button>
    <button type="button" class="rvp-radix-btn" data-radix="hex" aria-pressed="false" onclick="setRegisterParserRadix('hex')">HEX</button>
    <button type="button" class="rvp-radix-btn" data-radix="dec" aria-pressed="false" onclick="setRegisterParserRadix('dec')">DEC</button>
  </div>
</div>
```

- [ ] **Step 4: Add responsive CSS and help text**

Use these style rules and update the help dialog to explain that pure digits are interpreted according to the selected mode:

```css
.rvp-input-toolbar { display:flex; align-items:center; justify-content:space-between; gap:8px; margin-bottom:7px; }
.rvp-radix-switch { display:inline-flex; border:1px solid #b8c6d8; border-radius:7px; overflow:hidden; }
.rvp-radix-btn { border:0; border-right:1px solid #d4dce7; padding:4px 10px; background:#fff; color:#617086; cursor:pointer; }
.rvp-radix-btn:last-child { border-right:0; }
.rvp-radix-btn.active { background:#2869ad; color:#fff; }
.rvp-radix-btn:focus-visible { outline:2px solid rgba(74,144,226,.4); outline-offset:-2px; }
@media (max-width:700px) { .rvp-input-toolbar { align-items:flex-start; flex-direction:column; } }
```

- [ ] **Step 5: Run focused tests and verify GREEN**

Run: `node tests/register_value_parser_test.js`

Expected: `register value parser tests passed`.

### Task 3: Regression and delivery verification

**Files:**
- Verify: `html/register-viewer.html`
- Verify: `tests/register_value_parser_test.js`

- [ ] **Step 1: Check full-page JavaScript syntax**

Run the repository's Node `new Function` syntax check against every inline `<script>` in `html/register-viewer.html`.

Expected: `register-viewer scripts syntax OK (1)`.

- [ ] **Step 2: Run existing register-viewer regressions**

Run:

```bash
bash tests/register_viewer_selection_test.sh
bash tests/register_viewer_latest_autoload_test.sh
bash tests/register_viewer_description_overlay_test.sh
python tests/register_description_keys_test.py
```

Expected: all commands exit 0.

- [ ] **Step 3: Verify in a real browser**

Load `register-viewer.html` through the local server, open a multi-field register, and verify:

- Auto + `10` produces decimal 10.
- HEX + unchanged `10` immediately produces decimal 16.
- DEC + unchanged `10` immediately returns to decimal 10.
- DEC + `0x10` shows the forced-DEC error without clearing the input.
- Selected mode styling is clear at desktop and narrow viewport sizes.

- [ ] **Step 4: Run Linux build and diff checks**

Run:

```bash
make -j2
git diff --check -- html/register-viewer.html tests/register_value_parser_test.js
```

Expected: both commands exit 0.

- [ ] **Step 5: Commit the feature files**

```bash
git add html/register-viewer.html tests/register_value_parser_test.js
git commit -m "feat: add register parser radix modes"
```
