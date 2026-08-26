# Generated Copy and Network Library Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task with checkpoints.

**Goal:** Add three copy modes to generated HTML, keep editor/network actions usable during long scrolls, show the network library on demand, and export matching HTML plus JSON files to the network library.

**Architecture:** Keep the generated page self-contained and extend its existing runtime with line-aware command rendering and selection validation. Convert the editor’s existing network section into a fixed, scrollable dialog opened from the sticky project toolbar; retain current API endpoints and JSON normalization. Make bundle export preflight both filenames before sequentially saving JSON and HTML.

**Tech Stack:** Standalone HTML/CSS/JavaScript, existing `HtmlPasteGenCore`, Node contract tests, C HTTP server API, WSL `make`.

---

### Task 1: Specify failing contracts for copy modes and network UX

**Files:**
- Modify: `tests/html_paste_gen_test.js`
- Modify: `tests/html_paste_gen_ui_test.js`

- [ ] **Step 1: Add generated-template assertions for copy modes**

Assert the generated template contains `copyLine`, `copySelection`, `copyItem`, line splitting, selection containment checks, and the visible labels `复制选中` and `复制全部`.

- [ ] **Step 2: Add editor UI assertions for sticky actions and hidden dialog**

Assert the source page contains a sticky `project-toolbar`, a `network-library-toggle` with `aria-expanded="false"`, a hidden `network-library-dialog`, dialog close control, Escape handling, and `network-library-toolbar`/search/preview inside the dialog.

- [ ] **Step 3: Add export contract assertions**

Assert the source contains a bundle save function, both `.json` and `.html` filenames, conflict preflight before POST, and two save calls using the existing `/api/html-paste/save` route.

- [ ] **Step 4: Run the focused tests and verify RED**

Run:

```powershell
node tests/html_paste_gen_test.js
node tests/html_paste_gen_ui_test.js
```

Expected: assertions fail because the new copy functions, dialog markup, and bundle export do not yet exist.

### Task 2: Implement generated HTML copy modes

**Files:**
- Modify: `html/HtmlPasteGen.html` in the generated-shell CSS, `createCopyCard`, clipboard helpers, and generated toolbar event section.

- [ ] **Step 1: Add copy-mode styles and accessible controls**

Add `.command-line`, `.copy-mode-actions`, and `.copy-mode-button` styles. Keep command lines selectable, give each line a hover/focus affordance, and keep the three mode buttons keyboard reachable.

- [ ] **Step 2: Render command lines without nested interactive buttons**

Change the generated card copy body to a non-button `div` with `role="button"` and keyboard handling. Render `item.content.split(String.fromCharCode(10))` as block `.command-line` spans separated by newline text nodes. A line click calls `copyLine(item, lineText)`; clicking the surrounding body continues to copy all.

- [ ] **Step 3: Add selected/all actions and selection validation**

Add `copySelection(item, card)` that reads `window.getSelection()`, requires a non-empty selection whose anchor and focus nodes are inside the current `.item-content`, and otherwise announces a warning without writing to the clipboard. Add `copyLine` and route all successful paths through `markCopiedCards([item.id])` so visual feedback remains consistent.

- [ ] **Step 4: Preserve keyboard and batch behavior**

Keep shortcut copy and batch copy unchanged except for the new non-button copy body event handling. Ensure `copyItem` still copies the complete `item.content`, and use labels `复制选中` and `复制全部` in the card action row.

- [ ] **Step 5: Run focused tests and verify GREEN**

Run:

```powershell
node tests/html_paste_gen_test.js
node tests/html_paste_gen_ui_test.js
```

Expected: copy-mode assertions and all existing generated-page assertions pass.

### Task 3: Make editor actions sticky and convert network library to a dialog

**Files:**
- Modify: `html/HtmlPasteGen.html` source CSS and markup around `.project-toolbar` and the network library.
- Modify: `tests/html_paste_gen_ui_test.js` if selectors need exact contract coverage.

- [ ] **Step 1: Make the project toolbar sticky**

