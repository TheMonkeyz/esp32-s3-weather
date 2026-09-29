# Reads the board's serial output and saves it to serial_log.txt
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
while ((Get-Date) -lt $end) {
  try { $line = $sp.ReadLine(); Write-Host $line; $out += $line } catch {}
}
$sp.Close()
$out | Set-Content serial_log.txt
