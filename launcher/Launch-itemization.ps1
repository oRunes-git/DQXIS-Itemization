$ErrorActionPreference = 'Stop'
$runtimeDll = Join-Path $PSScriptRoot 'itemization-probe.dll'
$runtimeLoader = Join-Path $PSScriptRoot 'load-probe.exe'
$runtimeHistory = Join-Path $PSScriptRoot 'state/item-history.json'
$runtimeLog = Join-Path $PSScriptRoot 'itemization-probe.jsonl'
foreach ($requiredPath in @($runtimeDll, $runtimeLoader, $runtimeHistory)) {
    if (-not (Test-Path -LiteralPath $requiredPath -PathType Leaf)) {
        throw "Missing mod file: $requiredPath. Keep the complete runtime folder together."
    }
}
if (-not ('DQXISItemization.NativeClock' -as [type])) {
    Add-Type -TypeDefinition @'
using System.Runtime.InteropServices;
namespace DQXISItemization {
    public static class NativeClock {
        [DllImport("kernel32.dll")]
        public static extern ulong GetTickCount64();
    }
}
'@
}
$running = @(Get-Process -Name 'DRAGON QUEST XI S' -ErrorAction SilentlyContinue)
if ($running.Count -eq 0) { Start-Process 'steam://rungameid/1295510' -WindowStyle Hidden }
$deadline = (Get-Date).AddSeconds(60)
do {
    $running = @(Get-Process -Name 'DRAGON QUEST XI S' -ErrorAction SilentlyContinue)
    if ($running.Count -eq 1) { break }
    Start-Sleep -Milliseconds 500
} while ((Get-Date) -lt $deadline)
if ($running.Count -ne 1) { throw 'Could not identify one running XI S game.' }
$gameProcess = $running[0]
$processStartTick = [double][DQXISItemization.NativeClock]::GetTickCount64() - ((Get-Date) - $gameProcess.StartTime).TotalMilliseconds
Start-Sleep -Milliseconds 1500
& $runtimeLoader $runtimeDll
if ($LASTEXITCODE -ne 0) { throw 'The mod could not load. Leave the game at the title screen.' }

# LoadLibrary returning is insufficient: the DLL initializes asynchronously.
# Accept activation only from this game's process lifetime, never yesterday's log.
$readyDeadline = (Get-Date).AddSeconds(30)
do {
    $gameProcess.Refresh()
    if ($gameProcess.HasExited) { throw 'XI S exited before the mod became ready.' }
    $activation = $null
    $failure = $null
    $currentBootSeen = $false
    $currentTick = [double][DQXISItemization.NativeClock]::GetTickCount64()
    if (Test-Path -LiteralPath $runtimeLog) {
        $logStream = [IO.File]::Open($runtimeLog, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
        $logReader = New-Object IO.StreamReader($logStream, [Text.Encoding]::UTF8)
        try { $logSnapshot = $logReader.ReadToEnd() } finally { $logReader.Dispose() }
        foreach ($line in ($logSnapshot -split "`n")) {
            if ($line -notmatch '"event":"(runtime_config_loaded|probe_active|probe_initialization_failed)"') { continue }
            try { $entry = $line | ConvertFrom-Json } catch { continue }
            if ([double]$entry.elapsed_ms -lt ($processStartTick - 1000) -or [double]$entry.elapsed_ms -gt ($currentTick + 1000)) { continue }
            if ($entry.event -eq 'runtime_config_loaded') {
                $currentBootSeen = $true
                $activation = $null
                $failure = $null
                continue
            }
            if ($entry.event -eq 'probe_initialization_failed') { $failure = $entry }
            if ($currentBootSeen -and $entry.event -eq 'probe_active') { $activation = $entry }
        }
    }
    if ($failure) { throw "The mod could not initialize: $($failure.reason). Leave the game at the title screen." }
    if ($activation -and $activation.affixes_applied -and $activation.forge_behavior_changed) {
        $module = @($gameProcess.Modules | Where-Object { $_.ModuleName -ieq 'itemization-probe.dll' })
        if ($module.Count -ne 1) { throw 'The mod reported activation, but its module is missing.' }
        if ([IO.Path]::GetFullPath($module[0].FileName) -ine [IO.Path]::GetFullPath($runtimeDll)) {
            throw 'A different itemization runtime is already loaded. Close XI S and use this launcher again.'
        }
        Write-Host 'Itemization is ready. You can now load your adventure log.'
        return
    }
    Start-Sleep -Milliseconds 500
} while ((Get-Date) -lt $readyDeadline)
throw 'The mod did not become ready. Close XI S and check the installation guide before trying again.'
