# Removes leftover openmix audio devices that are no longer connected.
#
# Windows keeps a registry entry for every distinct USB device it has ever
# seen. Early openmix builds derived their USB product ID from the bus index
# and later from the display name, so those identities changed and Windows
# registered new hardware each time, leaving the old entries behind as
# "2- Openmix - Chat" and so on. Identity is now stable, but the historical
# entries need clearing once.
#
# Run elevated. Run it with openmix NOT running, so the live devices are not
# swept up too (harmless if they are -- they return on next launch).

#Requires -RunAsAdministrator

$ghosts = Get-PnpDevice | Where-Object {
    $_.FriendlyName -match 'penmix' -and $_.Status -ne 'OK'
}

if (-not $ghosts) {
    Write-Host "No leftover openmix devices found." -ForegroundColor Green
    exit 0
}

Write-Host "Removing $($ghosts.Count) leftover device(s):`n"
foreach ($g in $ghosts) {
    Write-Host ("  {0,-34} {1}" -f $g.FriendlyName, $g.InstanceId)
    & pnputil /remove-device $g.InstanceId | Out-Null
    if ($LASTEXITCODE -ne 0) {
        Write-Host "    failed (exit $LASTEXITCODE)" -ForegroundColor Yellow
    }
}

$left = Get-PnpDevice | Where-Object { $_.FriendlyName -match 'penmix' }
Write-Host "`nRemaining openmix devices: $($left.Count)"
$left | Select-Object Status, Class, FriendlyName | Format-Table -AutoSize
