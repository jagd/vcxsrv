REM @echo off

if "%1"=="nox86" goto skipx86

if exist vcxsrv.*.installer.exe del vcxsrv.*.installer.exe

copy "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Redist\MSVC\14.27.29016\x86\Microsoft.VC142.CRT\msvcp140.dll"
copy "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Redist\MSVC\14.27.29016\x86\Microsoft.VC142.CRT\vcruntime140.dll"
copy "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Redist\MSVC\14.27.29016\debug_nonredist\x86\Microsoft.VC142.DebugCRT\msvcp140d.dll"
copy "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Redist\MSVC\14.27.29016\debug_nonredist\x86\Microsoft.VC142.DebugCRT\vcruntime140d.dll"

C:\Users\wu\scoop\apps\nsis\current\Bin\makensis.exe vcxsrv.nsi

:skipx86
if "%1"=="nox64" goto skipx64

if exist vcxsrv-64.*.installer.exe del vcxsrv-64.*.installer.exe

copy "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Redist\MSVC\14.27.29016\x64\Microsoft.VC142.CRT\msvcp140.dll"
copy "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Redist\MSVC\14.27.29016\x64\Microsoft.VC142.CRT\vcruntime140.dll"
copy "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Redist\MSVC\14.27.29016\x64\Microsoft.VC142.CRT\vcruntime140_1.dll"
copy "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Redist\MSVC\14.27.29016\debug_nonredist\x64\Microsoft.VC142.DebugCRT\msvcp140d.dll"
copy "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Redist\MSVC\14.27.29016\debug_nonredist\x64\Microsoft.VC142.DebugCRT\vcruntime140d.dll"
copy "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Redist\MSVC\14.27.29016\debug_nonredist\x64\Microsoft.VC142.DebugCRT\vcruntime140_1d.dll"

C:\Users\wu\scoop\apps\nsis\current\Bin\makensis.exe vcxsrv-64.nsi
REM C:\Users\wu\scoop\apps\nsis\current\Bin\makensis.exe vcxsrv-64-debug.nsi

del vcruntime140_1.dll
del vcruntime140_1d.dll

:skipx64

del vcruntime140.dll
del vcruntime140d.dll
del msvcp140.dll
del msvcp140d.dll
