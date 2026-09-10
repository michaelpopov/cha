param(
    [Parameter(Mandatory = $true)]
    [string]$Version,

    [string]$OutputParent,

    [string]$CertificateThumbprint
)

$ErrorActionPreference = 'Stop'

function Invoke-Native {
    param(
        [Parameter(Mandatory = $true)] [string]$File,
        [Parameter(Mandatory = $true)] [string[]]$Arguments,
        [Parameter(Mandatory = $true)] [string]$WorkingDirectory
    )
    Push-Location -LiteralPath $WorkingDirectory
    try {
        & $File @Arguments
        if ($LASTEXITCODE -ne 0) {
            throw "$File failed with exit code $LASTEXITCODE"
        }
    } finally {
        Pop-Location
    }
}

function Remove-TemporaryDirectory {
    param([Parameter(Mandatory = $true)] [string]$Path)

    for ($attempt = 0; $attempt -lt 20; $attempt++) {
        try {
            Remove-Item -LiteralPath $Path -Recurse -Force -ErrorAction Stop
            return
        } catch {
            if ($attempt -eq 19) {
                $identity = [Security.Principal.WindowsIdentity]::GetCurrent().Name
                & icacls.exe $Path /grant:r "${identity}:(OI)(CI)F" /T /C /Q | Out-Null
                try {
                    Remove-Item -LiteralPath $Path -Recurse -Force -ErrorAction Stop
                } catch {
                    Write-Warning "Could not remove temporary directory '$Path': $($_.Exception.Message)"
                }
                return
            }
            Start-Sleep -Milliseconds 250
        }
    }
}

if ($Version -notmatch '^[A-Za-z0-9._-]+$') {
    throw 'Version may contain only letters, digits, dots, underscores, and hyphens.'
}
if (-not [Environment]::Is64BitOperatingSystem) {
    throw 'The x64 CHA package must be built on 64-bit Windows.'
}

