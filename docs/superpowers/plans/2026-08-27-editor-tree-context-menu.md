# Editor Tree Context Menu Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task with review checkpoints.

**Goal:** Add a safe, keyboard-accessible right-click menu to the editor's left tree navigation for copying, pasting, and deleting item or group subtrees.

**Architecture:** Keep the feature in \`HtmlPasteGen.html\`'s existing core and editor runtime. Add tested core operations for cloning/pasting trees, an in-memory clipboard for the editor, a fixed context-menu DOM surface, and title-node event listeners plus document-level close handling. Existing \`renderAll()\`, draft persistence, validation, and announcements remain the single refresh path.

**Tech Stack:** Embedded plain JavaScript/HTML/CSS; Node.js \`assert\` + \`vm\` static/core tests; WSL GNU Make for the C server regression build.

---

### Task 1: Add failing core and UI contract tests

**Files:**
- Modify: \`tests/html_paste_gen_test.js\` near recursive tree operation assertions
- Modify: \`tests/html_paste_gen_ui_test.js\` near tree navigation assertions

- [ ] **Step 1: Write failing core operation tests**

After the existing \`cloneItemTree\` assertions in \`tests/html_paste_gen_test.js\`, add:

\`\`\`js
assert.strictEqual(core.subtreeHeight(treeDocument.groups[0].items[0]), 4);
const copiedGroup = core.cloneGroupTree(treeDocument.groups[0]);
assert.notStrictEqual(copiedGroup.id, treeDocument.groups[0].id);
assert.notStrictEqual(copiedGroup.items[0].id, treeDocument.groups[0].items[0].id);
assert.strictEqual(copiedGroup.items[0].shortcut, '');
const childPasteDocument = core.cloneDocument(treeDocument);
const childPaste = core.pasteItemAsChild(
  childPasteDocument,
  'root',
  treeDocument.groups[0].items[0].children[0].children[0].children[0]
);
assert.strictEqual(childPaste.ok, true);
assert.notStrictEqual(childPaste.item.id, 'leaf');
assert.strictEqual(childPasteDocument.groups[0].items[0].children.at(-1).title, '叶（副本）');
const siblingPasteDocument = core.cloneDocument(treeDocument);
const siblingPaste = core.pasteItemAsSibling(
  siblingPasteDocument,
  'child',
  treeDocument.groups[0].items[0].children[0]
);
assert.strictEqual(siblingPaste.ok, true);
assert.strictEqual(siblingPasteDocument.groups[0].items[0].children.length, 2);
const depthPasteDocument = core.cloneDocument(treeDocument);
const depthBefore = JSON.stringify(depthPasteDocument);
const depthPaste = core.pasteItemAsChild(depthPasteDocument, 'leaf', treeDocument.groups[0].items[0]);
assert.strictEqual(depthPaste.ok, false);
assert.match(depthPaste.reason, /最多 4 级/);
assert.strictEqual(JSON.stringify(depthPasteDocument), depthBefore);
const groupPasteDocument = core.cloneDocument(treeDocument);
const groupPaste = core.pasteGroupAfter(groupPasteDocument, 'tree-group', treeDocument.groups[0]);
assert.strictEqual(groupPaste.ok, true);
assert.strictEqual(groupPasteDocument.groups.length, 2);
assert.notStrictEqual(groupPasteDocument.groups[1].id, 'tree-group');
\`\`\`

- [ ] **Step 2: Write failing UI context-menu assertions**

After the existing tree navigation assertions in \`tests/html_paste_gen_ui_test.js\`, add:

\`\`\`js
assert.match(html, /id=["']tree-context-menu["']/);
assert.match(html, /class=["'][^"']*tree-context-menu[^"']*["']/);
assert.match(html, /contextmenu/);
assert.match(html, /treeClipboard/);
assert.match(html, /contextMenuState/);
assert.match(html, /function\s+openTreeContextMenu\s*\(/);
assert.match(html, /function\s+handleTreeContextAction\s*\(/);
assert.match(html, /data-context-action/);
assert.match(html, /pasteItemAsChild\(/);
assert.match(html, /pasteItemAsSibling\(/);
assert.match(html, /pasteGroupAfter\(/);
assert.match(html, /Escape/);
\`\`\`

- [ ] **Step 3: Run tests and verify the expected RED failures**

Run:

\`\`\`bash
node tests/html_paste_gen_test.js
node tests/html_paste_gen_ui_test.js
\`\`\`

Expected: core tests fail on the missing \`subtreeHeight\`/clone/paste API and UI tests fail on the missing \`tree-context-menu\` markup. Do not change production code before observing both failures.

### Task 2: Implement tested recursive core clone/paste operations

**Files:**
- Modify: \`html/HtmlPasteGen.html\` beside \`cloneItemTree\` and the core export object

- [ ] **Step 1: Add the clone/paste helpers**

Add \`cloneGroupTree\`, \`pasteItemAsChild\`, \`pasteItemAsSibling\`, and \`pasteGroupAfter\` after \`cloneItemTree\`:

\`\`\`js
function cloneGroupTree(group) {
  const copy = cloneDocument(group || {});
  copy.id = newId('group');
  copy.title = \`\${copy.title || '未命名分组'}（副本）\`;
  copy.items = Array.isArray(copy.items) ? copy.items.map(item => cloneItemTree(item)) : [];
  return copy;
}

function pasteItemAsChild(value, targetId, source) {
  const location = findItemLocation(value, targetId);
  if (!location) return { ok: false, reason: '目标条目不存在。' };
  const item = cloneItemTree(source);
  if (location.depth + subtreeHeight(item) > MAX_ITEM_DEPTH) {
    return { ok: false, reason: \`粘贴后会超过最多 \${MAX_ITEM_DEPTH} 级。\` };
  }
  if (!Array.isArray(location.item.children)) location.item.children = [];
  location.item.children.push(item);
  return { ok: true, item };
}

function pasteItemAsSibling(value, targetId, source) {
  const location = findItemLocation(value, targetId);
  if (!location) return { ok: false, reason: '目标条目不存在。' };
  const item = cloneItemTree(source);
  if (location.depth + subtreeHeight(item) - 1 > MAX_ITEM_DEPTH) {
    return { ok: false, reason: \`粘贴后会超过最多 \${MAX_ITEM_DEPTH} 级。\` };
  }
  location.siblings.splice(location.index + 1, 0, item);
  return { ok: true, item };
}

function pasteGroupAfter(value, groupId, source) {
  const groups = Array.isArray(value && value.groups) ? value.groups : [];
  const index = groups.findIndex(group => String(group && group.id || '') === String(groupId || ''));
  if (index < 0) return { ok: false, reason: '目标分组不存在。' };
  const group = cloneGroupTree(source);
  groups.splice(index + 1, 0, group);
  return { ok: true, group };
}
\`\`\`

- [ ] **Step 2: Export helpers and reuse group cloning**

Add \`subtreeHeight\`, \`cloneGroupTree\`, \`pasteItemAsChild\`, \`pasteItemAsSibling\`, and \`pasteGroupAfter\` to \`window.HtmlPasteGenCore\`. Replace the existing manual \`duplicate-group\` clone block with:

\`\`\`js
const copy = core.cloneGroupTree(group);
state.document.groups.splice(groupIndex + 1, 0, copy);
\`\`\`

- [ ] **Step 3: Run core tests and verify GREEN**

Run:

\`\`\`bash
node tests/html_paste_gen_test.js
\`\`\`

Expected: all core tests pass, including ID regeneration, shortcut clearing, child/sibling insertion, depth rejection, and group insertion.

### Task 3: Add the context-menu surface, listeners, and editor actions

**Files:**
- Modify: \`html/HtmlPasteGen.html\` markup/styles/runtime sections

- [ ] **Step 1: Add the fixed accessible menu container and styles**

Add this root-level markup after the custom dialogs and before templates:

\`\`\`html
<div id="tree-context-menu" class="tree-context-menu" role="menu" aria-label="树节点快捷操作" hidden></div>
\`\`\`

Add theme-aware styles:

\`\`\`css
.tree-context-menu { position: fixed; z-index: 90; display: grid; min-width: 220px; max-width: min(300px, calc(100vw - 16px)); gap: 3px; padding: 6px; border: 1px solid var(--line-strong); border-radius: 12px; background: var(--surface); box-shadow: 0 18px 50px rgba(16, 24, 40, .24); }
.tree-context-menu[hidden] { display: none; }
.tree-context-menu-title { padding: 7px 9px 6px; overflow: hidden; color: var(--muted); font-size: 12px; font-weight: 800; text-overflow: ellipsis; white-space: nowrap; }
.tree-context-menu-item { min-height: 36px; padding: 7px 9px; border: 0; border-radius: 8px; background: transparent; color: var(--ink); cursor: pointer; text-align: left; }
.tree-context-menu-item:hover, .tree-context-menu-item:focus-visible { background: var(--primary-soft); color: var(--primary); outline: none; }
.tree-context-menu-item:disabled { color: var(--subtle); cursor: not-allowed; opacity: .65; }
\`\`\`

- [ ] **Step 2: Add clipboard/context state and menu rendering helpers**

Near the existing editor expansion state, add:

\`\`\`js
let treeClipboard = null;
let contextMenuState = null;
\`\`\`

Implement \`closeTreeContextMenu()\`, \`contextMenuButton()\`, \`resolveTreeContext()\`, and \`openTreeContextMenu(event, context)\` using \`createElement\`/ \`textContent\`. Render group actions for \`kind: 'group'\` and item actions for \`kind: 'item'\`; disable paste actions when the clipboard is empty or has the wrong kind. Position with viewport bounds, focus the first enabled menu button, and call \`event.preventDefault()\`.

- [ ] **Step 3: Attach right-click listeners to title controls**

In \`renderGroupList()\`, after creating the group title button, add:

\`\`\`js
select.addEventListener('contextmenu', event => {
  event.preventDefault();
  event.stopPropagation();
  openTreeContextMenu(event, { kind: 'group', groupId: group.id });
});
\`\`\`

In \`renderItemTree()\`, after creating each item title button, add the same listener with \`{ kind: 'item', groupId: group.id, itemId: item.id }\`.

- [ ] **Step 4: Implement action handling and document-level close behavior**

Implement \`handleTreeContextAction(action)\` so it re-resolves \`contextMenuState\`, stores \`core.cloneDocument(target)\` in \`treeClipboard\` for copy actions, calls the matching core paste helper for paste actions, and removes targets with the same confirmation wording as existing actions. Keep the clipboard after paste for repeated use; call existing refresh/save/announce paths, and expand the target group/parent before refresh when a child is pasted.

Add a document click handler that dispatches \`[data-context-action]\` buttons and closes the menu for outside clicks. Extend keydown handling so \`Escape\` closes the tree menu before the network dialog. Menu buttons use \`role="menuitem"\`, \`type="button"\`, and \`data-context-action\`.

- [ ] **Step 5: Run focused tests and verify GREEN**

Run:

\`\`\`bash
node tests/html_paste_gen_test.js
node tests/html_paste_gen_ui_test.js
\`\`\`

Expected: both focused tests pass, including generated-page regression assertions showing no editor context-menu code is embedded in generated runtime.

### Task 4: Regression verification and commit

**Files:**
- Verify: \`html/HtmlPasteGen.html\`, \`tests/html_paste_gen_test.js\`, \`tests/html_paste_gen_ui_test.js\`

- [ ] **Step 1: Run complete project checks**

\`\`\`bash
node tests/html_paste_gen_test.js
node tests/html_paste_gen_ui_test.js
node tests/http_handler_html_paste_test.js
node tests/index_navigation_test.js
node tests/simplewebserver_restart_test.js
node tests/html_paste_bundle_api_test.js
git diff --check
wsl make -B
\`\`\`

Expected: all Node tests exit 0, \`git diff --check\` has no whitespace errors, and WSL build exits 0 with \`Build successful: bin/simplewebserver\`.

- [ ] **Step 2: Stage only feature files and commit**

\`\`\`bash
git add html/HtmlPasteGen.html tests/html_paste_gen_test.js tests/html_paste_gen_ui_test.js
git commit -m "feat: add editor tree context menu"
\`\`\`

Do not stage pre-existing \`.claude/settings.local.json\`, deleted \`html/wiki/sqlite_db/*\`, \`.port\`, \`.superpowers/\`, or \`nul\`.

- [ ] **Step 3: Verify commit and worktree**

\`\`\`bash
git show --stat --oneline --summary HEAD
git status --short
\`\`\`

Expected: the feature commit contains only the three intended files, and unrelated worktree changes remain untouched.

