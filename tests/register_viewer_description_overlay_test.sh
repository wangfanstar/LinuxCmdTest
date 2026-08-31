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

require_regex 'var descriptionOverlay'
require_regex 'function regKeyOf\(r\)[[:space:]]*\{'
require_regex 'function fieldKeyOf\(regKey[[:space:]]*, f\)[[:space:]]*\{'
require_regex 'function isDescriptionPath\(relPath\)[[:space:]]*\{'
require_regex 'function mergeDescriptionOverlay\(patch\)[[:space:]]*\{'
require_regex 'function autoLoadRegisterDescriptions\(\)[[:space:]]*\{'
require_regex 'function applyOverlayToRegister\(reg\)[[:space:]]*\{'
require_text 'data.files.filter(isDescriptionPath)'
require_text 'descriptionOverlay'
require_text '_descSource ='
require_text 'applyOverlayToRegister(reg)'
require_text 'autoLoadRegisterDescriptions();'
