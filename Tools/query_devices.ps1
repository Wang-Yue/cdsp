$g = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio"
Write-Host "=== RENDER DEVICES ==="
Get-ChildItem "$g\Render" | ForEach-Object {
    $p = "$($_.PsPath)\Properties"
    if (Test-Path $p) {
        Write-Host "--- Render Device ID: $($_.PSChildName) ---"
        $ip = Get-ItemProperty $p
        $ip.PSObject.Properties | ForEach-Object {
            if ($_.Value -is [string] -or $_.Value -is [int]) {
                Write-Host "  $($_.Name) = $($_.Value)"
            }
        }
    }
}
Write-Host "=== CAPTURE DEVICES ==="
Get-ChildItem "$g\Capture" | ForEach-Object {
    $p = "$($_.PsPath)\Properties"
    if (Test-Path $p) {
        Write-Host "--- Capture Device ID: $($_.PSChildName) ---"
        $ip = Get-ItemProperty $p
        $ip.PSObject.Properties | ForEach-Object {
            if ($_.Value -is [string] -or $_.Value -is [int]) {
                Write-Host "  $($_.Name) = $($_.Value)"
            }
        }
    }
}