$repository = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
if (-not $OutputParent) {
    $OutputParent = Join-Path $repository 'packages'
}
New-Item -ItemType Directory -Path $OutputParent -Force | Out-Null
$OutputParent = [IO.Path]::GetFullPath($OutputParent)
$outputRoot = [IO.Path]::GetPathRoot($OutputParent)
if ($OutputParent.TrimEnd('\') -eq $outputRoot.TrimEnd('\')) {
    throw 'Refusing to use a drive root as the output parent.'
}

$webapp = Join-Path $repository 'webapp'
$nativeBuild = Join-Path $repository 'build\package-windows-x64'
$destination = Join-Path $OutputParent 'CHA-windows-x64'
$archive = Join-Path $OutputParent "CHA-windows-$Version-x64.zip"
$temporary = Join-Path $OutputParent ('.cha-windows-{0}.tmp.{1}' -f $Version, [guid]::NewGuid())
$application = Join-Path $temporary 'CHA-windows-x64'
$testApplication = Join-Path $temporary 'test-application'

try {
    New-Item -ItemType Directory -Path $temporary -Force | Out-Null

    Write-Host '==> Installing locked browser build dependencies'
    Invoke-Native 'npm.cmd' @('ci', '--no-audit') $webapp

    Write-Host '==> Installing the Playwright Chromium browser'
    Invoke-Native 'npx.cmd' @('playwright', 'install', 'chromium') $webapp

    Write-Host '==> Checking generated API types and browser application'
    $generatedSchema = Join-Path $temporary 'schema.d.ts'
    Invoke-Native 'npx.cmd' @(
        'openapi-typescript', '../resources/cha.yaml', '-o', $generatedSchema
    ) $webapp
    $committedSchemaText = (Get-Content -LiteralPath (Join-Path $webapp 'src\api\schema.d.ts') -Raw) -replace "`r`n", "`n"
    $generatedSchemaText = (Get-Content -LiteralPath $generatedSchema -Raw) -replace "`r`n", "`n"
    if ($committedSchemaText -ne $generatedSchemaText) {
        throw 'Generated API types are not up-to-date.'
    }
    Invoke-Native 'npm.cmd' @('run', 'typecheck') $webapp
    Invoke-Native 'npm.cmd' @('run', 'test') $webapp

    Write-Host '==> Building production browser files'
    Invoke-Native 'npm.cmd' @('run', 'build') $webapp

    Write-Host '==> Building x64 Windows application'
    Invoke-Native 'cmake' @(
        '-S', $repository,
        '-B', $nativeBuild,
        '-G', 'Visual Studio 17 2022',
        '-A', 'x64',
        '-DBUILD_TESTING=OFF',
        '-DCMAKE_DISABLE_FIND_PACKAGE_CURL=ON',
        '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded',
        "-DCHA_PACKAGE_VERSION=$Version"
    ) $repository
    Invoke-Native 'cmake' @(
        '--build', $nativeBuild,
        '--config', 'Release',
        '--target', 'cha_windows_app', 'chaweb_app'
    ) $repository

    Write-Host '==> Assembling portable application'
    New-Item -ItemType Directory -Path (Join-Path $application 'web') -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $nativeBuild 'Release\CHA.exe') -Destination $application
    Copy-Item -Path (Join-Path $webapp 'dist\*') -Destination (Join-Path $application 'web') -Recurse

    if (-not (Test-Path -LiteralPath (Join-Path $application 'CHA.exe') -PathType Leaf) -or
        -not (Test-Path -LiteralPath (Join-Path $application 'web\index.html') -PathType Leaf)) {
        throw 'The assembled Windows application is incomplete.'
    }
    $topLevel = @(Get-ChildItem -LiteralPath $application | Sort-Object Name | ForEach-Object Name)
    if (($topLevel -join ',') -ne 'CHA.exe,web') {
        throw "The assembled application has unexpected entries: $($topLevel -join ', ')"
    }
    $privateArtifacts = Get-ChildItem -LiteralPath $application -Recurse -Force | Where-Object {
        -not $_.PSIsContainer -and (
            $_.Name -match '\.(sqlite3?|db)$' -or
            $_.Name -match '(-wal|-shm|-journal|\.cha-lock|openai-auth\.json)$'
        )
    }
    if ($privateArtifacts) {
        throw "Private data leaked into the application: $($privateArtifacts[0].FullName)"
    }
    $runtimeArtifacts = Get-ChildItem -LiteralPath $application -Recurse -Force | Where-Object {
        $_.Name -in @('node_modules', 'node.exe', 'npm.cmd', 'npx.cmd', 'chaweb.exe')
    }
    if ($runtimeArtifacts) {
        throw "A development runtime leaked into the application: $($runtimeArtifacts[0].FullName)"
    }

    if ($CertificateThumbprint) {
        $signTool = Get-Command 'signtool.exe' -ErrorAction SilentlyContinue
        if (-not $signTool) {
            throw 'signtool.exe is required when CertificateThumbprint is provided.'
        }
        Write-Host '==> Signing application'
        Invoke-Native $signTool.Source @(
            'sign', '/sha1', $CertificateThumbprint,
            '/fd', 'SHA256', '/tr', 'http://timestamp.digicert.com',
            '/td', 'SHA256', (Join-Path $application 'CHA.exe')
        ) $repository
    }

    Write-Host '==> Testing the assembled native application'
    $smokeRoot = Join-Path $temporary 'smoke-data'
    $smokeArguments = @('--smoke-test', ('"{0}"' -f $smokeRoot))
    $smoke = Start-Process -FilePath (Join-Path $application 'CHA.exe') -ArgumentList $smokeArguments -PassThru -Wait -WindowStyle Hidden
    if ($smoke.ExitCode -ne 0) {
        throw "The native application smoke test failed with exit code $($smoke.ExitCode)."
    }

    Write-Host '==> Testing production web files through chaweb.exe'
    New-Item -ItemType Directory -Path $testApplication -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $nativeBuild 'Release\chaweb.exe') -Destination $testApplication
    Copy-Item -LiteralPath (Join-Path $repository 'bin\start-cha.bat') -Destination $testApplication
    Copy-Item -LiteralPath (Join-Path $repository 'packaging\linux\cha-config.example') -Destination $testApplication -Recurse
    Copy-Item -LiteralPath (Join-Path $repository 'packaging\linux\import-seed') -Destination $testApplication -Recurse
    Copy-Item -LiteralPath (Join-Path $application 'web') -Destination $testApplication -Recurse
    $previousApplicationRoot = $env:CHA_E2E_APPLICATION_ROOT
    try {
        $env:CHA_E2E_APPLICATION_ROOT = $testApplication
        Invoke-Native 'npx.cmd' @('playwright', 'test', '--project=served') $webapp
    } finally {
        $env:CHA_E2E_APPLICATION_ROOT = $previousApplicationRoot
    }

    Write-Host '==> Writing distribution files'
    if (Test-Path -LiteralPath $archive) {
        Remove-Item -LiteralPath $archive -Force
    }
    Compress-Archive -LiteralPath $application -DestinationPath $archive -CompressionLevel Optimal
    if (Test-Path -LiteralPath $destination) {
        if ([IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($destination)) -ne $OutputParent) {
            throw 'Refusing to replace an application outside the output parent.'
        }
        Remove-Item -LiteralPath $destination -Recurse -Force
    }
    Move-Item -LiteralPath $application -Destination $destination

    Write-Host "Windows application: $destination"
    Write-Host "Windows archive: $archive"
    Write-Host 'Requires the Evergreen Microsoft Edge WebView2 Runtime.'
} finally {
    if (Test-Path -LiteralPath $temporary) {
        Remove-TemporaryDirectory $temporary
    }
}
