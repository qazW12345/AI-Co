# Probe 01 - OS / CPU / RAM (read-only)
# Sneedworks Researcher - AI-Co environment baseline, 2026-08-08
# Usage: powershell.exe -NoProfile -ExecutionPolicy Bypass -File PROBE_01_OS_CPU_RAM.ps1
$ErrorActionPreference = 'Continue'
Write-Output "=== PROBE 01: OS / CPU / RAM ==="
Write-Output ("Probe run at: " + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz'))

Write-Output "`n--- Operating System (Win32_OperatingSystem) ---"
$os = Get-CimInstance -ClassName Win32_OperatingSystem
$os | Select-Object Caption, Version, BuildNumber, OSArchitecture, ProductType, InstallDate, LastBootUpTime | Format-List

Write-Output "--- Registry product name / edition ---"
try {
  $p = Get-ItemProperty -Path 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
  $p | Select-Object ProductName, EditionID, CurrentBuild, CurrentBuildNumber, DisplayVersion, UBR, InstallationType, BuildLabEx | Format-List
} catch { Write-Output ("registry read failed: " + $_.Exception.Message) }

Write-Output "--- Processor (Win32_Processor) ---"
$cpu = Get-CimInstance -ClassName Win32_Processor
$cpu | Select-Object Name, Manufacturer, NumberOfCores, NumberOfLogicalProcessors, SocketDesignation, VirtualizationFirmwareEnabled, SecondLevelAddressTranslationExtensions, VMMonitorModeExtensions, CurrentClockSpeed, MaxClockSpeed, L2CacheSize, L3CacheSize, Architecture | Format-List

Write-Output "--- Physical memory (Win32_PhysicalMemory, no serials) ---"
Get-CimInstance -ClassName Win32_PhysicalMemory | Select-Object Manufacturer, Capacity, Speed, ConfiguredClockSpeed, DeviceLocator | Format-Table -AutoSize

Write-Output "--- Total physical memory (Win32_ComputerSystem) ---"
$cs = Get-CimInstance -ClassName Win32_ComputerSystem
$cs | Select-Object TotalPhysicalMemory, Model, Manufacturer, HypervisorPresent, NumberOfProcessors, NumberOfLogicalProcessors | Format-List

Write-Output "--- Commit / available memory ---"
$ms = Get-CimInstance -ClassName Win32_OperatingSystem
$ms | Select-Object FreePhysicalMemory, TotalVisibleMemorySize, FreeVirtualMemory, TotalVirtualMemorySize | Format-List
