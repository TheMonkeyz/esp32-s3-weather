# Flash helper: waits for a "flash.request" file in this folder, then runs flash.bat.
# Only ever runs flash.bat from this folder. Close this window to stop it.
Set-Location $PSScriptRoot
$Host.UI.RawUI.WindowTitle = "ESP flash helper (close to stop)"
Write-Host "Flash helper running in $PSScriptRoot - waiting for flash.request ..."
while ($true) {
  if (Test-Path "flash.request") {
    $secs = 60
    $txt = (Get-Content "flash.request" -Raw -ErrorAction SilentlyContinue)
    if ($txt -match '^\s*(\d{1,3})\s*$') { $secs = [int]$Matches[1] }
    Remove-Item "flash.request" -Force
    Remove-Item "flash.done" -Force -ErrorAction SilentlyContinue
    $start = Get-Date -Format s
    Set-Content "flash.running" $start
    Write-Host "[$start] Flashing, then logging serial for $secs s ..."
    & cmd.exe /c "flash.bat auto $secs" *> "flash_helper_output.txt"
    $rc = $LASTEXITCODE
    Remove-Item "flash.running" -Force -ErrorAction SilentlyContinue
    Set-Content "flash.done" "exit=$rc started=$start finished=$(Get-Date -Format s)"
    Write-Host "[$(Get-Date -Format s)] Done (exit $rc)"
  }
  Start-Sleep -Seconds 2
}
