@echo off
rem GeaOS process host on Windows (packages/geaos/geaos_process.*): apps as
rem child processes in a job object, talking over a named pipe, that crash,
rem hang, flood the channel or lose their Home. Needs clang-cl (LLVM, or
rem Visual Studio's "C++ Clang tools") on PATH or in GEA_WINDOWS_CLANG_CL.
setlocal
cd /d "%~dp0..\..\.."
set "CLANG_CL=%GEA_WINDOWS_CLANG_CL%"
if "%CLANG_CL%"=="" set "CLANG_CL=clang-cl"
set "OUT=%CD%\packages\geatsc-plugin-gea\dist"
if not exist "%OUT%" mkdir "%OUT%"
set "TMPDIR=%OUT%"
"%CLANG_CL%" /nologo /std:c++17 /EHsc /W4 /WX /D_CRT_SECURE_NO_WARNINGS ^
  /Ipackages\geaos ^
  packages\core\test\test_geaos_process.cpp ^
  packages\geaos\geaos_json.cpp packages\geaos\geaos_manifest.cpp packages\geaos\geaos_package.cpp ^
  packages\geaos\geaos_fs_posix.cpp packages\geaos\geaos_registry.cpp packages\geaos\geaos_install.cpp ^
  packages\geaos\geaos_monitor.cpp packages\geaos\geaos_launcher.cpp packages\geaos\geaos_settings.cpp packages\geaos\geaos_trust.cpp packages\geaos\geaos_crypto.cpp packages\geaos\geaos_process.cpp ^
  /Fo"%OUT%\\" /Fe"%OUT%\test_geaos_process.exe" || exit /b 1
"%OUT%\test_geaos_process.exe"
