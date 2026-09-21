param(
    [string]$Expect = 'pass',
    [string]$Executable = $env:CHA_WEBVIEW2_EXECUTABLE,
    [string]$Assets = $env:CHA_NATIVE_ASSETS,
    [string]$PrepareVault = $env:CHA_PREPARE_TEST_VAULT,
    [switch]$Development
)

$ErrorActionPreference = 'Stop'
$repository = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
if (-not $Assets) {
    $Assets = Join-Path $repository 'webapp\dist'
}
if (-not $Executable) {
    foreach ($candidate in @(
            (Join-Path $repository 'build\ninja\CHATest.exe'),
            (Join-Path $repository 'build\ninja\Release\CHATest.exe'),
            (Join-Path $repository 'build\ninja\CHA.exe')
        )) {
        if (Test-Path $candidate) {
            $Executable = $candidate
            break
        }
    }
}
if (-not $Executable) {
    throw 'CHA_WEBVIEW2_EXECUTABLE is required on Windows'
}
if (-not $PrepareVault) {
    foreach ($candidate in @(
            (Join-Path $repository 'build\ninja\cha_prepare_test_vault.exe'),
            (Join-Path $repository 'build\ninja\Release\cha_prepare_test_vault.exe')
        )) {
        if (Test-Path $candidate) {
            $PrepareVault = $candidate
            break
        }
    }
}
if (-not $PrepareVault) {
    throw 'CHA_PREPARE_TEST_VAULT is required on Windows'
}

$env:CHA_WEBVIEW2_EXECUTABLE = $Executable
$env:CHA_NATIVE_ASSETS = $Assets
$env:CHA_PREPARE_TEST_VAULT = $PrepareVault
$env:CHA_NATIVE_DEV_ORIGIN = $(if ($Development) { 'http://127.0.0.1:5173' } else { $null })
$env:CHA_WEBVIEW2_CDP_PORT = $(if ($env:CHA_WEBVIEW2_CDP_PORT) { $env:CHA_WEBVIEW2_CDP_PORT } else { '9222' })
$webapp = Join-Path $repository 'webapp'
$env:NODE_PATH = Join-Path $webapp 'node_modules'

function Invoke-WebView2Probe {
    param([Parameter(Mandatory = $true)] [string]$Grep)

    $previousErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = & npx --prefix $webapp playwright test --config playwright.config.ts --grep $Grep 2>&1 | Out-String
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousErrorActionPreference
    }
    Write-Host $output
    return @{ Code = $exitCode; Output = $output }
}

Push-Location (Join-Path $repository 'tests\native\windows')
try {
    if ($Expect -eq 'fail') {
        $result = Invoke-WebView2Probe 'exits the assertion as failed'
        if ($result.Code -eq 0) {
            throw 'expected the failing WebView2 assertion to exit nonzero'
        }
        $intentional = $result.Output -match 'exits the assertion as failed' -and
            ($result.Output -match 'Received:\s*false' -or $result.Output -match 'toBe\(true\)')
        if (-not $intentional) {
            throw "WebView2 fail probe did not record the intentional assertion (infra failure).`n$($result.Output)"
        }
        Write-Host 'FAIL intentional assertion failure'
        exit 1
    }
    $result = Invoke-WebView2Probe 'runs the packaged application|retains session|blocks Blob documents|routes media requests'
    if ($result.Code -ne 0) { exit $result.Code }
} finally {
    Pop-Location
}
