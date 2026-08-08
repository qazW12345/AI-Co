# Probe 04 - Virtualization / WSL / hypervisor state (read-only)
# Sneedworks Researcher - AI-Co environment baseline, 2026-08-08
$ErrorActionPreference = 'Continue'
Write-Output "=== PROBE 04: Virtualization / WSL ==="
Write-Output ("Probe run at: " + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz'))

Write-Output "`n--- Hypervisor presence ---"
$cs = Get-CimInstance -ClassName Win32_ComputerSystem
Write-Output ("Model: " + $cs.Model + " | Manufacturer: " + $cs.Manufacturer + " | HypervisorPresent: " + $cs.HypervisorPresent)

Write-Output "`n--- CPU virtualization capability (Win32_Processor) ---"
$cpu = Get-CimInstance -ClassName Win32_Processor
$cpu | Select-Object VirtualizationFirmwareEnabled, SecondLevelAddressTranslationExtensions, VMMonitorModeExtensions | Format-List

Write-Output "--- Optional features relevant to virtualization/WSL (InstallState: 1=Enabled,2=Disabled,3=Absent) ---"
try {
  Get-CimInstance -ClassName Win32_OptionalFeature | Where-Object { $_.Name -match 'Hyper|Linux|Virtual|Contain|Sandbox' } | Select-Object Name, InstallState | Sort-Object Name | Format-Table -AutoSize
} catch { Write-Output ("optional feature query failed: " + $_.Exception.Message) }

Write-Output "--- WSL executables present? ---"
foreach ($n in @('wsl.exe','wslconfig.exe','wslhost.exe','docker.exe','podman.exe','qemu-system-x86_64.exe','VBoxManage.exe','vmrun.exe')) {
  $c = Get-Command $n -ErrorAction SilentlyContinue
  if ($c) { Write-Output ("FOUND  : " + $n + " -> " + $c.Source) } else { Write-Output ("absent : " + $n) }
}
