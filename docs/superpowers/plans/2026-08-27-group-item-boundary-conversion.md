# Group/Item Boundary Conversion Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Allow editor groups to demote into the previous group as items and allow any item to promote into a new group without losing item data or breaking four-level and legacy JSON rules.

**Architecture:** Add mutation-safe conversion operations to `HtmlPasteGenCore`, then connect them to the existing visible hierarchy controls and tree context menu through two editor orchestration functions. Keep JSON schema v2 unchanged and reuse existing render, focus, draft-save, announcement, confirmation, and navigation-expansion paths.

**Tech Stack:** Standalone HTML/CSS/JavaScript, Node.js `assert`/`vm` contract tests, Codex in-app browser smoke testing, C11 static web server built through WSL.

---

## File Map

- Modify `html/HtmlPasteGen.html`: core conversion model, exported API, group/item controls, context-menu actions, confirmation and post-conversion focus flow.
- Modify `tests/html_paste_gen_test.js`: mutation, data-preservation, depth-limit, missing-target and JSON validation coverage.
- Modify `tests/html_paste_gen_ui_test.js`: visible controls, right-click actions, continuous top-level promotion and editor orchestration contracts.
- Reference `docs/superpowers/specs/2026-08-27-group-item-boundary-conversion-design.md`: approved behavior and non-goals.

### Task 1: Implement mutation-safe core boundary conversions

**Files:**
- Modify: `tests/html_paste_gen_test.js`
- Modify: `html/HtmlPasteGen.html` in the `core-logic` helpers beside `pasteGroupAfter()`, `indentItem()` and the `window.HtmlPasteGenCore` export.

- [ ] **Step 1: Write failing core model tests**

Add this fixture and assertions after the existing group paste tests in `tests/html_paste_gen_test.js`:

