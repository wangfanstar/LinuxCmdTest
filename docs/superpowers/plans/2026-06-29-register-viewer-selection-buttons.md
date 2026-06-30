# Register Viewer Selection Buttons Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add explicit buttons in `register-viewer.html` to select all registers on the current page or all registers in the current filtered result set.

**Architecture:** This is a single-file browser UI change. The selection state already lives in the global `selected` Set and the current result set lives in `filtered`, so the feature should add small wrapper functions that add visible/current-page UIDs to `selected` and then reuse existing UI sync helpers.

**Tech Stack:** Static HTML/CSS/JavaScript in `html/register-viewer.html`; lightweight shell/static verification with `tests/register_viewer_selection_test.sh`.

---

### Task 1: Add Static Behavior Test

**Files:**
- Create: `tests/register_viewer_selection_test.sh`
- Read: `html/register-viewer.html`

- [ ] **Step 1: Write the failing test**

Create `tests/register_viewer_selection_test.sh` with this content:

```bash
#!/usr/bin/env bash
set -euo pipefail

FILE="html/register-viewer.html"

require_text() {
  local needle="$1"
  if ! grep -Fq "$needle" "$FILE"; then
    printf 'Missing expected text: %s\n' "$needle" >&2
    exit 1
  fi
}

require_regex() {
  local regex="$1"
  if ! grep -Eq "$regex" "$FILE"; then
    printf 'Missing expected pattern: %s\n' "$regex" >&2
    exit 1
  fi
}

require_text 'onclick="selectCurrentPage()"'
require_text 'onclick="selectAllFiltered()"'
require_text '全选当前页'
require_text '全选全部'
require_regex 'function selectCurrentPage\(\)[[:space:]]*\{'
require_regex 'function selectAllFiltered\(\)[[:space:]]*\{'
require_text 'selected.add(pg[i].uid);'
require_text 'selected.add(filtered[i].uid);'
require_text 'syncVisibleCheckboxes();'
```

- [ ] **Step 2: Run test to verify it fails**

Run:

```bash
bash tests/register_viewer_selection_test.sh
```

Expected: FAIL, with the first missing text similar to:

```text
Missing expected text: onclick="selectCurrentPage()"
```

### Task 2: Implement Selection Buttons

**Files:**
- Modify: `html/register-viewer.html:703-708`
- Modify: `html/register-viewer.html:1646-1673`
- Test: `tests/register_viewer_selection_test.sh`

- [ ] **Step 1: Add buttons to the selection bar**

In `html/register-viewer.html`, update the selection bar button block so it contains these buttons immediately after `<span id="select-count">已选 0 个</span>`:

```html
<span id="select-count">已选 0 个</span>
<button class="btn-toolbar secondary" onclick="selectCurrentPage()">☑ 全选当前页</button>
<button class="btn-toolbar secondary" onclick="selectAllFiltered()">☑ 全选全部</button>
<button class="btn-toolbar" onclick="saveSelectedToRegister()">💾 存档选中</button>
<button class="btn-toolbar success" onclick="exportSelected()">⬇ 导出选中 JSON</button>
<button class="btn-toolbar secondary" onclick="exportAll()">⬇ 导出全部结果</button>
<button class="btn-toolbar danger" onclick="clearSelection()">取消选择</button>
```

- [ ] **Step 2: Add shared checkbox sync helper and selection functions**

In the `// ── 全选 ──` section, replace the existing `toggleSelectAll` function with this version and add the two new functions plus helper:

```javascript
function getCurrentPageRegisters() {
  var start=(currentPage-1)*PAGE_SIZE;
  return filtered.slice(start,start+PAGE_SIZE);
}
function syncVisibleCheckboxes() {
  var chks=document.querySelectorAll('.reg-chk');
  for(var j=0;j<chks.length;j++) chks[j].checked=selected.has(Number(chks[j].dataset.uid));
}
function selectCurrentPage() {
  var pg=getCurrentPageRegisters();
  for(var i=0;i<pg.length;i++) selected.add(pg[i].uid);
  syncVisibleCheckboxes();
  syncHeaderCheckbox();
  updateSelectBar();
}
function selectAllFiltered() {
  for(var i=0;i<filtered.length;i++) selected.add(filtered[i].uid);
  syncVisibleCheckboxes();
  syncHeaderCheckbox();
  updateSelectBar();
}
function toggleSelectAll(checked) {
  var pg=getCurrentPageRegisters();
  for(var i=0;i<pg.length;i++){ if(checked) selected.add(pg[i].uid); else selected.delete(pg[i].uid); }
  syncVisibleCheckboxes();
  updateSelectBar();
}
```

