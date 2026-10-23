@echo off
rem Builds dist\EchoQueue.dll with llvm-mingw (on PATH, or set CXX).
setlocal
cd /d "%~dp0"
if "%CXX%"=="" for %%c in (x86_64-w64-mingw32-clang++.exe) do set "CXX=%%~$PATH:c"
if "%CXX%"=="" (echo Put llvm-mingw's bin folder on PATH, or set CXX to its x86_64-w64-mingw32-clang++.exe. & exit /b 1)
set CC=%CXX:clang++=clang%
if not exist dist mkdir dist
if not exist build mkdir build
set MH=vendor\minhook\src
for %%f in (%MH%\buffer.c %MH%\hook.c %MH%\trampoline.c %MH%\hde\hde64.c) do (
  "%CC%" -O2 -c %%f -o build\%%~nf.o || exit /b 1
)
"%CXX%" -std=c++17 -O2 -shared -static -Wall -o dist\EchoQueue.dll ^
  src\echo_queue.cpp build\buffer.o build\hook.o build\trampoline.o build\hde64.o ^
  -Wl,--kill-at -luser32 -lkernel32 -lwinhttp
if errorlevel 1 exit /b 1
copy /y EchoQueue.ini dist\EchoQueue.ini >nul
echo built dist\EchoQueue.dll