```js
const boundaryDocument = {
  schemaVersion: 2,
  documentId: 'boundary-doc',
  meta: { title: '跨层级测试', filename: 'boundary.html', themePreference: 'system' },
  groups: [
    {
      id: 'group-alpha', title: '甲组', collapsed: false,
      items: [
        { id: 'alpha-item', title: '甲条目', content: 'alpha', shortcut: '', link: '', note: '', favorite: false, collapsed: false, children: [] }
      ]
    },
    {
      id: 'group-beta', title: '乙组', collapsed: true,
      items: [
        {
          id: 'beta-root', title: '乙根', content: 'beta-root', shortcut: 'Alt+7', link: 'https://example.com/beta', note: '保留备注', favorite: true, collapsed: false,
          children: [
            {
              id: 'beta-child', title: '乙子', content: 'beta-child', shortcut: '', link: '', note: '', favorite: false, collapsed: false,
              children: [
                { id: 'beta-leaf', title: '乙叶', content: 'beta-leaf', shortcut: '', link: '', note: '', favorite: false, collapsed: false, children: [] }
              ]
            }
          ]
        }
      ]
    },
    { id: 'group-gamma', title: '丙组', collapsed: false, items: [] }
  ]
};

const demotionDocument = core.cloneDocument(boundaryDocument);
const demotion = core.demoteGroupToItem(demotionDocument, 'group-beta');
assert.strictEqual(demotion.ok, true);
assert.deepStrictEqual(Array.from(demotionDocument.groups, group => group.id), ['group-alpha', 'group-gamma']);
assert.strictEqual(demotion.group.id, 'group-alpha');
assert.strictEqual(demotion.item.title, '乙组');
assert.strictEqual(demotion.item.content, '');
assert.strictEqual(demotion.item.collapsed, true);
assert.notStrictEqual(demotion.item.id, 'group-beta');
assert.deepStrictEqual(Array.from(demotion.item.children, item => item.id), ['beta-root']);
assert.strictEqual(demotion.item.children[0].shortcut, 'Alt+7');
assert.strictEqual(demotion.item.children[0].link, 'https://example.com/beta');
assert.strictEqual(demotion.item.children[0].note, '保留备注');
assert.strictEqual(demotion.item.children[0].favorite, true);
assert.strictEqual(core.validateDocument(demotionDocument).valid, true);

const firstGroupDocument = core.cloneDocument(boundaryDocument);
const firstGroupBefore = JSON.stringify(firstGroupDocument);
const firstGroupDemotion = core.demoteGroupToItem(firstGroupDocument, 'group-alpha');
assert.strictEqual(firstGroupDemotion.ok, false);
assert.match(firstGroupDemotion.reason, /第一个分组|上一分组/);
assert.strictEqual(JSON.stringify(firstGroupDocument), firstGroupBefore);

const deepGroupDocument = core.cloneDocument(treeDocument);
deepGroupDocument.groups.unshift({ id: 'depth-target', title: '接收组', collapsed: false, items: [] });
const deepGroupBefore = JSON.stringify(deepGroupDocument);
const deepGroupDemotion = core.demoteGroupToItem(deepGroupDocument, 'tree-group');
assert.strictEqual(deepGroupDemotion.ok, false);
assert.match(deepGroupDemotion.reason, /最多 4 级/);
assert.strictEqual(JSON.stringify(deepGroupDocument), deepGroupBefore);

const nestedPromotionDocument = core.cloneDocument(boundaryDocument);
const nestedPromotion = core.promoteItemToGroup(nestedPromotionDocument, 'beta-child');
assert.strictEqual(nestedPromotion.ok, true);
assert.deepStrictEqual(Array.from(nestedPromotionDocument.groups, group => group.title), ['甲组', '乙组', '乙子', '丙组']);
assert.strictEqual(nestedPromotion.group.items.length, 1);
assert.strictEqual(nestedPromotion.group.items[0].id, 'beta-child');
assert.strictEqual(nestedPromotion.group.items[0].children[0].id, 'beta-leaf');
assert.deepStrictEqual(Array.from(nestedPromotionDocument.groups[1].items[0].children, item => item.id), []);
assert.strictEqual(core.validateDocument(nestedPromotionDocument).valid, true);

const topPromotionDocument = core.cloneDocument(boundaryDocument);
const topPromotion = core.promoteItemToGroup(topPromotionDocument, 'beta-root');
assert.strictEqual(topPromotion.ok, true);
assert.deepStrictEqual(Array.from(topPromotionDocument.groups, group => group.title), ['甲组', '乙组', '乙根', '丙组']);
assert.strictEqual(topPromotion.item.id, 'beta-root');
assert.strictEqual(topPromotion.item.shortcut, 'Alt+7');
assert.strictEqual(topPromotion.item.note, '保留备注');
assert.strictEqual(topPromotion.item.children[0].id, 'beta-child');

const missingBoundaryDocument = core.cloneDocument(boundaryDocument);
const missingBoundaryBefore = JSON.stringify(missingBoundaryDocument);
assert.strictEqual(core.demoteGroupToItem(missingBoundaryDocument, 'missing-group').ok, false);
assert.strictEqual(core.promoteItemToGroup(missingBoundaryDocument, 'missing-item').ok, false);
assert.strictEqual(JSON.stringify(missingBoundaryDocument), missingBoundaryBefore);
```

- [ ] **Step 2: Run the core test and verify RED**

Run:

```bash
node tests/html_paste_gen_test.js
```

Expected: FAIL with `TypeError: core.demoteGroupToItem is not a function` (or the corresponding missing `promoteItemToGroup` function).

- [ ] **Step 3: Add non-mutating eligibility and conversion helpers**

Add these functions after `pasteGroupAfter()` in the main `core-logic` script in `html/HtmlPasteGen.html`:

