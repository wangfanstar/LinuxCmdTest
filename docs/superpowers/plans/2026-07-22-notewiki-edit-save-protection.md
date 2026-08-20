# NoteWiki Edit Save Protection Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make NoteWiki article editing deterministic so a partially loaded or stale editor cannot display blank content or overwrite the saved article.

**Architecture:** Keep the current single-page editor and lazy Vditor integration, but add a monotonically increasing edit-load sequence as the ownership token for asynchronous reads and saves. Centralize loading/dirty-button state, reject Vditor-to-textarea synchronization while an existing article is still loading, and ignore callbacks that no longer belong to the active editor.

**Tech Stack:** Vanilla JavaScript embedded in `html/wiki/notewiki.html`, Node.js VM-based regression script, POSIX shell test entry point, existing C11 Makefile build.

---

### Task 1: Add a failing NoteWiki editor regression test

**Files:**
- Create: `tests/notewiki_edit_flow_test.sh`
- Test: `html/wiki/notewiki.html` functions `loadEditorFromArticleId`, `syncVditorToTextarea`, and `_doSave`

- [ ] **Step 1: Write the failing test**

Create a Node VM harness that extracts the named production functions from the HTML, supplies a minimal DOM and stubs non-editor helpers. Add these cases:

```js
// Vditor must not replace the loading marker before the read finishes.
context.S.editId = 'article-a';
context.S.editIsNew = false;
context.S.editEngine = 'vditor';
context._contentLoaded = false;
context._vditor = { getValue: () => '' };
context.document.getElementById('edit-textarea').value = '加载中…';
context._doSave({});
assert.equal(context.document.getElementById('edit-textarea').value, '加载中…');
assert.equal(saveRequests.length, 0);

// A late response for an older editor load must be ignored.
const firstRead = deferred();
const secondRead = deferred();
context.fetch = url => url.includes('article-a') ? firstRead.promise : secondRead.promise;
context.loadEditorFromArticleId('article-a');
context.loadEditorFromArticleId('article-b');
secondRead.resolve({ ok: true, json: async () => ({ ok: true, content: 'B' }) });
await flushPromises();
firstRead.resolve({ ok: true, json: async () => ({ ok: true, content: 'A' }) });
await flushPromises();
assert.equal(context.document.getElementById('edit-textarea').value, 'B');
```

The harness must load the actual function bodies from `notewiki.html`; do not duplicate the production implementation in the test. Keep the test independent of browser packages.

- [ ] **Step 2: Run the test to verify it fails**

Run:

```powershell
bash tests/notewiki_edit_flow_test.sh
```

Expected: FAIL because the current save path synchronizes Vditor before checking `_contentLoaded`, and an older read callback can still write into the editor.

### Task 2: Make editor loading and Vditor synchronization state-safe

**Files:**
- Modify: `html/wiki/notewiki.html:2020-2245`

- [ ] **Step 1: Add an edit-load sequence and shared loading-state helper**

Add `_editLoadSeq` beside `_contentLoaded`, and add helpers that disable the textarea and save button while an existing article is loading. `setEditDirty` must also keep the save button disabled whenever `_contentLoaded` is false, while still allowing new articles to save after user input.

```js
var _editLoadSeq = 0;

function isCurrentEditLoad(id, seq) {
  return S.editId === id && _editLoadSeq === seq;
}

function isCurrentEditContext(id, seq) {
  return _editLoadSeq === seq && (id ? S.editId === id : S.editIsNew);
}

function setEditorLoadingState(loading) {
  var ta = document.getElementById('edit-textarea');
  if (ta) ta.disabled = !!loading;
  if (loading) {
    setEditDirty(false);
  } else {
    refreshEditDirtyState();
  }
}
```

- [ ] **Step 2: Guard Vditor-to-textarea synchronization**

In `syncVditorToTextarea`, return before reading Vditor when the active existing article has not finished loading. This preserves the loading marker and prevents an old/empty Vditor instance from becoming the save source.

- [ ] **Step 3: Run the regression test**

Run:

```powershell
bash tests/notewiki_edit_flow_test.sh
```

Expected: the synchronization case passes; the stale-response case remains failing until Task 3.

### Task 3: Isolate article reads and saves to the active editor

**Files:**
- Modify: `html/wiki/notewiki.html:2020-2115` and `html/wiki/notewiki.html:2510-2660`

- [ ] **Step 1: Protect `loadEditorFromArticleId` with the sequence token**

Increment `_editLoadSeq` when a read starts, reset `_editBaseline`, set `_contentLoaded = false`, disable the editor, and capture the sequence in a local variable. In both `.then` and `.catch`, call `isCurrentEditLoad(id, loadSeq)` before mutating title, textarea, Vditor, baseline, backups, or status. Only a current `data.ok` response sets `_contentLoaded = true` and re-enables the editor; failed responses leave saving disabled.

- [ ] **Step 2: Invalidate pending reads when leaving or starting an editor**

Increment `_editLoadSeq` in `cancelEdit` and `doNewArticle`, clear the old baseline, and ensure a new article calls `setEditorLoadingState(false)` after initializing its empty draft.

- [ ] **Step 3: Guard `_doSave` before Vditor synchronization**

Keep authentication enforcement, then reject an existing article when `_contentLoaded` is false before calling `syncVditorToTextarea`. Capture `saveEditId` and `saveLoadSeq` before the request. In the response and catch handlers, ignore UI mutations when `isCurrentEditContext(saveEditId, saveLoadSeq)` is false. This prevents an old save response from changing the active article’s baseline or clearing its backup.

- [ ] **Step 4: Run the regression test to verify green**

Run:

```powershell
bash tests/notewiki_edit_flow_test.sh
```

Expected: PASS for loading-state save protection and stale-read isolation.

### Task 4: Verify the complete change

**Files:**
- Verify: `html/wiki/notewiki.html`
- Verify: `tests/notewiki_edit_flow_test.sh`

- [ ] **Step 1: Run JavaScript syntax validation**

Extract the inline script and run `node --check` against a temporary file, then remove only that temporary file.

- [ ] **Step 2: Run all repository tests**

Run each existing `tests/*_test.sh` script plus `bash tests/notewiki_edit_flow_test.sh` from the repository root. Expected: every script exits 0.

- [ ] **Step 3: Build the Linux-targeted server**

Run `make`. Expected: the `bin/simplewebserver` target builds successfully with no new compiler errors.

- [ ] **Step 4: Review the diff and worktree**

Run `git diff --check`, inspect the NoteWiki diff, and confirm unrelated pre-existing modified files remain untouched. Report any unavailable browser/manual verification explicitly rather than claiming it was performed.
