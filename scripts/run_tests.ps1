param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Debug",

    [ValidateSet("x64")]
    [string]$Platform = "x64",

    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = Resolve-Path (Join-Path $ScriptDir "..")
$SolutionPath = Join-Path $RepoRoot "DagorAssetsExtractor.sln"
$TestExe = Join-Path $RepoRoot "build\tests\$Configuration\AssetIndexTests.exe"

function Find-MSBuild {
    $roots = @($env:ProgramFiles, ${env:ProgramFiles(x86)}) | Where-Object { $_ }
    $editions = @("Community", "Professional", "Enterprise", "BuildTools")
    $candidates = @()

    foreach ($root in $roots) {
        foreach ($edition in $editions) {
            $candidates += Join-Path $root "Microsoft Visual Studio\2022\$edition\MSBuild\Current\Bin\MSBuild.exe"
        }
    }

    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate) {
            return $candidate
        }
    }

    $vswhereCandidates = @(
        (Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"),
        (Join-Path $env:ProgramFiles "Microsoft Visual Studio\Installer\vswhere.exe")
    ) | Where-Object { $_ }

    foreach ($vswhere in $vswhereCandidates) {
        if (Test-Path -LiteralPath $vswhere) {
            $result = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find "MSBuild\**\Bin\MSBuild.exe" | Select-Object -First 1
            if ($result -and (Test-Path -LiteralPath $result)) {
                return $result
            }
        }
    }

    $command = Get-Command msbuild.exe -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    throw "Visual Studio C++ MSBuild was not found. Install Visual Studio 2022 with the C++ desktop workload or run this from a Developer PowerShell."
}

if (-not $SkipBuild) {
    $msbuild = Find-MSBuild
    & $msbuild $SolutionPath /m /p:Configuration=$Configuration /p:Platform=$Platform
    if ($LASTEXITCODE -ne 0) {
        throw "MSBuild failed with exit code $LASTEXITCODE."
    }
}

if (-not (Test-Path -LiteralPath $TestExe)) {
    throw "Test executable was not found: $TestExe"
}

& $TestExe $RepoRoot
if ($LASTEXITCODE -ne 0) {
    throw "AssetIndexTests failed with exit code $LASTEXITCODE."
}

Write-Host "All tests passed!"