```js
function groupDemotionStatus(value, groupId) {
  const groups = Array.isArray(value && value.groups) ? value.groups : [];
  const index = groups.findIndex(group => String(group && group.id || '') === String(groupId || ''));
  if (index < 0) return { ok: false, reason: '目标分组不存在，请刷新后重试。' };
  if (index === 0) return { ok: false, reason: '第一个分组没有可接收它的上一分组。' };
  const group = groups[index];
  const targetGroup = groups[index - 1];
  if (!Array.isArray(group.items) || !Array.isArray(targetGroup && targetGroup.items)) {
    return { ok: false, reason: '分组条目结构无效，无法转换。' };
  }
  const childHeight = group.items.length ? Math.max.apply(null, group.items.map(subtreeHeight)) : 0;
  if (childHeight + 1 > MAX_ITEM_DEPTH) {
    return { ok: false, reason: `降级后会超过最多 ${MAX_ITEM_DEPTH} 级。` };
  }
  return { ok: true, reason: '', groups, index, group, targetGroup };
}

function demoteGroupToItem(value, groupId) {
  const status = groupDemotionStatus(value, groupId);
  if (!status.ok) return status;
  const item = createItem({
    title: String(status.group.title || '').trim() || '未命名分组',
    collapsed: Boolean(status.group.collapsed),
    children: status.group.items
  });
  status.targetGroup.items.push(item);
  status.groups.splice(status.index, 1);
  return { ok: true, reason: '', group: status.targetGroup, item, sourceGroup: status.group };
}

function promoteItemToGroup(value, itemId) {
  const groups = Array.isArray(value && value.groups) ? value.groups : [];
  const location = findItemLocation(value, itemId);
  if (!location) return { ok: false, reason: '目标条目不存在，请刷新后重试。' };
  const groupIndex = groups.indexOf(location.group);
  if (groupIndex < 0 || !Array.isArray(location.siblings)) {
    return { ok: false, reason: '条目所在分组结构无效，无法转换。' };
  }
  const sourceGroup = location.group;
  const item = location.item;
  const group = createGroup({
    title: String(item.title || '').trim() || '未命名条目',
    collapsed: false,
    items: [item]
  });
  location.siblings.splice(location.index, 1);
  groups.splice(groupIndex + 1, 0, group);
  return { ok: true, reason: '', group, item, sourceGroup };
}
```

Export `groupDemotionStatus`, `demoteGroupToItem`, and `promoteItemToGroup` from `window.HtmlPasteGenCore` beside the existing tree helpers.

- [ ] **Step 4: Run focused core tests and verify GREEN**

Run:

```bash
node tests/html_paste_gen_test.js
```

Expected: `HtmlPasteGen core model tests passed` and exit code 0.

- [ ] **Step 5: Commit the core conversion model**

```bash
git add html/HtmlPasteGen.html tests/html_paste_gen_test.js
git commit -m "feat: add group item boundary conversions"
```

### Task 2: Connect continuous hierarchy controls and right-click actions

**Files:**
- Modify: `tests/html_paste_gen_ui_test.js`
- Modify: `html/HtmlPasteGen.html` in `openTreeContextMenu()`, `handleTreeContextAction()`, `renderGroupList()`, `renderItemTree()`, `createItemEditor()` and `handleAction()`.

- [ ] **Step 1: Write failing UI contract assertions**

Add these assertions beside the existing tree context-menu assertions in `tests/html_paste_gen_ui_test.js`:

```js
assert.match(html, /demote-group-to-item/);
assert.match(html, /promote-item-to-group/);
assert.match(html, /groupDemotionStatus\(/);
assert.match(html, /function\s+demoteGroupFromEditor\s*\(/);
assert.match(html, /function\s+promoteItemFromEditor\s*\(/);
assert.match(html, /降级为上一分组的条目/);
assert.match(html, /直接升级为分组/);
assert.match(html, /currentDepth\s*===\s*1[\s\S]*升级为分组/);
assert.match(html, /confirm\([\s\S]*升级为新分组/);
```

- [ ] **Step 2: Run the UI test and verify RED**

Run:

```bash
node tests/html_paste_gen_ui_test.js
```

Expected: FAIL on the first missing `demote-group-to-item` or `promote-item-to-group` contract.

- [ ] **Step 3: Add reusable editor conversion orchestration**

Add these functions near `updateAfterAction()` in `html/HtmlPasteGen.html`:

