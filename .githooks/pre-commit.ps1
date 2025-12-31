$ErrorActionPreference = "Stop"
# Ensure we run at repo root (hooks may run from another cwd)
Set-Location (git rev-parse --show-toplevel)

$files = @(git diff --cached --name-only) |
  Where-Object { $_ -and $_.Trim().Length -gt 0 } |
  ForEach-Object { $_.Trim() } |
  Where-Object { $_ -match '\.(h|hpp|c|cpp|ino|md|txt|json|ini|ps1)$' }

if ($files.Count -eq 0) { exit 0 }

$bad = @()
$utf8Strict = New-Object System.Text.UTF8Encoding($false, $true)
$replacement = [string][char]0xFFFD

foreach ($f in $files) {
  $tmpOut = Join-Path $env:TEMP ("gitshow_" + [Guid]::NewGuid().ToString("N") + ".bin")
  $tmpErr = Join-Path $env:TEMP ("gitshow_" + [Guid]::NewGuid().ToString("N") + ".err")

  $args = @("show", ":$f")
  $p = Start-Process -FilePath "git" -ArgumentList $args -NoNewWindow -PassThru -Wait -RedirectStandardOutput $tmpOut -RedirectStandardError $tmpErr

  if ($p.ExitCode -ne 0) {
    Remove-Item -Force $tmpOut,$tmpErr -ErrorAction SilentlyContinue
    continue
  }

  $bytes = [System.IO.File]::ReadAllBytes($tmpOut)
  Remove-Item -Force $tmpOut,$tmpErr -ErrorAction SilentlyContinue

  try {
    $text = $utf8Strict.GetString($bytes)
  } catch {
    $bad += "$f (invalid UTF-8 bytes in staged content)"
    continue
  }

  if ($text.Contains($replacement)) {
    $bad += "$f (contains U+FFFD replacement character)"
    continue
  }

  if (-not ($bytes.Length -gt 0 -and $bytes[$bytes.Length - 1] -eq 0x0A)) {
    $bad += "$f (missing final newline)"
    continue
  }
}

if ($bad.Count -gt 0) {
  Write-Host ""
  Write-Host "ERROR: Encoding / newline guard failed for staged changes:" -ForegroundColor Red
  $bad | ForEach-Object { Write-Host "  - $_" -ForegroundColor Red }
  Write-Host ""
  Write-Host "Fix: remove mojibake chars, save as UTF-8 (no BOM), ensure final newline, then re-stage." -ForegroundColor Yellow
  exit 1
}

exit 0
