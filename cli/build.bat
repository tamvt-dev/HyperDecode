@echo off
setlocal

set "MSYS_BIN=C:\msys64\mingw64\bin"
set "PATH=%MSYS_BIN%;%PATH%"

if not exist bin mkdir bin

gcc -Wall -Wextra -O2 -Wno-cast-function-type ^
  -I..\core\include ^
  -IC:/msys64/mingw64/include/glib-2.0 ^
  -IC:/msys64/mingw64/lib/glib-2.0/include ^
  main.c ^
  ..\core\src\core.c ^
  ..\core\src\decoder.c ^
  ..\core\src\encoder.c ^
  ..\core\src\plugin.c ^
  ..\core\src\lru_cache.c ^
  ..\core\src\logging.c ^
  ..\core\src\errors.c ^
  ..\core\src\crash_handler.c ^
  ..\core\src\score.c ^
  ..\core\src\buffer.c ^
  ..\core\src\pipeline.c ^
  ..\core\src\plugins\aes_plugin.c ^
  ..\core\src\plugins\atbash_plugin.c ^
  ..\core\src\plugins\base64_plugin.c ^
  ..\core\src\plugins\caesar_plugin.c ^
  ..\core\src\plugins\gzip_plugin.c ^
  ..\core\src\plugins\rot13_plugin.c ^
  ..\core\src\plugins\scramble_plugin.c ^
  ..\core\src\plugins\sha256_plugin.c ^
  ..\core\src\plugins\url_plugin.c ^
  ..\core\src\plugins\xor_plugin.c ^
  -LC:/msys64/mingw64/lib ^
  -lglib-2.0 ^
  -lintl ^
  -liconv ^
  -lssl ^
  -lcrypto ^
  -lz ^
  -o bin\hyperdecode.exe

if errorlevel 1 (
  echo.
  echo CLI build failed.
  exit /b 1
)

echo.
echo CLI build successful:
echo   cli\bin\hyperdecode.exe

endlocal
