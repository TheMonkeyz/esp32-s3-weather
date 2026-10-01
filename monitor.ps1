# Reads the board's serial output and saves it to serial_log.txt
# Stops early when stop.request appears or Q / Esc is pressed in this window (exit code 2).
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
$sp.DtrEnable = $false; $sp.RtsEnable = $false; $sp.ReadTimeout = 500
$out = @()
for ($i = 0; $i -lt 5 -and -not $sp.IsOpen; $i++) { try { $sp.Open() } catch { Start-Sleep -Seconds 1 } }
if (-not $sp.IsOpen) { "Could not open $Port" | Tee-Object serial_log.txt; exit 1 }
$end = (Get-Date).AddSeconds($Seconds)
$stopped = $false; $nextCheck = Get-Date
while ((Get-Date) -lt $end) {
  try { $line = $sp.ReadLine(); Write-Host $line; $out += $line } catch {}
  if ((Get-Date) -ge $nextCheck) {
    $nextCheck = (Get-Date).AddMilliseconds(500)
    if (Test-Path "stop.request") { Remove-Item "stop.request" -Force; $stopped = $true; break }
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
$out | Set-Content serial_log.txt
if ($stopped) { Write-Host "Stopped early."; exit 2 }
