$ErrorActionPreference = "Stop"
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (!(Test-Path $vswhere)) { throw "vswhere.exe not found." }
$vs = & $vswhere -latest -products * -property installationPath
if (!$vs) { throw "Visual Studio installation not found." }
$msbuild = Join-Path $vs "MSBuild\Current\Bin\MSBuild.exe"
$solution = Join-Path $PSScriptRoot "DLSS5_MFC_UI.sln"
if (!(Test-Path $solution)) { throw "Solution file not found: $solution" }
& $msbuild $solution /t:Rebuild /p:Configuration=Release /p:Platform=x64 /m /v:minimal
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$exe = Join-Path $PSScriptRoot "bin\x64\Release\DLSS5_MFC_UI.exe"
if (Test-Path $exe) { Write-Host "BUILD SUCCESS: $exe" -ForegroundColor Green }
else { throw "Build completed but executable is missing: $exe" }
