$ErrorActionPreference = "Stop"

# staged対象（追加/変更/リネーム）だけ
$files = git diff --cached --name-only --diff-filter=ACMR |
  ForEach-Object { $_.Trim() } |
  Where-Object { $_ -match '\.(h|hpp|c|cpp|ino|md|txt|json|ini)$' }

$bad = @()

foreach ($f in $files) {
  if (!(Test-Path $f)) { continue }

  $bytes = [System.IO.File]::ReadAllBytes($f)

  # UTF-8として不正なバイト列が混ざってたら例外になる（=非UTF-8混入の可能性が高い）
  $utf8 = New-Object System.Text.UTF8Encoding($false, $true)
  try {
    $null = $utf8.GetString($bytes)
  } catch {
    $bad += "$f (invalid UTF-8 sequence)"
  }
}

if ($bad.Count -gt 0) {
  Write-Host ""
  Write-Host "ERROR: Non-UTF8 files detected in staged changes:" -ForegroundColor Red
  $bad | ForEach-Object { Write-Host "  - $_" -ForegroundColor Red }
  Write-Host ""
  Write-Host "Fix: Reopen with correct encoding, then Save with Encoding = UTF-8, and re-stage." -ForegroundColor Yellow
  exit 1
}

exit 0
