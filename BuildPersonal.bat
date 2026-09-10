@echo off
rem One-click personal build: optimized DLL + AIO folder in build\ALL\aio,
rem identical to BuildDev.bat, then prunes selected features from the AIO so
rem they are not installed at runtime (their .ini / shaders are removed, which
rem is how the base "Core" package already ships without addons), and finally
rem packs the pruned AIO into an installable zip in dist\.
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
echo Pruning excluded features from AIO ^(%EXCLUDE_FEATURES:|=, %^) and packing zip...
powershell -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference='Stop'; $root=('%~dp0').TrimEnd('\'); $aio=Join-Path $root 'build\ALL\aio'; if(!(Test-Path -LiteralPath $aio)){ Write-Host ('BuildPersonal: AIO folder not found at ' + $aio); exit 1 }; $aioRoot=(Resolve-Path -LiteralPath $aio).Path.TrimEnd('\'); $exclude='%EXCLUDE_FEATURES%'.Split('|'); $dirs=New-Object System.Collections.Generic.HashSet[string]; foreach($f in $exclude){ $src=Join-Path $root ('features\' + $f); if(!(Test-Path -LiteralPath $src)){ Write-Host ('  skip (no such feature): ' + $f); continue }; $base=(Resolve-Path -LiteralPath $src).Path; Get-ChildItem -LiteralPath $src -Recurse -File | Where-Object { $_.Name -ne 'CORE' } | ForEach-Object { $rel=$_.FullName.Substring($base.Length + 1); $t=Join-Path $aioRoot $rel; if(Test-Path -LiteralPath $t){ Remove-Item -LiteralPath $t -Force; Write-Host ('  pruned ' + $rel) }; [void]$dirs.Add((Split-Path -Parent $t)) } }; foreach($d0 in ($dirs | Sort-Object -Property Length -Descending)){ $d=$d0; while($d.Length -gt $aioRoot.Length -and (Test-Path -LiteralPath $d) -and @(Get-ChildItem -LiteralPath $d -Force).Count -eq 0){ Remove-Item -LiteralPath $d -Force; Write-Host ('  removed empty dir ' + $d.Substring($aioRoot.Length + 1)); $d=Split-Path -Parent $d } }; $dist=Join-Path $root 'dist'; if(!(Test-Path -LiteralPath $dist)){ New-Item -ItemType Directory -Path $dist | Out-Null }; $zip=Join-Path $dist ('CommunityShaders-Personal-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.zip'); if(Test-Path -LiteralPath $zip){ Remove-Item -LiteralPath $zip -Force }; $sz=Get-Command 7z -ErrorAction SilentlyContinue; if($sz){ & $sz.Source a -tzip -bso0 -bsp0 -- $zip (Join-Path $aioRoot '*') | Out-Null; if($LASTEXITCODE -ne 0){ throw '7z failed' } } else { Compress-Archive -Path (Join-Path $aioRoot '*') -DestinationPath $zip -CompressionLevel Optimal -Force }; Write-Host ('  wrote ' + $zip + '  (' + [math]::Round((Get-Item -LiteralPath $zip).Length/1MB,1) + ' MB)')"
set "exit_code=%ERRORLEVEL%"
if not "%exit_code%" == "0" (
    echo BuildPersonal: prune/pack step failed
    endlocal & exit /b %exit_code%
)

echo.
echo BuildPersonal complete: pruned AIO in build\ALL\aio and zip in dist\ ^(excluded: %EXCLUDE_FEATURES:|=, %^)
endlocal & exit /b 0
