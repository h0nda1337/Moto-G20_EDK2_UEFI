$ErrorActionPreference = 'Stop'
$testRoot = $PSScriptRoot
$edkIncludes = 'C:\MOTO_G20_WOA_LAB\05_EDK2\edk2\MdePkg\Include'
$platformIncludes = Join-Path (Split-Path (Split-Path $testRoot -Parent) -Parent) 'usb_platform_driver'
& gcc -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -fshort-wchar -I $edkIncludes -I (Join-Path $edkIncludes 'X64') -I $platformIncludes (Join-Path $testRoot 'test_musb.c') -o (Join-Path $testRoot 'test_musb.exe')
if ($LASTEXITCODE -ne 0) { throw 'MUSB test compilation failed' }
& (Join-Path $testRoot 'test_musb.exe') 2>&1 | Tee-Object -FilePath (Join-Path $testRoot 'RESULT.txt')
if ($LASTEXITCODE -ne 0) { throw 'MUSB tests failed' }
