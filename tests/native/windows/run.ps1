param(
    [string]$Expect = 'pass',
    [string]$Executable = $env:CHA_WEBVIEW2_EXECUTABLE,
    [string]$Assets = $env:CHA_NATIVE_ASSETS
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

$env:CHA_WEBVIEW2_EXECUTABLE = $Executable
$env:CHA_NATIVE_ASSETS = $Assets
$env:CHA_WEBVIEW2_CDP_PORT = $(if ($env:CHA_WEBVIEW2_CDP_PORT) { $env:CHA_WEBVIEW2_CDP_PORT } else { '9222' })
$webapp = Join-Path $repository 'webapp'

function Invoke-WebView2Probe {
    param([Parameter(Mandatory = $true)] [string]$Grep)

    $output = & npx --prefix $webapp playwright test --config playwright.config.ts --grep $Grep 2>&1 | Out-String
    Write-Host $output
    return @{ Code = $LASTEXITCODE; Output = $output }
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
    $result = Invoke-WebView2Probe 'proves a passing assertion'
    if ($result.Code -ne 0) { exit $result.Code }
} finally {
    Pop-Location
}
