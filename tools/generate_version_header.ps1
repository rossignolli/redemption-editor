param(
    [Parameter(Mandatory = $true)]
    [string]$VersionFile,

    [Parameter(Mandatory = $true)]
    [string]$OutputFile
)

$version = (Get-Content -Raw -LiteralPath $VersionFile).Trim()
if ($version -notmatch '^[0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z.-]+)?$') {
    throw "Invalid replay editor version: '$version'"
}

$header = @"
#pragma once

// Generated from VERSION by tools/generate_version_header.ps1.
// Change VERSION instead of editing this file directly.
namespace replay::build
{
    inline constexpr char kVersion[] = "$version";
    inline constexpr char kDisplayName[] = "Redemption Editor v$version";
}
"@

$header += [Environment]::NewLine
$current = if (Test-Path -LiteralPath $OutputFile) {
    Get-Content -Raw -LiteralPath $OutputFile
} else {
    $null
}

if ($current -cne $header) {
    $outputDirectory = Split-Path -Parent $OutputFile
    if ($outputDirectory) {
        New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
    }
    [System.IO.File]::WriteAllText($OutputFile, $header)
}
