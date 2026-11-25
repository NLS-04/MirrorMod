# Copyright (C) 2026 NLS-04. Part of MirrorMod, see NOTICE for the additional attribution term.
# SPDX-License-Identifier: GPL-3.0-or-later

<#
.SYNOPSIS
    Sets the version of MirrorMod in CMakeLists.txt and vcpkg.json, and adds a section for it to CHANGELOG.md.

.EXAMPLE
    ./scripts/Set-Version.ps1 1.1.0
#>
param(
    [Parameter(Mandatory)]
    [ValidatePattern('^\d+\.\d+\.\d+$')]
    [string] $Version
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path $PSScriptRoot -Parent
$Utf8 = New-Object System.Text.UTF8Encoding $false

function Update-File([string] $Path, [string] $Pattern, [string] $Replacement) {
    $Text = [IO.File]::ReadAllText($Path)
    $New = [regex]::Replace($Text, $Pattern, $Replacement)

    if ($New -eq $Text -and -not $Text.Contains($Replacement)) {
        throw "No version found in $Path"
    }

    [IO.File]::WriteAllText($Path, $New, $Utf8)
}

Update-File "$Root/CMakeLists.txt" 'project\(MirrorMod VERSION \d+\.\d+\.\d+' "project(MirrorMod VERSION $Version"
Update-File "$Root/vcpkg.json" '"version": "\d+\.\d+\.\d+"' "`"version`": `"$Version`""

$Changelog = "$Root/CHANGELOG.md"
$Text = [IO.File]::ReadAllText($Changelog)

if ($Text -notmatch "(?m)^## \[$([regex]::Escape($Version))\]") {
    $Date = Get-Date -Format 'yyyy-MM-dd'
    $Section = "## [$Version] - $Date`n`n### Changed`n`n- `n`n"
    $Index = $Text.IndexOf('## [')
    $Text = if ($Index -ge 0) { $Text.Insert($Index, $Section) } else { $Text.TrimEnd() + "`n`n" + $Section }
    [IO.File]::WriteAllText($Changelog, $Text, $Utf8)
}

Write-Host "Version set to $Version. Fill in its section in CHANGELOG.md, then commit and push to main to release it."
