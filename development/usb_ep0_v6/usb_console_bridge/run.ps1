$ErrorActionPreference = 'Stop'
$usbTestRoot = $PSScriptRoot
& 'C:\Users\Angelo\AppData\Local\Programs\Python\Python311\python.exe' -B (Join-Path $usbTestRoot 'test_policy.py')
if ($LASTEXITCODE -ne 0) { throw 'Console policy test failed' }
& 'C:\Users\Angelo\AppData\Local\Programs\Python\Python311\python.exe' -B (Join-Path $usbTestRoot 'test_status.py')
if ($LASTEXITCODE -ne 0) { throw 'Observer test failed' }
$usbEvidence = @('MotoG20UsbConsolePolicy.c','MotoG20UsbConsoleStatus.c','status_test.c','test_status.py','producer_under_test.h','status_test.exe','STATUS_TEST.txt','STATUS_TEST.json','POLICY_TEST.json') | ForEach-Object {
  $usbTestPath = Join-Path $usbTestRoot $_
  [PSCustomObject]@{ path = $usbTestPath; size = (Get-Item -LiteralPath $usbTestPath).Length; sha256 = (Get-FileHash -LiteralPath $usbTestPath -Algorithm SHA256).Hash }
}
$usbEvidence | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $usbTestRoot 'TEST_MANIFEST.json') -Encoding utf8
