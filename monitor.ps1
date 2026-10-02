# Reads the board's serial output and saves it to serial_log.txt
# Stops early when stop.request appears or Q / Esc is pressed in this window (exit code 2).
# While it runs (for tools/harness):
#   serial_live.txt  grows line by line (the whole log is still written to serial_log.txt at the end)
#   serial.send      each line in it is sent to the board's test console (main/testcon.c), then the file is
#                    deleted; the sent line also appears in the logs as "> command"
param([string]$Port = "", [int]$Seconds = 75)
Set-Location $PSScriptRoot
if (-not $Port -and (Test-Path flash_log.txt)) {
  $m = Select-String -Path flash_log.txt -Pattern 'Serial port (COM\d+)' | Select-Object -Last 1
  if ($m) { $Port = $m.Matches[0].Groups[1].Value }
}
if (-not $Port) { $Port = [System.IO.Ports.SerialPort]::GetPortNames() | Select-Object -Last 1 }
Write-Host "Monitoring $Port ..."
Start-Sleep -Seconds 2   # let USB re-enumerate after reset
$sp = New-Object System.IO.Ports.SerialPort $Port, 115200
$sp.DtrEnable = $false; $sp.RtsEnable = $false; $sp.ReadTimeout = 200
$out = New-Object System.Collections.Generic.List[string]
for ($i = 0; $i -lt 5 -and -not $sp.IsOpen; $i++) { try { $sp.Open() } catch { Start-Sleep -Seconds 1 } }
if (-not $sp.IsOpen) { "Could not open $Port" | Tee-Object serial_log.txt; exit 1 }
if (Test-Path "serial.send") { Remove-Item "serial.send" -Force }          # stale commands from an earlier run
$live = New-Object System.IO.StreamWriter((Join-Path $PSScriptRoot "serial_live.txt"), $false, [System.Text.Encoding]::UTF8)
$live.AutoFlush = $true
try { while ([Console]::KeyAvailable) { [void][Console]::ReadKey($true) } } catch {}   # ignore keys pressed before the log started
$end = (Get-Date).AddSeconds($Seconds)
$stopped = $false; $nextCheck = Get-Date
while ((Get-Date) -lt $end) {
  try { $line = $sp.ReadLine(); Write-Host $line; $out.Add($line); $live.WriteLine($line) } catch {}
  if ((Get-Date) -ge $nextCheck) {
    $nextCheck = (Get-Date).AddMilliseconds(200)
    if (Test-Path "stop.request") { Remove-Item "stop.request" -Force; $stopped = $true; break }
    if (Test-Path "serial.send") {
      try {
        $cmds = Get-Content "serial.send" -ErrorAction Stop
        Remove-Item "serial.send" -Force
        foreach ($c in $cmds) {
          if (-not $c.Trim()) { continue }
          $sp.Write($c.Trim() + "`n")
          $note = "> " + $c.Trim()
          Write-Host $note -ForegroundColor Cyan; $out.Add($note); $live.WriteLine($note)
        }
      } catch {}   # still being written: next check
    }
    try {
      while ([Console]::KeyAvailable) {
        $k = [Console]::ReadKey($true).Key
        if ($k -eq 'Q' -or $k -eq 'Escape') { $stopped = $true }
      }
    } catch {}
    if ($stopped) { break }
  }
}
$sp.Close()
$live.Close()
$out | Set-Content serial_log.txt
if ($stopped) { Write-Host "Stopped early."; exit 2 }