```js
function demoteGroupFromEditor(group) {
  const status = core.groupDemotionStatus(state.document, group && group.id);
  if (!status.ok) {
    announce(status.reason, 'warning');
    return false;
  }
  const count = core.flattenItems({ groups: [{ items: status.group.items }] }).length;
  if (!confirm(`将分组“${status.group.title || '未命名分组'}”降级为“${status.targetGroup.title || '未命名分组'}”末尾的条目？其中 ${count} 条内容会作为它的子条目。`)) {
    return false;
  }
  const result = core.demoteGroupToItem(state.document, status.group.id);
  if (!result.ok) {
    announce(result.reason, 'warning');
    return false;
  }
  expandedNavGroupIds.add(result.group.id);
  if (result.item.children.length) expandedNavNodeIds.add(result.item.id);
  scheduleDraftSave();
  selectAndFocus(result.group.id, result.item.id);
  announce('分组已降级为上一分组中的条目。', 'success');
  return true;
}

function promoteItemFromEditor(item) {
  const location = core.findItemLocation(state.document, item && item.id);
  if (!location) {
    announce('目标条目不存在，请刷新后重试。', 'warning');
    return false;
  }
  const count = core.descendantCount(location.item);
  if (!confirm(`将条目“${location.item.title || '未命名条目'}”升级为新分组？原条目及其 ${count} 个子条目会完整保留为新组第一条。`)) {
    return false;
  }
  const result = core.promoteItemToGroup(state.document, location.item.id);
  if (!result.ok) {
    announce(result.reason, 'warning');
    return false;
  }
  expandedNavGroupIds.add(result.group.id);
  scheduleDraftSave();
  selectAndFocus(result.group.id, result.item.id);
  announce('条目已升级为新分组，原内容完整保留。', 'success');
  return true;
}
```

Keep both helpers as the only UI entry points for conversion so visible buttons and context-menu commands share confirmation, selection, expansion, save and announcement behavior.

- [ ] **Step 4: Add context-menu actions and disabled reasons**

Extend `contextMenuButton()` to accept an optional disabled reason:

```js
function contextMenuButton(label, action, disabled, disabledReason) {
  const button = createElement('button', 'tree-context-menu-item', label);
  button.type = 'button';
  button.setAttribute('role', 'menuitem');
  button.dataset.contextAction = action;
  button.disabled = Boolean(disabled);
  if (button.disabled && disabledReason) {
    button.title = disabledReason;
    button.setAttribute('aria-label', `${label}：${disabledReason}`);
  }
  return button;
}
```

In `openTreeContextMenu()`, compute group eligibility and append the new commands:

```js
if (target.kind === 'group') {
  const demotion = core.groupDemotionStatus(state.document, target.group.id);
  menu.append(
    contextMenuButton('复制分组', 'copy-group'),
    contextMenuButton('粘贴分组', 'paste-group', !treeClipboard || treeClipboard.kind !== 'group'),
    contextMenuButton('降级为上一分组的条目', 'demote-group-to-item', !demotion.ok, demotion.reason),
    contextMenuButton('删除分组', 'delete-group')
  );
} else {
  menu.append(
    contextMenuButton('复制条目', 'copy-item'),
    contextMenuButton('粘贴为子条目', 'paste-item-child', !treeClipboard || treeClipboard.kind !== 'item' || core.itemDepth(state.document, target.item.id) >= core.MAX_ITEM_DEPTH),
    contextMenuButton('粘贴为同级条目', 'paste-item-sibling', !treeClipboard || treeClipboard.kind !== 'item'),
    contextMenuButton('直接升级为分组', 'promote-item-to-group'),
    contextMenuButton('删除条目', 'delete-item')
  );
}
```

Before delete handling in `handleTreeContextAction()` add:

```js
if (action === 'demote-group-to-item' && target.kind === 'group') {
  demoteGroupFromEditor(target.group);
  return;
}
if (action === 'promote-item-to-group' && target.kind === 'item') {
  promoteItemFromEditor(target.item);
  return;
}
```

- [ ] **Step 5: Add visible continuous hierarchy controls**

In `renderGroupList()`, add a third group action after up/down:

```js
const demotion = core.groupDemotionStatus(state.document, group.id);
const demote = makeButton('↳', 'demote-group-to-item', 'icon-button group-demote-button', demotion.ok ? '降级为上一分组的条目' : demotion.reason);
demote.disabled = !demotion.ok;
actions.append(up, down, demote);
```

