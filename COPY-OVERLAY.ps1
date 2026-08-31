[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)]
    [string]$RepositoryRoot
)

$ErrorActionPreference = 'Stop'
$SourceRoot = $PSScriptRoot
$Destination = (Resolve-Path $RepositoryRoot).Path

Get-ChildItem -LiteralPath $SourceRoot -Recurse -File | ForEach-Object {
    $relative = $_.FullName.Substring($SourceRoot.Length).TrimStart('\\','/')
    if ($relative -eq 'COPY-OVERLAY.ps1' -or $relative -eq 'README-FULL-OVERLAY.md' -or $relative -eq 'MANIFEST-SHA256.txt') {
        return
    }
    if ($relative -like 'examples\\*' -or $relative -like 'examples/*') {
        return
    }
    $target = Join-Path $Destination $relative
    $parent = Split-Path -Parent $target
    New-Item -ItemType Directory -Force -Path $parent | Out-Null
    Copy-Item -LiteralPath $_.FullName -Destination $target -Force
    Write-Host $relative
}
