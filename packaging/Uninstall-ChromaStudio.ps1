# ChromaStudio — удаление установки текущего пользователя.
#
# Удаляет папку программы, ярлык и записи HKCU. Работы пользователя не трогает: проекты, библиотека
# кистей и журнал восстановления в %LOCALAPPDATA%\ChromaStudio остаются (-RemoveUserData удаляет и их).

[CmdletBinding()]
param(
    [string]$Destination = '',
    [switch]$NoRegistry,
    [switch]$RemoveUserData
)

$ErrorActionPreference = 'Stop'
if (-not $Destination) { $Destination = $PSScriptRoot }
$target = [System.IO.Path]::GetFullPath($Destination)
if (-not (Test-Path -LiteralPath (Join-Path $target 'ChromaStudio.exe'))) { throw "Здесь нет установки ChromaStudio: $target" }
$running = Get-Process -Name ChromaStudio -ErrorAction SilentlyContinue |
    Where-Object { $_.Path -and ([System.IO.Path]::GetDirectoryName($_.Path) -ieq $target) }
if ($running) { throw 'ChromaStudio запущен. Закройте его и повторите.' }

$link = Join-Path ([Environment]::GetFolderPath('Programs')) 'ChromaStudio.lnk'
if (-not $NoRegistry) {
    if (Test-Path -LiteralPath $link) { Remove-Item -LiteralPath $link -Force }
    Remove-Item -Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\ChromaStudio' -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -Path 'HKCU:\Software\Classes\ChromaStudio.Project' -Recurse -Force -ErrorAction SilentlyContinue
    $ext = 'HKCU:\Software\Classes\.rawproj'
    if ((Get-ItemProperty -Path $ext -ErrorAction SilentlyContinue).'(default)' -eq 'ChromaStudio.Project') {
        Remove-Item -Path $ext -Recurse -Force
    }
}

# Only what the installer put there.
foreach ($item in 'assets', 'ChromaStudio.exe', 'Uninstall-ChromaStudio.ps1') {
    $path = Join-Path $target $item
    if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Recurse -Force }
}
if (-not (Get-ChildItem -LiteralPath $target -Force)) { Remove-Item -LiteralPath $target -Force }

if ($RemoveUserData) {
    $data = Join-Path $env:LOCALAPPDATA 'ChromaStudio'
    if (Test-Path -LiteralPath $data) { Remove-Item -LiteralPath $data -Recurse -Force }
}
Write-Host "ChromaStudio удалён из $target"
