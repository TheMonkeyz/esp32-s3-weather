# Flash helper: waits for a "flash.request" file in this folder, flashes firmware\*.bin,
# then logs the board's serial output. Shows each step live in this window.
# The request file may contain the number of seconds to log (default 60).
# "reboot.request" does the same without flashing: restarts the board (through its test console, the port kept open,
# so the boot log is whole; else esptool's reset) and logs it
# (used to re-run the diagnostics, see docs/DIAGNOSTICS.md).
# Stop the serial log early with "stop.request" or by pressing Q / Esc in this window.
# Only ever flashes the files in .\firmware with .\tools\esptool.exe. Close this window to stop it.
Set-Location $PSScriptRoot
$Host.UI.RawUI.WindowTitle = "ESP flash helper - waiting"

function Stamp { Get-Date -Format "HH:mm:ss" }
function Say($msg, $color = "Gray") {
  Write-Host "[$(Stamp)] $msg" -ForegroundColor $color
  Add-Content -Path "flash_helper.log" -Value "[$(Get-Date -Format s)] $msg" -Encoding ASCII
}
function Status($s) { Set-Content -Path "flash.status" -Value $s -Encoding ASCII }

Say "Flash helper running in $PSScriptRoot" "Cyan"
# Requests left from before this start: whoever wrote them has given up waiting (the harness waits ~4 min), and a
# flash nobody watches is a surprise. Drop them (espforge: a restarted helper once flashed one left an hour earlier;
# its LESSONS L157).
foreach ($old in @("flash.request", "reboot.request", "stop.request", "serial.send")) {
  if (Test-Path $old) { Remove-Item $old -Force; Say "Ignored a $old left from before this start" "Yellow" }
}
Say "Waiting for flash.request / reboot.request ..." "Cyan"
Status "idle"

