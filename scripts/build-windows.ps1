$ErrorActionPreference = 'Stop'

# Ninja needs the compiler and Windows SDK environment even in a normal shell.
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) {
    throw 'Visual Studio with the C++ build tools is required.'
}
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installation) {
    throw 'Visual Studio with the C++ build tools is required.'
}
Import-Module (Join-Path $installation 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $installation -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64'

cmake --preset ninja
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
cmake --build --preset ninja
exit $LASTEXITCODE
