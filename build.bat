@echo off
setlocal
set "CFG=%~1"
if "%CFG%"=="" set "CFG=Release"
set "VSW=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSW%" -latest -products * -property installationPath`) do set "VS=%%i"
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
msbuild "%~dp0editor.vcxproj" /p:Configuration=%CFG% /p:Platform=x64 /v:m /nologo
