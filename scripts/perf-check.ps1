<#
.SYNOPSIS
    Reference-machine performance gate (docs/09-performance-charter.md §3), skeleton from M0.

.DESCRIPTION
    Startup: launches Lattice with --quit-when-ready several times, reads the stage times it appends
    to <userdata>/logs/startup.log, and checks the medians against the charter's targets:
      window shown <= 0.5 s, ready <= 1.5 s (both measured from process creation).

    Soak (-Soak): plays the test tone for -SoakSeconds (default 600) at -BufferSize (default 64)
    and passes only with 0 callback overruns and 0 device xruns. Results go to
    <userdata>/logs/soak.log (one progress line per minute, then a result line). Your saved device
    setup is not changed. -DeviceType picks a device type for the run, e.g.
    "Windows Audio (Low Latency Mode)" or "ASIO", and -DeviceName the device (for ASIO, the driver:
    "ASIO4ALL v2" or "FlexASIO").

    Later milestones add the reference-set load, pad-to-sound latency, and benchmark comparisons.

    Run under the charter's test conditions: on AC power, Performance mode, background apps closed.

.EXAMPLE
    scripts\perf-check.ps1
    scripts\perf-check.ps1 -Soak
    scripts\perf-check.ps1 -Soak -BufferSize 128 -DeviceType "Windows Audio (Low Latency Mode)"
    scripts\perf-check.ps1 -Runs 5 -Exe build\windows-release\Lattice_artefacts\Release\Lattice.exe
#>
param(
    [string]$Exe = (Join-Path $PSScriptRoot '..\build\windows-release\Lattice_artefacts\Release\Lattice.exe'),
    [int]$Runs = 3,
    [double]$WindowShownTarget = 0.5,
    [double]$ReadyTarget = 1.5,
    [switch]$Soak,
    [int]$SoakSeconds = 600,
    [int]$BufferSize = 64,
    [double]$SampleRate = 0,
    [string]$DeviceType = '',
    [string]$DeviceName = ''
)
$ErrorActionPreference = 'Stop'

if (-not (Test-Path $Exe)) {
    throw "Lattice not found at $Exe. Build it first: cmake --build --preset windows-release"
}
# A process left stuck while exiting (no window, one thread; seen after an ASIO4ALL driver hang) can't
# be ended until a reboot, and it doesn't block a new instance, so it's reported but ignored.
$stuck = @(Get-Process Lattice -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -eq 0 -and $_.Threads.Count -le 1 })
if ($stuck) { Write-Warning ('Ignoring {0} Lattice process(es) stuck while exiting (PID {1}); a reboot clears them.' -f $stuck.Count, ($stuck.Id -join ', ')) }
if (Get-Process Lattice -ErrorAction SilentlyContinue | Where-Object { $stuck.Id -notcontains $_.Id }) {
    throw 'Lattice is already running. Close it first: only one instance can run, and a second one exits at once.'
}

if ($Soak) {
    $soakLog = Join-Path $env:APPDATA 'Lattice\logs\soak.log'
    $soakArgs = @('--soak-seconds', $SoakSeconds, '--buffer-size', $BufferSize)
    if ($DeviceType) { $soakArgs += @('--device-type', ('"{0}"' -f $DeviceType)) }
    if ($DeviceName) { $soakArgs += @('--device-name', ('"{0}"' -f $DeviceName)) }
    if ($SampleRate -gt 0) { $soakArgs += @('--sample-rate', $SampleRate) }
    $previous = if (Test-Path $soakLog) { @(Get-Content $soakLog).Count } else { 0 }
    "Soak: {0} s at a {1}-sample buffer{2}. Progress is logged to {3} once a minute." -f $SoakSeconds, $BufferSize, $(if ($DeviceType) { " on $DeviceType" } else { '' }), $soakLog
    $p = Start-Process -FilePath $Exe -ArgumentList $soakArgs -PassThru
    if (-not $p.WaitForExit(($SoakSeconds + 120) * 1000)) {
        $p.Kill()
        throw 'The soak did not finish in time'
    }
    $lines = @(Get-Content $soakLog | Select-Object -Skip $previous)
    $lines
    $result = $lines | Where-Object { $_ -match ' \| result \| ' } | Select-Object -Last 1
    if (-not $result) { throw 'The soak wrote no result line' }
    if ($result -match '\| PASS$') { 'SOAK PASS'; exit 0 } else { 'SOAK FAIL'; exit 1 }
}
$log = Join-Path $env:APPDATA 'Lattice\logs\startup.log'

function Get-StageTimes([string]$line) {
    $times = @{}
    foreach ($part in ($line -split '\s\|\s' | Select-Object -Skip 1)) {
        $name, $value = $part -split '=', 2
        $times[$name.Trim()] = [double]::Parse($value, [Globalization.CultureInfo]::InvariantCulture)
    }
    return $times
}

function Get-Median([double[]]$values) {
    $sorted = $values | Sort-Object
    $mid = [int][Math]::Floor($sorted.Count / 2)
    if ($sorted.Count % 2) { return $sorted[$mid] }
    return ($sorted[$mid - 1] + $sorted[$mid]) / 2
}

$shown = @(); $ready = @()
for ($i = 1; $i -le $Runs; $i++) {
    $previousLast = if (Test-Path $log) { @(Get-Content $log)[-1] } else { '' }
    $p = Start-Process -FilePath $Exe -ArgumentList '--quit-when-ready' -PassThru
    if (-not $p.WaitForExit(30000)) {
        $p.Kill()
        throw "Run ${i}: Lattice did not reach 'ready' within 30 s"
    }
    $lines = @(Get-Content $log)
    if ($lines[-1] -eq $previousLast) { throw "Run ${i}: Lattice exited without writing a startup log line" }
    $t = Get-StageTimes $lines[-1]
    if (-not $t.ContainsKey('window shown') -or -not $t.ContainsKey('ready')) {
        throw "Run ${i}: startup log line is missing stages: $($lines[-1])"
    }
    $shown += $t['window shown']; $ready += $t['ready']
    '{0}: window shown {1:N3} s, audio started {2:N3} s, ready {3:N3} s' -f $i, $t['window shown'], $t['audio started'], $t['ready']
    Start-Sleep -Milliseconds 500 # let the previous instance release the audio device
}

$medShown = Get-Median $shown
$medReady = Get-Median $ready
$ok = ($medShown -le $WindowShownTarget) -and ($medReady -le $ReadyTarget)

''
'Median of {0} runs:' -f $Runs
'  window shown  {0:N3} s  (target <= {1} s)  {2}' -f $medShown, $WindowShownTarget, $(if ($medShown -le $WindowShownTarget) { 'PASS' } else { 'FAIL' })
'  ready         {0:N3} s  (target <= {1} s)  {2}' -f $medReady, $ReadyTarget, $(if ($medReady -le $ReadyTarget) { 'PASS' } else { 'FAIL' })
''
'Also run the dropout soak: scripts\perf-check.ps1 -Soak'

if (-not $ok) { exit 1 }
