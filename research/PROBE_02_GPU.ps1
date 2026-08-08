# Probe 02 - GPU / display (read-only)
# Sneedworks Researcher - AI-Co environment baseline, 2026-08-08
$ErrorActionPreference = 'Continue'
Write-Output "=== PROBE 02: GPU / Display ==="
Write-Output ("Probe run at: " + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz'))

Write-Output "`n--- Video controllers (Win32_VideoController) ---"
Get-CimInstance -ClassName Win32_VideoController | Select-Object Name, AdapterRAM, DriverVersion, DriverDate, VideoModeDescription, CurrentHorizontalResolution, CurrentVerticalResolution, CurrentRefreshRate, Status, PNPDeviceID | Format-List

Write-Output "--- GPU tooling candidates on PATH (read-only) ---"
$names = @('nvidia-smi','nvcc','clinfo','dxc','dxc.exe','vulkaninfo','glslangValidator','glslc','cuobjdump','nsys','ncu')
foreach ($n in $names) {
  $cmd = Get-Command $n -ErrorAction SilentlyContinue
  if ($cmd) { Write-Output ("FOUND  : " + $n + " -> " + $cmd.Source) }
  else { Write-Output ("absent : " + $n) }
}
