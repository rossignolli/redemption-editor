@echo off
rem Builds the FFmpeg libraries the render export links (libavcodec, libavformat,
rem libavfilter, libswscale, libswresample, x264, x265, NVENC, AMF, QSV...), the same
rem vcpkg feature set as EVER Enhanced (external\ffmpeg\vcpkg.json), into
rem external\ffmpeg\installed\x64-windows-release. One time, about an hour.
rem
rem   tools\ffmpeg\install_ffmpeg.bat <vcpkg root> [work folder]
rem
rem The work folder (downloads, build trees, packages) defaults to %TEMP%\re-vcpkg.
rem VCPKG_MAX_CONCURRENCY limits the parallel compile jobs (default 4).
setlocal
set "ROOT=%~dp0..\.."
set "VCPKG=%~1"
if "%VCPKG%"=="" (
  echo usage: install_ffmpeg.bat ^<vcpkg root^> [work folder]
  exit /b 2
)
set "WORK=%~2"
if "%WORK%"=="" set "WORK=%TEMP%\re-vcpkg"
if "%VCPKG_MAX_CONCURRENCY%"=="" set VCPKG_MAX_CONCURRENCY=4
if not exist "%VCPKG%\vcpkg.exe" call "%VCPKG%\bootstrap-vcpkg.bat" -disableMetrics
set "VCPKG_DEFAULT_BINARY_CACHE=%WORK%\cache"
if not exist "%VCPKG_DEFAULT_BINARY_CACHE%" mkdir "%VCPKG_DEFAULT_BINARY_CACHE%"
start "" /belownormal /wait /b "%VCPKG%\vcpkg.exe" install --vcpkg-root="%VCPKG%" ^
  --x-manifest-root="%ROOT%\external\ffmpeg" ^
  --x-install-root="%ROOT%\external\ffmpeg\installed" ^
  --overlay-triplets="%ROOT%\external\ffmpeg\triplets" ^
  --triplet=x64-windows-release --host-triplet=x64-windows-release ^
  --downloads-root="%WORK%\downloads" ^
  --x-buildtrees-root="%WORK%\buildtrees" ^
  --x-packages-root="%WORK%\packages" ^
  --clean-after-build --disable-metrics
echo FFMPEG_INSTALL_EXIT=%ERRORLEVEL%
exit /b %ERRORLEVEL%
