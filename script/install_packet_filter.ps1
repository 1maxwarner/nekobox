param(
    [Parameter(Mandatory = $true)]
    [string]$MsiPath,
    [string]$RenameScript = "",
    [switch]$Reinstall
)

$ErrorActionPreference = 'Stop'
$successCodes = @(0, 3010, 1641)

function Test-NdisFilter {
    & netcfg.exe -q nt_ndisrd *> $null
    return ($LASTEXITCODE -eq 0)
}

function Invoke-Msi {
    param([string]$Arguments)
    $process = Start-Process -FilePath 'msiexec.exe' -ArgumentList $Arguments -Wait -PassThru -WindowStyle Hidden
    return $process.ExitCode
}

if (-not (Test-Path -LiteralPath $MsiPath)) {
    throw "Packet Filter MSI was not found: $MsiPath"
}

$logPath = Join-Path (Split-Path -Parent $MsiPath) 'packetfilter-install.log'

if ($Reinstall -or (Test-NdisFilter)) {
    # Detach the old NDIS component first. This is the common recovery path
    # for MSI 1603 after an interrupted update or stale adapter binding.
    & netcfg.exe -v -u nt_ndisrd *> $null
    & net stop ndisrd /y *> $null
}

$installArgs = "/i `"$MsiPath`" /passive /norestart /L*V `"$logPath`""
$exitCode = Invoke-Msi $installArgs

if ($exitCode -notin $successCodes) {
    # A stale product registration can survive netcfg removal. The package
    # uninstall is allowed to return 1605/1614 when no product is present.
    $removeCode = Invoke-Msi "/x `"$MsiPath`" /passive /norestart /L*V `"$logPath`""
    & netcfg.exe -v -u nt_ndisrd *> $null
    $exitCode = Invoke-Msi $installArgs
}

if ($exitCode -notin $successCodes) {
    throw "Packet Filter MSI installation failed with exit code $exitCode. See $logPath"
}

if ($exitCode -in @(3010, 1641)) {
    exit $exitCode
}

if (-not (Test-NdisFilter)) {
    throw "Packet Filter MSI completed but NDISRD is unavailable. Restart Windows and retry."
}

if ($RenameScript -and (Test-Path -LiteralPath $RenameScript)) {
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $RenameScript
}

exit 0
