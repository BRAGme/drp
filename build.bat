@echo off
REM Builds pcsx2-drp.exe with MSVC. Nothing to install beyond Visual Studio's
REM C++ workload; there are no third-party dependencies.
setlocal

where cl.exe >nul 2>&1
if not errorlevel 1 goto :compile

call :try "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise"
call :try "%ProgramFiles%\Microsoft Visual Studio\2022\Professional"
call :try "%ProgramFiles%\Microsoft Visual Studio\2022\Community"
call :try "%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools"
call :try "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Enterprise"
call :try "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Professional"
call :try "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Community"
call :try "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\BuildTools"

where cl.exe >nul 2>&1
if errorlevel 1 (
  echo Could not find the MSVC compiler.
  echo Install Visual Studio with "Desktop development with C++", or run this
  echo from a "x64 Native Tools Command Prompt".
  exit /b 1
)

:compile
if not exist "%~dp0bin" mkdir "%~dp0bin"
if not exist "%~dp0bin\obj" mkdir "%~dp0bin\obj"

cl /nologo /std:c++17 /EHsc /O2 /MT /W3 /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS ^
   /Fe:"%~dp0bin\drp.exe" /Fo:"%~dp0bin\obj\\" ^
   "%~dp0src\drp_main.cpp" "%~dp0src\mercs.cpp" ^
   ws2_32.lib psapi.lib advapi32.lib user32.lib /link /SUBSYSTEM:WINDOWS
if errorlevel 1 exit /b 1

REM The diagnostic, built alongside so it can never fall behind the tool.
cl /nologo /std:c++17 /EHsc /O2 /MT /W3 /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS ^
   /Fe:"%~dp0bin\probe_pcsx2.exe" /Fo:"%~dp0bin\obj\\" ^
   "%~dp0src\probe_pcsx2.cpp" ^
   ws2_32.lib psapi.lib advapi32.lib user32.lib
if errorlevel 1 exit /b 1

rmdir /s /q "%~dp0bin\obj" 2>nul
echo.
echo Built %~dp0bin\drp.exe and %~dp0bin\probe_pcsx2.exe
exit /b 0

:try
where cl.exe >nul 2>&1
if not errorlevel 1 exit /b 0
if exist "%~1\VC\Auxiliary\Build\vcvars64.bat" call "%~1\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
exit /b 0
