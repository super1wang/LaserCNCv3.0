param([Parameter(Mandatory=$true)][string]$Executable, [Parameter(Mandatory=$true)][string]$EvidenceRoot)
$ErrorActionPreference = 'Stop'
$runRoot = Join-Path $EvidenceRoot ([Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($runRoot) | Out-Null
$database = Join-Path $runRoot 'state.db'
$script:sequence = 0
function Start-Probe([string]$Mode) {
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $Executable
    $info.Arguments = "$Mode `"$database`""
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardInput = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $info
    if (-not $process.Start()) { throw '无法启动析构探针' }
    return $process
}
function Check-Probe([string]$Mode, [int]$Expected, [string]$Marker) {
    $process = Start-Probe $Mode
    try {
        $timeout = if ($Mode.StartsWith('self-') -or $Mode -eq 'active-bootstrap') { 3000 } else { 15000 }
        if (-not $process.WaitForExit($timeout)) { throw "$Mode 超时" }
        $output = $process.StandardOutput.ReadToEnd() + $process.StandardError.ReadToEnd()
        $script:sequence++
        [IO.File]::WriteAllText((Join-Path $runRoot "$script:sequence-$Mode.log"), "exit=$($process.ExitCode)`n$output")
        if ($process.ExitCode -ne $Expected -or -not $output.Contains($Marker)) { throw "$Mode 非预期结果：$output" }
    } finally {
        if (-not $process.HasExited) { $process.Kill(); $process.WaitForExit(5000) | Out-Null }
        $process.Dispose()
    }
}
$holder = Start-Probe 'hold'
$transcript = [Collections.Generic.List[string]]::new()
function Expect-Line([string]$Expected) {
    $line = $holder.StandardOutput.ReadLineAsync()
    if (-not $line.Wait(15000) -or $line.Result -ne $Expected) { throw "没有收到 $Expected" }
    $transcript.Add($line.Result)
}
function Send-Line([string]$Value) {
    $bytes = [Text.Encoding]::ASCII.GetBytes("$Value`n")
    $holder.StandardInput.BaseStream.Write($bytes, 0, $bytes.Length)
    $holder.StandardInput.BaseStream.Flush()
}
try {
    Expect-Line 'ready'
    Check-Probe 'probe' 23 'owned'
    Send-Line 'destroy'
    Expect-Line 'draining'
    Check-Probe 'probe' 23 'owned'
    [IO.File]::WriteAllText((Join-Path $runRoot 'release'), 'release')
    Expect-Line 'destroyed'
    if ($holder.HasExited) { throw '持有进程提前退出，不能证明 Host 级释放' }
    # The holder process is still alive: ownership belongs to Host lifetime, not process exit.
    # 中文翻译：持有进程仍存活，证明所有权跟随 Host 寿命，而非依赖进程退出释放。
    Check-Probe 'probe' 0 'acquired'
    Send-Line 'exit'
    if (-not $holder.WaitForExit(5000) -or $holder.ExitCode -ne 0) { throw '持有进程没有正常结束' }
} finally {
    if (-not $holder.HasExited) { $holder.Kill(); $holder.WaitForExit(5000) | Out-Null }
    [IO.File]::WriteAllText((Join-Path $runRoot 'holder.log'), (($transcript -join "`n") + "`nexit=$($holder.ExitCode)`n" + $holder.StandardError.ReadToEnd()))
    $holder.Dispose()
}
Check-Probe 'self-host' 91 'self-host-delete=1'
Check-Probe 'self-executor' 91 'self-executor-delete'
Check-Probe 'active-bootstrap' 91 'active-bootstrap-delete'
if ($script:sequence -ne 6) { throw '进程矩阵未完整执行六个检查' }
Write-Output "kernel-final-drain-process-verified evidence=$runRoot"