while ($true) {
  $req = if (Test-Path "flash.request") { "flash.request" } elseif (Test-Path "reboot.request") { "reboot.request" } else { $null }
  if (-not $req) { Start-Sleep -Seconds 2; continue }
  $reboot = $req -eq "reboot.request"

  # ---- read request ----
  $secs = 60
  $txt = (Get-Content $req -Raw -ErrorAction SilentlyContinue)
  if ($txt -match '^\s*(\d{1,4})\s*$') { $secs = [int]$Matches[1] }
  Remove-Item $req -Force
  Remove-Item "flash.done" -Force -ErrorAction SilentlyContinue
  Remove-Item "stop.request" -Force -ErrorAction SilentlyContinue
  $start = Get-Date
  Set-Content "flash.running" (Get-Date -Format s)
  Write-Host ""
  if ($reboot) { Say "=== Reboot request received, no flashing (serial log: $secs s) ===" "Cyan" }
  else { Say "=== Flash request received (serial log: $secs s) ===" "Cyan" }
  $Host.UI.RawUI.WindowTitle = "ESP flash helper - FLASHING"
  Status "flashing"

  $app = Get-Item "firmware\weather_amoled.bin" -ErrorAction SilentlyContinue
  if ($app) { Say ("Firmware: {0:N0} bytes, built {1}" -f $app.Length, $app.LastWriteTime.ToString("HH:mm:ss")) }

  # ---- restart through the test console (reboot.request) ----
  # esptool's hard reset re-enumerates the USB and the first ~2.5 s of boot log are lost; a software restart with the
  # port open keeps them (espforge LESSONS L191, its flash_helper.ps1). esptool only when no boot follows.
  $consoleReboot = $false
  if ($reboot) {
    $dev = Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue |
           Where-Object { $_.PNPDeviceID -match 'VID_303A' -and $_.Name -match '\((COM\d+)\)' } | Select-Object -First 1
    if ($dev -and $dev.Name -match '\((COM\d+)\)') {
      $port = $Matches[1]
      Say "Restarting through the test console on $port (the port stays open: the boot log is kept from its first line)"
      $Host.UI.RawUI.WindowTitle = "ESP flash helper - logging serial ($secs s)"
      & powershell -NoProfile -ExecutionPolicy Bypass -File "monitor.ps1" -Port $port -Seconds $secs -Reboot | Out-Null
      $monExit = $LASTEXITCODE
      if ($monExit -ne 3) { $consoleReboot = $true; $flashSecs = 0; $early = if ($monExit -eq 2) { 1 } else { 0 } }
      else { Say "No restart through the test console: esptool's hard reset instead" "Yellow" }
    }
  }
  if (-not $consoleReboot) {   # esptool: a flash, or a restart the console did not do (unindented)

  # ---- flash ----
  $lines = New-Object System.Collections.Generic.List[string]
  $esptoolArgs = @("--chip", "esp32s3", "-b", "460800", "--before", "default_reset", "--after", "hard_reset",
                   "write_flash", "--flash_mode", "dio", "--flash_freq", "80m", "--flash_size", "16MB",
                   "0x0", "firmware\bootloader.bin", "0x8000", "firmware\partition-table.bin",
                   "0x10000", "firmware\weather_amoled.bin")
  # Two-slot (OTA) layout: reset the boot selection so the board boots the app just written to ota_0
  if (Test-Path "firmware\ota_data_initial.bin") { $esptoolArgs += @("0x610000", "firmware\ota_data_initial.bin") }
  if ($reboot) { $esptoolArgs = @("--chip", "esp32s3", "--before", "default_reset", "--after", "hard_reset", "chip_id") }
  & ".\tools\esptool.exe" @esptoolArgs 2>&1 | ForEach-Object {
    $l = "$_"
    $lines.Add($l)
    if ($l -match 'Serial port|Chip is|Writing at .*\(100 %\)|Wrote |Hash of data|Hard resetting|error|Error|failed') {
      Write-Host "    $l" -ForegroundColor DarkGray
    }
  }
  $rc = $LASTEXITCODE
  $lines | Set-Content -Path "flash_log.txt" -Encoding ASCII
  $port = ($lines | Select-String -Pattern 'Serial port (COM\d+)' | Select-Object -Last 1).Matches.Groups[1].Value
  $flashSecs = [int]((Get-Date) - $start).TotalSeconds

  if ($rc -ne 0) {
    Say "$(if ($reboot) {"REBOOT"} else {"FLASH"}) FAILED (exit $rc) after $flashSecs s - last lines:" "Red"
    $lines | Select-Object -Last 6 | ForEach-Object { Write-Host "    $_" -ForegroundColor Red }
    Say "Tip: if no port was found, hold BOOT, tap RESET, release BOOT, and request again." "Yellow"
    Status "flash_failed"
    Set-Content "flash.done" "exit=$rc stage=flash started=$(Get-Date $start -Format s) finished=$(Get-Date -Format s)"
    Remove-Item "flash.running" -Force -ErrorAction SilentlyContinue
    [console]::beep(400, 600)
    $Host.UI.RawUI.WindowTitle = "ESP flash helper - FLASH FAILED (waiting)"
    continue
  }
  Say "$(if ($reboot) {"REBOOT"} else {"FLASH"}) OK on $port in $flashSecs s - board is restarting" "Green"
  [console]::beep(1000, 150)

  # ---- serial log ----
  Status "logging"
  $Host.UI.RawUI.WindowTitle = "ESP flash helper - logging serial ($secs s)"
  Say "Logging serial output for $secs s (saved to serial_log.txt). Press Q or Esc to stop early ..."
  & powershell -NoProfile -ExecutionPolicy Bypass -File "monitor.ps1" -Port $port -Seconds $secs | Out-Null
  $early = if ($LASTEXITCODE -eq 2) { 1 } else { 0 }

  }   # (esptool)
  if ($early) { Say "Serial log stopped early" "Yellow" }

  # ---- summary ----
  $log = Get-Content "serial_log.txt" -ErrorAction SilentlyContinue
  $errs = @($log | Where-Object { $_ -match '^E \(' }).Count
  $warns = @($log | Where-Object { $_ -match '^W \(' }).Count
  $resets = @($log | Where-Object { $_ -match 'rst:0x' }).Count
  $color = if ($errs -gt 0 -or $resets -gt 0) { "Yellow" } else { "Green" }
  Say ("Serial log saved: {0} lines, {1} errors, {2} warnings, {3} resets" -f $log.Count, $errs, $warns, $resets) $color
  $total = [int]((Get-Date) - $start).TotalSeconds
  Set-Content "flash.done" "exit=0 port=$port flash_s=$flashSecs total_s=$total errors=$errs warnings=$warns resets=$resets stopped_early=$early started=$(Get-Date $start -Format s) finished=$(Get-Date -Format s)"
  Remove-Item "flash.running" -Force -ErrorAction SilentlyContinue
  Status "idle"
  Say "=== Done in $total s. Waiting for the next flash.request ===" "Cyan"
  [console]::beep(1200, 120); [console]::beep(1500, 120)
  $Host.UI.RawUI.WindowTitle = "ESP flash helper - waiting"
}
