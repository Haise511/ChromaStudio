# ChromaStudio — установка для текущего пользователя без прав администратора (spec §12).
#
# Копирует ChromaStudio.exe и assets\ (если есть) из папки пакета в %LOCALAPPDATA%\Programs\ChromaStudio,
# создаёт ярлык в меню «Пуск», запись «Программы и компоненты» и связь .rawproj в HKCU.
# Данные, библиотека кистей и журнал восстановления живут отдельно, в %LOCALAPPDATA%\ChromaStudio.
#
#   install.cmd                      — обычная установка
#   Install-ChromaStudio.ps1 -Destination D:\Apps\ChromaStudio -NoShortcut -NoRegistry

[CmdletBinding()]
param(
    [string]$Destination = (Join-Path $env:LOCALAPPDATA 'Programs\ChromaStudio'),
    [switch]$NoShortcut,
    [switch]$NoRegistry
)

$ErrorActionPreference = 'Stop'
$source = $PSScriptRoot
$exe = Join-Path $source 'ChromaStudio.exe'
if (-not (Test-Path -LiteralPath $exe)) { throw "В папке пакета нет ChromaStudio.exe: $source" }

$target = [System.IO.Path]::GetFullPath($Destination)
$running = Get-Process -Name ChromaStudio -ErrorAction SilentlyContinue |
    Where-Object { $_.Path -and ([System.IO.Path]::GetDirectoryName($_.Path) -ieq $target) }
if ($running) { throw 'ChromaStudio запущен из папки установки. Закройте его и повторите.' }

$version = (Get-Item -LiteralPath $exe).VersionInfo.ProductVersion
if (-not $version) { $version = '0.1.0' }

# Files: a fresh copy of assets\, the executable and the uninstaller.
New-Item -ItemType Directory -Force -Path $target | Out-Null
$assets = Join-Path $target 'assets'
if (Test-Path -LiteralPath $assets) { Remove-Item -LiteralPath $assets -Recurse -Force }
if (Test-Path -LiteralPath (Join-Path $source 'assets')) {
    Copy-Item -LiteralPath (Join-Path $source 'assets') -Destination $assets -Recurse
}
New-Item -ItemType Directory -Force -Path (Join-Path $assets 'textures'), (Join-Path $assets 'materials') | Out-Null
Copy-Item -LiteralPath $exe -Destination $target -Force
Copy-Item -LiteralPath (Join-Path $source 'Uninstall-ChromaStudio.ps1') -Destination $target -Force
$installed = Join-Path $target 'ChromaStudio.exe'

if (-not $NoShortcut) {
    $programs = [Environment]::GetFolderPath('Programs')  # the user's own Start menu
    $shell = New-Object -ComObject WScript.Shell
    $link = $shell.CreateShortcut((Join-Path $programs 'ChromaStudio.lnk'))
    $link.TargetPath = $installed
    $link.WorkingDirectory = $target
    $link.Description = 'ChromaStudio — графический редактор'
    $link.Save()
}

if (-not $NoRegistry) {
    $uninstall = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\ChromaStudio'
    New-Item -Path $uninstall -Force | Out-Null
    $command = "powershell.exe -NoProfile -ExecutionPolicy Bypass -File `"$(Join-Path $target 'Uninstall-ChromaStudio.ps1')`""
    Set-ItemProperty -Path $uninstall -Name DisplayName -Value 'ChromaStudio'
    Set-ItemProperty -Path $uninstall -Name DisplayVersion -Value $version
    Set-ItemProperty -Path $uninstall -Name DisplayIcon -Value $installed
    Set-ItemProperty -Path $uninstall -Name InstallLocation -Value $target
    Set-ItemProperty -Path $uninstall -Name UninstallString -Value $command
    Set-ItemProperty -Path $uninstall -Name NoModify -Value 1 -Type DWord
    Set-ItemProperty -Path $uninstall -Name NoRepair -Value 1 -Type DWord

    # .rawproj opens in ChromaStudio for this user only.
    $classes = 'HKCU:\Software\Classes'
    New-Item -Path "$classes\.rawproj" -Force | Out-Null
    Set-ItemProperty -Path "$classes\.rawproj" -Name '(default)' -Value 'ChromaStudio.Project'
    New-Item -Path "$classes\ChromaStudio.Project\shell\open\command" -Force | Out-Null
    Set-ItemProperty -Path "$classes\ChromaStudio.Project" -Name '(default)' -Value 'Проект ChromaStudio'
    New-Item -Path "$classes\ChromaStudio.Project\DefaultIcon" -Force | Out-Null
    Set-ItemProperty -Path "$classes\ChromaStudio.Project\DefaultIcon" -Name '(default)' -Value "`"$installed`",0"
    Set-ItemProperty -Path "$classes\ChromaStudio.Project\shell\open\command" -Name '(default)' -Value "`"$installed`" `"%1`""
}

Write-Host "ChromaStudio $version установлен в $target"
