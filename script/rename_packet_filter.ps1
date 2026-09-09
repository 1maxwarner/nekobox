# Renames the WinpkFilter / NDISAPI packet-filter driver so it appears as
# "Nekobox Network Filter" everywhere Windows surfaces it: the network
# connection list, the adapter/filter component description, and the service
# entry. The script is idempotent and safe to run after every driver install.
# It requires administrator rights (HKLM writes).

$ErrorActionPreference = 'SilentlyContinue'
$displayName = 'Nekobox Network Filter'
$componentId = 'nt_ndisrd'

function Set-ComponentDescription {
    param([string]$ClassGuid)

    $root = "HKLM:\SYSTEM\CurrentControlSet\Control\Network\{$ClassGuid}"
    Get-ChildItem -Path $root -ErrorAction SilentlyContinue | ForEach-Object {
        $instance = $_.PSPath
        $connection = Join-Path $instance 'Connection'
        $connectionProps = Get-ItemProperty -Path $connection -ErrorAction SilentlyContinue
        $instanceProps = Get-ItemProperty -Path $instance -ErrorAction SilentlyContinue

        $matches = ($connectionProps.ComponentId -ieq $componentId) -or
                   ($instanceProps.ComponentId -ieq $componentId)
        if ($matches) {
            if ($connectionProps) {
                Set-ItemProperty -Path $connection -Name Name -Value $displayName -Force -ErrorAction SilentlyContinue
            }
            Set-ItemProperty -Path $instance -Name Description -Value $displayName -Force -ErrorAction SilentlyContinue
        }
    }
}

# Net class (miniport adapters, exposes a "Connection" subkey) and NetService
# class (NDIS lightweight filters). The component id lives in one of them
# depending on how the WinpkFilter package registered on this machine.
Set-ComponentDescription -ClassGuid '4D36E974-E325-11CE-BFC1-08002BE10318'
Set-ComponentDescription -ClassGuid '4D36E975-E325-11CE-BFC1-08002BE10318'

# The kernel service the ProxiFyre/NDISAPI userland talks to.
$service = 'HKLM:\SYSTEM\CurrentControlSet\Services\ndisrd'
if (Test-Path $service) {
    Set-ItemProperty -Path $service -Name DisplayName -Value $displayName -Force -ErrorAction SilentlyContinue
    Set-ItemProperty -Path $service -Name Description -Value $displayName -Force -ErrorAction SilentlyContinue
}
