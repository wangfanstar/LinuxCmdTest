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
require_regex 'var descFiles'
require_regex 'function regKeyOf\(r\)[[:space:]]*\{'
require_regex 'function fieldKeyOf\(regKey[[:space:]]*, f\)[[:space:]]*\{'
require_regex 'function isDescFileName\(name\)[[:space:]]*\{'
require_regex 'function isRegisterFileName\(name\)[[:space:]]*\{'
require_regex 'function companionNameFor\(regName\)[[:space:]]*\{'
require_regex 'function isDescriptionPath\(relPath\)[[:space:]]*\{'
require_regex 'function mergeDescriptionOverlay\(patch\)[[:space:]]*\{'
require_regex 'function autoLoadRegisterDescriptions\(\)[[:space:]]*\{'
require_regex 'function toggleShowDescriptions\(\)[[:space:]]*\{'
require_regex 'function showDescriptionsEnabled\(\)[[:space:]]*\{'
require_regex 'function initShowDescriptions\(\)[[:space:]]*\{'
require_regex 'function setOverlayDisplay\(on\)[[:space:]]*\{'
require_regex 'function initOverlayDisplay\(\)[[:space:]]*\{'
require_regex 'function toggleBnColumn\(force\)[[:space:]]*\{'
require_regex 'function applyOverlayToRegister\(reg\)[[:space:]]*\{'
require_regex 'function addDescFiles\(files[[:space:]]*, opts\)[[:space:]]*\{'
require_regex 'function removeDescFile\(id\)[[:space:]]*\{'
require_regex 'function removeAllDescFiles\(\)[[:space:]]*\{'
require_regex 'function replaceDescFiles\(id[[:space:]]*, files\)[[:space:]]*\{'
require_regex 'function pickDescFilesReplace\(id\)[[:space:]]*\{'
require_regex 'function rebuildDescOverlay\(\)[[:space:]]*\{'
require_regex 'function descFileExistsByName\(name\)[[:space:]]*\{'
require_regex 'function registerHasConflict\(r\)[[:space:]]*\{'
require_regex 'function openHelp\(\)[[:space:]]*\{'
require_regex 'function closeHelp\(\)[[:space:]]*\{'
require_regex 'function pickRegFilesReplace\(id\)[[:space:]]*\{'
require_regex 'function replaceRegFile\(id[[:space:]]*, files\)[[:space:]]*\{'
require_regex 'function buildRstPopup\(resetVal[[:space:]]*, width\)[[:space:]]*\{'
require_regex 'function toggleRstPreview\(el\)[[:space:]]*\{'
require_regex 'function fieldBinHtml\(s[[:space:]]*, e[[:space:]]*, val\)[[:space:]]*\{'
require_regex 'function searchMatch\(r[[:space:]]*, q[[:space:]]*, mode[[:space:]]*, ignoreCase\)[[:space:]]*\{'
require_regex 'function wildcardToRegExp\(p\)[[:space:]]*\{'
require_regex 'function fuzzyMatch\(hay[[:space:]]*, q\)[[:space:]]*\{'
require_regex 'function computeFiltered\(\)[[:space:]]*\{'
require_regex 'function pasteInto\(inputId[[:space:]]*, renderFn\)[[:space:]]*\{'
require_regex 'function clearInput\(inputId[[:space:]]*, renderFn\)[[:space:]]*\{'
require_regex 'function toggleFieldBin\(hand\)[[:space:]]*\{'
require_regex 'function buildBinGrid\(s[[:space:]]*, e[[:space:]]*, val\)[[:space:]]*\{'
require_regex 'function closeBinModal\(\)[[:space:]]*\{'
require_regex 'function renderBinModal\(\)[[:space:]]*\{'
require_regex 'function openBinConverter\(\)[[:space:]]*\{'
require_regex 'function closeBinConverter\(\)[[:space:]]*\{'
require_regex 'function renderBinConverter\(\)[[:space:]]*\{'
require_regex 'function openDescStats\(\)[[:space:]]*\{'
require_regex 'function closeDescStats\(\)[[:space:]]*\{'
require_regex 'function computeDescStats\(\)[[:space:]]*\{'
require_regex 'function renderDescStats\(\)[[:space:]]*\{'
require_regex 'function renderDescStatsBlocks\(\)[[:space:]]*\{'
require_regex 'function openDescEditor\(\)[[:space:]]*\{'
require_regex 'function closeDescEditor\(\)[[:space:]]*\{'
require_regex 'function applyDescEditor\(\)[[:space:]]*\{'
require_regex 'function saveDescEditorLocal\(\)[[:space:]]*\{'
require_regex 'function saveDescEditorNetwork\(\)[[:space:]]*\{'
require_text 'id="desc-editor-overlay"'
require_text 'data.files.filter(isDescriptionPath)'
require_text 'descriptionOverlay'
require_text '_descSource ='
require_text 'applyOverlayToRegister(reg)'
require_text 'id="show-descriptions"'
require_text 'id="show-overlay-desc"'
require_text 'id="bn-column"'
require_text 'id="desc-stats-overlay"'
require_text 'onclick="openDescStats()"'
require_text 'pickDescFilesReplace('
require_text 'file-section-header descs'
require_text 'file-section-header regs'
require_text 'id="only-overlaid"'
require_text 'id="only-conflict"'
require_text 'id="help-overlay"'
require_text 'id="search-mode"'
require_text 'id="ignore-case"'
require_text 'id="bin-modal-overlay"'
require_text 'id="bin-converter-overlay"'
require_text 'onclick="toggleFieldBin(this)"'
require_text 'onclick="openBinConverter()"'
require_text 'onlyOvl && r._descSource'
require_text 'onlyConflict && !registerHasConflict(r)'
require_text 'autoLoadRegisterDescriptions();'
