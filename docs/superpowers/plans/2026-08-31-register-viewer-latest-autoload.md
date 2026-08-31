# Register Viewer latest Auto-load Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `html/register-viewer.html` automatically load every XML/JSON file under the page-relative `./register/latest/` directory on startup.

**Architecture:** Reuse `GET /api/list-register-files` for recursive discovery, filter returned relative paths to safe `latest/` descendants, fetch each file from `./register/<path>`, and pass the resulting `File` objects through the existing `addFiles`/cache/parse pipeline. Manual file picking and the existing network modal remain unchanged.

**Tech Stack:** Static HTML/CSS/JavaScript, browser `fetch`, `Blob`, and `File`; Bash static contract test; existing C server build.

---

### Task 1: Add a failing static contract test

**Files:**
- Create: `tests/register_viewer_latest_autoload_test.sh`
- Read: `html/register-viewer.html`

- [ ] **Step 1: Write the failing test**

Create `tests/register_viewer_latest_autoload_test.sh`:

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

require_regex 'function autoLoadLatestRegisters\(\)[[:space:]]*\{'
require_regex 'function isLatestRegisterPath\(relPath\)[[:space:]]*\{'
require_text "fetch('/api/list-register-files')"
require_text "fetch('./register/"
require_text "paths.filter(isLatestRegisterPath)"
require_text 'Promise.all(requests)'
require_text "addFiles(xmlFiles, 'xml')"
require_text "addFiles(jsonFiles, 'json')"
require_text 'autoLoadLatestRegisters();'
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `bash tests/register_viewer_latest_autoload_test.sh`

Expected: FAIL with a missing `autoLoadLatestRegisters` function because the page does not yet contain the new startup loader.

### Task 2: Implement safe latest-directory discovery and loading

**Files:**
- Modify: `html/register-viewer.html:3009-3123` (network loading helpers and startup initialization)

- [ ] **Step 1: Add the path filter and startup loader before `initFileInputs()`**

Insert this code immediately before the existing `initFileInputs();` call:

```javascript
function isLatestRegisterPath(relPath) {
  var p = String(relPath || '').replace(/\\/g, '/');
  var parts = p.split('/');
  if (parts.shift() !== 'latest' || !parts.length) return false;
  for (var i = 0; i < parts.length; i++) {
    if (!parts[i] || parts[i] === '.' || parts[i] === '..') return false;
  }
  return /\.(xml|json)$/i.test(parts[parts.length - 1]);
}

function autoLoadLatestRegisters() {
  fetch('/api/list-register-files')
    .then(function(resp) {
      if (!resp.ok) throw new Error('HTTP ' + resp.status);
      return resp.json();
    })
    .then(function(data) {
      if (!data || !data.ok || !Array.isArray(data.files))
        throw new Error('接口返回异常');

      var paths = data.files.filter(isLatestRegisterPath);
      if (!paths.length) return;

      var errors = [];
      var requests = paths.map(function(relPath) {
        var normalized = String(relPath).replace(/\\/g, '/');
        var urlPath = normalized.split('/').map(encodeURIComponent).join('/');
        return fetch('./register/' + urlPath)
          .then(function(resp) {
            if (!resp.ok) throw new Error('HTTP ' + resp.status);
            var modified = resp.headers.get('Last-Modified');
            var timestamp = modified ? Date.parse(modified) : 0;
            return resp.blob().then(function(blob) {
              var name = normalized.split('/').pop();
              return new File([blob], name, {
                type: name.toLowerCase().endsWith('.json') ? 'application/json' : 'text/xml',
                lastModified: timestamp && !isNaN(timestamp) ? timestamp : Date.now()
              });
            });
          })
          .catch(function(err) {
            errors.push(normalized + ': ' + err.message);
            return null;
          });
      });

      return Promise.all(requests).then(function(files) {
        var xmlFiles = files.filter(function(file) {
          return file && file.name.toLowerCase().endsWith('.xml');
        });
        var jsonFiles = files.filter(function(file) {
          return file && file.name.toLowerCase().endsWith('.json');
        });
        if (xmlFiles.length) addFiles(xmlFiles, 'xml');
        if (jsonFiles.length) addFiles(jsonFiles, 'json');
        if (errors.length) showToast('latest/ 自动加载完成，失败 ' + errors.length + ' 个');
      });
    })
    .catch(function(err) {
      showToast('latest/ 自动加载失败：' + err.message);
    });
}

initFileInputs();
autoLoadLatestRegisters();
```

- [ ] **Step 2: Run the static test to verify it passes**

Run: `bash tests/register_viewer_latest_autoload_test.sh`

Expected: PASS with no output. The existing `tests/register_viewer_selection_test.sh` must also remain passing.

- [ ] **Step 3: Commit the page loader and regression test**

Run:

```bash
git add html/register-viewer.html tests/register_viewer_latest_autoload_test.sh
git commit -m "feat: auto-load register latest files"
```

### Task 3: Verify the repository build and diff safety

**Files:**
- Read: `Makefile`
- Read: `git diff --check`

- [ ] **Step 1: Run both static tests**

Run: `bash tests/register_viewer_latest_autoload_test.sh; bash tests/register_viewer_selection_test.sh`

Expected: both commands exit 0.

- [ ] **Step 2: Check whitespace and build**

Run: `git diff --check; make`

Expected: no whitespace errors and the C build completes successfully, producing `bin/simplewebserver`.

- [ ] **Step 3: Confirm only intended files changed**

Run: `git status --short`

Expected: the implementation commit contains only `html/register-viewer.html` and `tests/register_viewer_latest_autoload_test.sh`; pre-existing user changes remain untouched.
