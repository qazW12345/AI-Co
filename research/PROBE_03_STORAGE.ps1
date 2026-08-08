# Probe 03 - Storage (read-only, no serials)
# Sneedworks Researcher - AI-Co environment baseline, 2026-08-08
$ErrorActionPreference = 'Continue'
Write-Output "=== PROBE 03: Storage ==="
Write-Output ("Probe run at: " + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz'))

Write-Output "`n--- Logical disks (Win32_LogicalDisk, no serials) ---"
Get-CimInstance -ClassName Win32_LogicalDisk | Where-Object { $_.DriveType -eq 3 } | Select-Object DeviceID, VolumeName, FileSystem, @{N='SizeGB';E={[math]::Round($_.Size/1GB,1)}}, @{N='FreeGB';E={[math]::Round($_.FreeSpace/1GB,1)}}, @{N='FreePct';E={if($_.Size){[math]::Round($_.FreeSpace/$_.Size*100,1)}}} | Format-Table -AutoSize

Write-Output "--- Non-fixed logical disks (drive type summary) ---"
Get-CimInstance -ClassName Win32_LogicalDisk | Group-Object DriveType | Select-Object Name, Count | Format-Table -AutoSize

Write-Output "--- Volumes summary (no serials, no names beyond drive letters) ---"
Get-CimInstance -ClassName Win32_Volume | Where-Object { $_.DriveLetter } | Select-Object DriveLetter, FileSystem, @{N='SizeGB';E={[math]::Round($_.Capacity/1GB,1)}}, @{N='FreeGB';E={[math]::Round($_.FreeSpace/1GB,1)}} | Format-Table -AutoSize

Write-Output "--- Disk geometry (Win32_DiskDrive, no serials) ---"
Get-CimInstance -ClassName Win32_DiskDrive | Select-Object Index, Model, MediaType, @{N='SizeGB';E={[math]::Round($_.Size/1GB,1)}}, InterfaceType, Partitions | Format-Table -AutoSize

Write-Output "--- Partition table (no serials) ---"
Get-CimInstance -ClassName Win32_DiskPartition | Select-Object DiskIndex, Index, Name, @{N='SizeGB';E={[math]::Round($_.Size/1GB,1)}}, Type, BootPartition | Format-Table -AutoSize
