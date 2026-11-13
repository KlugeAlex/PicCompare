$ErrorActionPreference = 'Stop'
Set-Location -LiteralPath $PSScriptRoot
New-Item -ItemType Directory -Force build | Out-Null
$archive = Join-Path $PSScriptRoot 'build\libwebp.zip'
$url = 'https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-1.6.0-windows-x64.zip'
& curl.exe --fail --location --silent --show-error $url --output $archive
if ($LASTEXITCODE -ne 0) { throw 'Could not download libwebp.' }
if ((Get-FileHash -LiteralPath $archive).Hash -ne '48886F506B21F62E4661F0F4CBFCA19800897C385128E8902542D29A950C93F1') { throw 'Unexpected libwebp archive checksum.' }
Expand-Archive -LiteralPath $archive -DestinationPath (Join-Path $PSScriptRoot 'build') -Force
