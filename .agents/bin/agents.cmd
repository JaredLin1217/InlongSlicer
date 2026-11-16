@echo off
setlocal
pwsh -NoProfile -File "%~dp0..\scripts\agents.ps1" %*
exit /b %ERRORLEVEL%