- [ ] **Step 3: Update `syncHeaderCheckbox` to use helper**

Change the first two lines inside `syncHeaderCheckbox()` from recalculating `start` and `pg` to:

```javascript
  var pg=getCurrentPageRegisters();
```

- [ ] **Step 4: Update `clearSelection` to use helper**

Replace the checkbox loop inside `clearSelection()` with:

```javascript
  syncVisibleCheckboxes();
```

The final full selection section should be:

```javascript
// ── 全选 ─────────────────────────────────────────────────
function getCurrentPageRegisters() {
  var start=(currentPage-1)*PAGE_SIZE;
  return filtered.slice(start,start+PAGE_SIZE);
}
function syncVisibleCheckboxes() {
  var chks=document.querySelectorAll('.reg-chk');
  for(var j=0;j<chks.length;j++) chks[j].checked=selected.has(Number(chks[j].dataset.uid));
}
function selectCurrentPage() {
  var pg=getCurrentPageRegisters();
  for(var i=0;i<pg.length;i++) selected.add(pg[i].uid);
  syncVisibleCheckboxes();
  syncHeaderCheckbox();
  updateSelectBar();
}
function selectAllFiltered() {
  for(var i=0;i<filtered.length;i++) selected.add(filtered[i].uid);
  syncVisibleCheckboxes();
  syncHeaderCheckbox();
  updateSelectBar();
}
function toggleSelectAll(checked) {
  var pg=getCurrentPageRegisters();
  for(var i=0;i<pg.length;i++){ if(checked) selected.add(pg[i].uid); else selected.delete(pg[i].uid); }
  syncVisibleCheckboxes();
  updateSelectBar();
}
function syncHeaderCheckbox() {
  var pg=getCurrentPageRegisters();
  var el=document.getElementById('chk-all'); if(!el)return;
  var allSel=pg.length>0;
  var anySel=false;
  for(var i=0;i<pg.length;i++){
    if(!selected.has(pg[i].uid)) allSel=false;
    else anySel=true;
  }
  el.checked=allSel;
  el.indeterminate=!allSel&&anySel;
}
function clearSelection() {
  selected.clear();
  syncVisibleCheckboxes();
  syncHeaderCheckbox(); updateSelectBar();
}
```

- [ ] **Step 5: Run test to verify it passes**

Run:

```bash
bash tests/register_viewer_selection_test.sh
```

Expected: PASS with no output.

### Task 3: Verify Build Still Works

**Files:**
- Read: `Makefile`
- No code changes expected

- [ ] **Step 1: Run project build**

Run:

```bash
make
```

Expected: build completes successfully and produces `bin/simplewebserver`.

- [ ] **Step 2: Optional manual browser check**

Run the server if needed:

```bash
make run
```

Open `register-viewer.html`, load XML/JSON register data, and verify:

1. Clicking `全选当前页` increases `已选` by the number of records on the current page that were not already selected.
2. Clicking `全选全部` selects every record in the current filtered result set.
3. Existing header checkbox still toggles only the current page.
4. `取消选择` clears all selected records and updates visible checkboxes.

---

## Self-Review

- Spec coverage: Task 2 adds both requested buttons and implements current-page and all-filtered-register selection behavior.
- Placeholder scan: No placeholders remain; all code snippets and commands are concrete.
- Type/name consistency: New functions are `getCurrentPageRegisters`, `syncVisibleCheckboxes`, `selectCurrentPage`, and `selectAllFiltered`; button `onclick` values match these names.
