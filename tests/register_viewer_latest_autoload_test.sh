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
require_text "if (location.protocol === 'file:')"
require_text '本地打开无法自动扫描 latest/'
require_text 'autoLoadLatestRegisters();'
