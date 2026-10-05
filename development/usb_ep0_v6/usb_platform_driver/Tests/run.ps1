$ErrorActionPreference = 'Stop'
$testRoot = $PSScriptRoot
$testSource = Join-Path $testRoot 'platform_tests.c'
$testInclude = Join-Path $testRoot 'include'
$testExecutable = Join-Path $testRoot 'platform_tests.exe'
& gcc -std=c11 -Wall -Wextra -Werror -I $testInclude $testSource -o $testExecutable
if ($LASTEXITCODE -ne 0) { throw 'Platform host test compilation failed.' }
$testOutput = & $testExecutable 2>&1
$testExitCode = $LASTEXITCODE
$testOutput | Set-Content -LiteralPath (Join-Path $testRoot 'RESULT.txt') -Encoding utf8
$testOutput
if ($testExitCode -ne 0) { throw "Platform host tests failed with exit code $testExitCode." }
