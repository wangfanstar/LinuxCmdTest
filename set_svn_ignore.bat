@echo off
:: =============================================================================
::  Set svn:ignore on html/wiki/ to exclude generated/imported directories
::  Dirs already tracked by SVN are removed first (--keep-local)
::  Usage: set_svn_ignore.bat
:: =============================================================================
cd /d "%~dp0"

set "WIKI_DIR=html\wiki"
set "DIRS=adoc_db adoc_html ci_html code_html"

:: ── Step 1: Remove from SVN tracking ──
echo [1/2] Removing from SVN tracking ...
for %%d in (%DIRS%) do (
    svn info "%WIKI_DIR%\%%d" >nul 2>&1
    if not errorlevel 1 (
        echo   Removing: %%d
        svn rm --keep-local --force "%WIKI_DIR%\%%d"
        if errorlevel 1 echo   WARNING: failed to remove %%d
    ) else (
        echo   Not tracked: %%d
    )
)

:: ── Step 2: Set svn:ignore ──
echo.
echo [2/2] Setting svn:ignore on %WIKI_DIR%/ ...
>  "%TEMP%\svn_ignore.txt" echo adoc_db
>> "%TEMP%\svn_ignore.txt" echo adoc_html
>> "%TEMP%\svn_ignore.txt" echo ci_html
>> "%TEMP%\svn_ignore.txt" echo code_html

svn propset svn:ignore -F "%TEMP%\svn_ignore.txt" %WIKI_DIR%\
del "%TEMP%\svn_ignore.txt"

echo.
echo ============================================================
echo Done. You MUST commit the property change to make it permanent:
echo   svn commit %WIKI_DIR% -m "ignore adoc_db adoc_html ci_html code_html"
echo ============================================================
