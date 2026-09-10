@echo off
rem One-click personal build: optimized DLL + AIO folder in build\ALL\aio,
rem identical to BuildDev.bat, then prunes selected features from the AIO so
rem they are not installed at runtime (their .ini / shaders are removed, which
rem is how the base "Core" package already ships without addons).
rem
rem Included: every feature (core and non-core).
rem Excluded: Terrain Helper, HDR Display.
rem
rem Edit EXCLUDE_FEATURES below (feature folder names under features\) to change
rem the set. Never ship these binaries.
setlocal
set "SKIP_CONFIGURE=1"
set "EXCLUDE_FEATURES=Terrain Helper|HDR Display"

call "%~dp0BuildRelease.bat" Dev ALL
set "exit_code=%ERRORLEVEL%"
if not "%exit_code%" == "0" (
    endlocal & exit /b %exit_code%
)

echo.
echo Pruning excluded features from AIO ^(%EXCLUDE_FEATURES:|=, %^)...
powershell -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference='Stop'; $root=('%~dp0').TrimEnd('\'); $aio=Join-Path $root 'build\ALL\aio'; if(!(Test-Path -LiteralPath $aio)){ Write-Host ('BuildPersonal: AIO folder not found at ' + $aio); exit 1 }; $exclude='%EXCLUDE_FEATURES%'.Split('|'); foreach($f in $exclude){ $src=Join-Path $root ('features\' + $f); if(!(Test-Path -LiteralPath $src)){ Write-Host ('  skip (no such feature): ' + $f); continue }; $base=(Resolve-Path -LiteralPath $src).Path; Get-ChildItem -LiteralPath $src -Recurse -File | Where-Object { $_.Name -ne 'CORE' } | ForEach-Object { $rel=$_.FullName.Substring($base.Length + 1); $t=Join-Path $aio $rel; if(Test-Path -LiteralPath $t){ Remove-Item -LiteralPath $t -Force; Write-Host ('  pruned ' + $rel) } } }; do { $empty=@(Get-ChildItem -LiteralPath $aio -Recurse -Directory | Where-Object { @(Get-ChildItem -LiteralPath $_.FullName -Recurse -File).Count -eq 0 }); foreach($d in $empty){ Remove-Item -LiteralPath $d.FullName -Recurse -Force } } while($empty.Count -gt 0)"
set "exit_code=%ERRORLEVEL%"
if not "%exit_code%" == "0" (
    echo BuildPersonal: prune step failed
    endlocal & exit /b %exit_code%
)

echo.
echo BuildPersonal complete: build\ALL\aio ^(excluded: %EXCLUDE_FEATURES:|=, %^)
endlocal & exit /b 0
