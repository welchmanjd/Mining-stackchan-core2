$ErrorActionPreference = "Stop"

# staged対象（追加/変更/リネーム）だけ
$files = git diff --cached --name-only --diff-filter=ACMR |
  ForEach-Object { $_.Trim() } |
  Where-Object { $_ -match '\.(h|hpp|c|cpp|ino|md|txt|json|ini|ps1)$' }

$bad = @()

foreach ($f in $files) {
  if (!(Test-Path $f)) { continue }

  $bytes = [System.IO.File]::ReadAllBytes($f)

  # UTF-8として不正なバイト列が混ざってたら例外になる
  $utf8 = New-Object System.Text.UTF8Encoding($false, $true)
  try {
    $text = $utf8.GetString($bytes)
  } catch {
    $bad += "$f (invalid UTF-8 sequence)"
    continue
  }

  # ガード1: 置換文字( )が含まれたら弾く（文字化けの典型）
  if ($text.Contains([char]0xFFFD)) {
    $bad += "$f (contains replacement character U+FFFD ' ' - possible mojibake)"
    continue
  }

  # ガード2: ファイル末尾が改行で終わってないのは拒否
  if (-not $text.EndsWith("`n")) {
    $bad += "$f (missing final newline)"
    continue
  }
}

if ($bad.Count -gt 0) {
  Write-Host ""
  Write-Host "ERROR: Encoding / newline guard failed for staged changes:" -ForegroundColor Red
  $bad | ForEach-Object { Write-Host "  - $_" -ForegroundColor Red }
  Write-Host ""
  Write-Host "Fix: reopen with correct encoding, save as UTF-8 (no BOM), ensure final newline, then re-stage." -ForegroundColor Yellow
  exit 1
}

exit 0