In `renderItemTree()`, keep the top-level outdent button enabled and make its meaning explicit:

```js
const outdentLabel = depth === 1 ? '升级为分组' : '提升层级';
const outdent = makeButton('−', 'outdent-item', 'tree-level-button', outdentLabel);
outdent.disabled = false;
```

In `createItemEditor()`, after the action buttons are appended, replace the existing top-level disable rule with:

```js
actions.children[4].disabled = false;
actions.children[4].title = currentDepth === 1 ? '升级为分组' : '提升层级';
actions.children[4].setAttribute('aria-label', actions.children[4].title);
```

- [ ] **Step 6: Route visible actions through the shared orchestration**

In `handleAction()`:

```js
if (action === 'demote-group-to-item') {
  demoteGroupFromEditor(group);
  return;
}
```

Place this after group reordering and before item-only handling. Then replace the item hierarchy block with:

```js
if (action === 'indent-item' || action === 'outdent-item') {
  if (action === 'outdent-item' && location && location.depth === 1) {
    promoteItemFromEditor(item);
    return;
  }
  const result = handleTreeAction(action, item.id);
  if (!result.ok) {
    announce(result.reason, 'warning');
    return;
  }
}
```

Nested outdent behavior remains unchanged; only the top-level boundary now promotes to a group.

- [ ] **Step 7: Run focused UI and core tests**

Run:

```bash
node tests/html_paste_gen_ui_test.js
node tests/html_paste_gen_test.js
```

Expected:

```text
HtmlPasteGen UI contract tests passed
HtmlPasteGen core model tests passed
```

- [ ] **Step 8: Commit the editor interaction**

```bash
git add html/HtmlPasteGen.html tests/html_paste_gen_ui_test.js
git commit -m "feat: connect group item hierarchy controls"
```

### Task 3: Verify browser behavior, regressions and delivery

**Files:**
- Verify: `html/HtmlPasteGen.html`
- Verify: all existing test files under `tests/`
- No unrelated source files should be modified.

- [ ] **Step 1: Restart the local server with the verified build order**

Run:

```bash
wsl ./simplewebserver.sh restart -p 8881
```

Expected: compilation succeeds before the running server is replaced, followed by a successful start on port 8881.

- [ ] **Step 2: Run browser smoke tests against the editor**

Open `http://127.0.0.1:8881/HtmlPasteGen.html` in the Codex in-app browser and verify:

1. The first group demotion button is disabled and its accessible label explains that no previous group exists.
2. Right-click the second group, choose “降级为上一分组的条目”, accept the confirmation, and verify the group count decreases by one while a same-title wrapper item appears at the previous group’s end with the old subtree beneath it.
3. Restore sample data, make an item nested if needed, right-click it, choose “直接升级为分组”, accept the confirmation, and verify the new group appears immediately after the source group with the original item data intact.
4. On a top-level item, click the visible “− 升级” control and verify it uses the same item-to-group confirmation and result.
5. Build or import a four-level subtree in the second group and verify group demotion is disabled or refused with “最多 4 级”, with no document mutation.
6. Read console warnings/errors and confirm there are no new runtime exceptions.

- [ ] **Step 3: Run the full automated regression and build**

Run:

```bash
node tests/html_paste_gen_test.js
node tests/html_paste_gen_ui_test.js
node tests/http_handler_html_paste_test.js
node tests/index_navigation_test.js
node tests/simplewebserver_restart_test.js
node tests/html_paste_bundle_api_test.js
git diff --check
wsl make -B
```

Expected: all six Node test commands exit 0, `git diff --check` reports no whitespace errors, and the build ends with `Build successful: bin/simplewebserver`. Existing unrelated compiler warnings may remain but no new warning should originate from this HTML/JavaScript-only change.

- [ ] **Step 4: Inspect scope and push the existing feature branch**

Run:

```bash
git status --short
git log --oneline -5
git push
gh pr view 1 --repo wangfanstar/LinuxCmdTest --json number,url,state,headRefName,commits
```

Expected: only the user's pre-existing unrelated working-tree changes remain; the branch `codex/register-viewer-auto-parse` is pushed and open PR #1 contains both new feature commits.
