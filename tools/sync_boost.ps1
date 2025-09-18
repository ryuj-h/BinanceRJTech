param(
    [Parameter(Mandatory=$true)] [string] $SourcePath,
    [Parameter(Mandatory=$true)] [string] $Version
)

if (-not (Test-Path $SourcePath)) {
    throw "Source path '$SourcePath' does not exist."
}

$repoRoot = (Get-Item -Path $PSScriptRoot).Parent.FullName
$targetRoot = Join-Path $repoRoot "third_party\boost\$Version"
$targetPath = Join-Path $targetRoot "boost"

if (Test-Path $targetPath) {
    Write-Host "Removing existing Boost payload at $targetPath"
    Remove-Item -Path $targetPath -Recurse -Force
}

New-Item -ItemType Directory -Path $targetPath -Force | Out-Null
Write-Host "Copying Boost from $SourcePath to $targetPath"
Copy-Item -Path (Join-Path $SourcePath '*') -Destination $targetPath -Recurse -Force

$libPath = Join-Path $targetPath 'lib'
if (-not (Test-Path $libPath)) {
    Write-Warning "No prebuilt libraries found in '$libPath'. Build or bootstrap as required."
}

Write-Host "Boost $Version sync complete. Update project include/lib paths if necessary."