Set `.project-toolbar` to `position: sticky; top: 0; z-index` below the quick-actions/toast layers, add a translucent background and shadow, and expose a stable CSS height variable from a small resize synchronizer so lower sticky elements never overlap it.

- [ ] **Step 2: Add the network library entry point**

Add `network-library-toggle` to the project toolbar with `aria-expanded="false"`, and keep existing project JSON import/export controls in this sticky toolbar.

- [ ] **Step 3: Wrap network controls in a hidden fixed dialog**

Wrap `network-library-toolbar` and `network-library-panel` in `network-library-dialog` with `hidden`, `role="dialog"`, `aria-modal="true"`, and a close button. Add a backdrop and an internal scroll container; the dialog must not consume page height while hidden.

- [ ] **Step 4: Implement open/close state and focus behavior**

Add `setNetworkLibraryDialogOpen(open)`, wire the toggle and close button, close on Escape, synchronize `aria-expanded`/`aria-hidden`, and return focus to the toggle. Keep existing search/filter/preview state when closing and reopening.

- [ ] **Step 5: Re-run UI tests and inspect source script syntax**

Run:

```powershell
node tests/html_paste_gen_ui_test.js
node tests/html_paste_gen_test.js
```

Expected: sticky/dialog assertions pass and the embedded `app-logic` script still parses without errors.

### Task 4: Export HTML and JSON as one network-library bundle

**Files:**
- Modify: `html/HtmlPasteGen.html` network export functions and event binding.
- Modify: `tests/html_paste_gen_test.js`

- [ ] **Step 1: Add deterministic bundle filename/content helpers**

Derive `jsonFilename` from the existing safe HTML filename and `htmlFilename` from the same base. Serialize current `state.document` for JSON and call `core.buildGeneratedHtml(state.document)` for HTML.

- [ ] **Step 2: Add conflict preflight**

Before any POST, compare both filenames against `state.networkFiles`. If overwrite is false and either exists, announce the two-file conflict and return without writing. If overwrite is true, show one confirmation dialog.

- [ ] **Step 3: Save both files sequentially with explicit partial-failure reporting**

Create a helper that POSTs `{ name, content, overwrite }` to `/api/html-paste/save`. Save JSON first, then HTML; track completed names and include them in any failure announcement. On success, call `loadNetworkLibrary()` and report both files.

- [ ] **Step 4: Preserve compatibility for callers and labels**

Keep `saveNetworkJson` as the event handler name or update all references atomically, change the visible button label to `导出 HTML + JSON`, and preserve the existing overwrite checkbox semantics.

- [ ] **Step 5: Run the focused tests**

Run:

```powershell
node tests/html_paste_gen_test.js
node tests/html_paste_gen_ui_test.js
node tests/http_handler_html_paste_test.js
```

Expected: all pass, including bundle filename, preflight, and two-save assertions.

### Task 5: Full verification, review, and delivery

**Files:**
- Verify: `html/HtmlPasteGen.html`, `tests/html_paste_gen_test.js`, `tests/html_paste_gen_ui_test.js`

- [ ] **Step 1: Run all repository checks**

Run:

```powershell
node tests/html_paste_gen_test.js
node tests/html_paste_gen_ui_test.js
node tests/http_handler_html_paste_test.js
node tests/index_navigation_test.js
git diff --check
wsl make
```

- [ ] **Step 2: Perform browser smoke checks**

Open the generated page and verify one line copy, selected-text copy, full copy, and copied-card feedback. Open the editor, verify the project toolbar remains visible while scrolling, open/close the network dialog, use Escape, scroll its file list, and verify HTML+JSON export conflict behavior.

- [ ] **Step 3: Commit only feature files**

Stage `html/HtmlPasteGen.html`, the two tests, and any spec/plan files already created. Preserve unrelated worktree changes. Commit with:

```powershell
git commit -m "feat: expand generated copy and network library workflow"
```

- [ ] **Step 4: Push and report evidence**

Push `codex/register-viewer-auto-parse`, report the commit hash, test output, and note that existing generated HTML files must be regenerated to receive the new runtime.
