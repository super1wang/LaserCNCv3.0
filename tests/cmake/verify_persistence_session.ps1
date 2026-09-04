param([Parameter(Mandatory=$true)][string]$Executable, [Parameter(Mandatory=$true)][string]$EvidenceRoot, [switch]$Service)
$ErrorActionPreference = 'Stop'
$runRoot = Join-Path $EvidenceRoot ([Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($runRoot) | Out-Null
$database = Join-Path $runRoot 'state.db'
$probeMode = if ($Service) { 'probe-service' } else { 'probe' }
$holdMode = if ($Service) { 'hold-service' } else { 'hold' }
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
    if (-not $process.Start()) { throw '无法启动所有权测试子进程' }
    return $process
}
function Check-Probe([int]$ExpectedExit, [string]$Marker) {
    $probe = Start-Probe $probeMode
    try {
        if (-not $probe.WaitForExit(5000)) { throw '所有权探针超时' }
        $output = $probe.StandardOutput.ReadToEnd() + $probe.StandardError.ReadToEnd()
        if ($probe.ExitCode -ne $ExpectedExit -or -not $output.Contains($Marker)) {
            throw "所有权探针不符合预期：退出 $($probe.ExitCode)，输出 $output"
        }
    } finally {
        if (-not $probe.HasExited) { $probe.Kill(); if (-not $probe.WaitForExit(5000)) { throw '探针清理超时' } }
        $probe.Dispose()
    }
}
foreach ($mode in @('clean', 'killed')) {
    $holder = Start-Probe $holdMode
    try {
        $line = $holder.StandardOutput.ReadLineAsync()
        if (-not $line.Wait(5000) -or $line.Result -ne 'host-session-ready') { throw '持有进程没有就绪' }
        Check-Probe 23 'Persistence.HostAlreadyOwned'
        if ($mode -eq 'clean') {
            $releaseBytes = [Text.Encoding]::ASCII.GetBytes("release`n")
            $holder.StandardInput.BaseStream.Write($releaseBytes, 0, $releaseBytes.Length)
            $holder.StandardInput.BaseStream.Flush()
            if (-not $holder.WaitForExit(5000)) { throw '正常释放超时' }
            if ($holder.ExitCode -ne 0) { throw "正常释放失败：退出 $($holder.ExitCode)，$($holder.StandardError.ReadToEnd())" }
        } else {
            $holder.Kill()
            if (-not $holder.WaitForExit(5000)) { throw '中断进程未退出' }
        }
        Check-Probe 0 'host-session-acquired'
        Write-Output "host-session-$mode-verified"
    } finally {
        if (-not $holder.HasExited) { $holder.Kill(); if (-not $holder.WaitForExit(5000)) { throw '持有进程清理超时' } }
        $holder.Dispose()
    }
}
Write-Output "host-session-process-verified evidence=$runRoot"
