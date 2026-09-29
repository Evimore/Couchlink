# Downloads the usbip-win2 installer that InputLine-Setup.exe includes, and
# checks it is exactly the file that was tested. To move to a newer
# usbip-win2: test it with InputLine, then update the version and hash here.
param([string] $OutDir = '.')
$ErrorActionPreference = 'Stop'

$version = '0.9.8.1'
$sha256 = '38CAD6D4432B52D5BB9409D9AD03B72FDFFC4ADA4CD3A48FBECA1A2752A8518A'
$name = "USBip-$version-x64.exe"
$url = "https://github.com/vadimgrn/usbip-win2/releases/download/v.$version/$name"

$path = Join-Path $OutDir $name
Invoke-WebRequest -Uri $url -OutFile $path -UseBasicParsing
$actual = (Get-FileHash -Algorithm SHA256 $path).Hash
if ($actual -ne $sha256) {
  Remove-Item $path
  throw "usbip-win2 installer hash mismatch: expected $sha256, got $actual"
}
Write-Output (Resolve-Path $path).Path
