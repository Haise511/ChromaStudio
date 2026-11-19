@echo off
rem ChromaStudio: per-user installation, no administrator rights (spec section 12).
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install-ChromaStudio.ps1" %*
if errorlevel 1 pause
