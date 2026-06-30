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
