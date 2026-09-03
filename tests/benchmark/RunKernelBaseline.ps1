param(
    [string]$Executable = "$PSScriptRoot/../../build/vs2022/tests/Release/lasercnc_kernel_benchmark.exe",
    [int[]]$ObjectCounts = @(1000, 10000, 100000),
    [ValidateRange(2, 64)][int]$Samples = 5,
    [ValidateRange(0, 10)][int]$Warmup = 1,
    [ValidateRange(3, 30)][int]$Cycles = 10,
    [string]$OutputRoot = "$PSScriptRoot/../../build/kernel-baselines"
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path "$PSScriptRoot/../..").Path
$binary = (Resolve-Path -LiteralPath $Executable).Path
foreach ($count in $ObjectCounts) {
    if ($count -notin @(1000, 10000, 100000)) { throw '正式基线只接受 1k、10k、100k 对象。' }
}
if ($ObjectCounts.Count -eq 0 -or @($ObjectCounts | Select-Object -Unique).Count -ne $ObjectCounts.Count) {
    throw '数据集不能为空或重复。'
}
$base = [IO.Path]::GetFullPath($OutputRoot)
$runRoot = Join-Path $base ("run-" + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $runRoot | Out-Null
$cpu = @(Get-CimInstance Win32_Processor | Select-Object Name, NumberOfCores, NumberOfLogicalProcessors)
$os = Get-CimInstance Win32_OperatingSystem | Select-Object Caption, Version, BuildNumber, TotalVisibleMemorySize
$commit = (& git -C $repo rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0) { throw '无法记录 Git 版本。' }
$dirty = @(& git -C $repo status --porcelain)
if ($LASTEXITCODE -ne 0) { throw '无法记录工作区状态。' }
$index = [ordered]@{
    schema_version = 1; started_at = (Get-Date -Format o); status = 'running'
    source_commit = $commit; working_tree_changes = $dirty
    benchmark_source_sha256 = (Get-FileHash "$PSScriptRoot/kernel_benchmark.cpp" -Algorithm SHA256).Hash
    runner_source_sha256 = (Get-FileHash $PSCommandPath -Algorithm SHA256).Hash
    executable = $binary; executable_sha256 = (Get-FileHash $binary -Algorithm SHA256).Hash
    cpu = $cpu; os = $os; object_counts = $ObjectCounts; samples = $Samples; warmup = $Warmup; cycles = $Cycles
    runs = @()
}
$indexPath = Join-Path $runRoot 'index.json'
function Save-Index { $index | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $indexPath -Encoding UTF8 }
Save-Index
# 每一组使用独立进程并串行执行；不清理目录，不修改数据库同步设置。
$cases = @(
    @{ family = 'component'; storage = 'memory'; operations = 3 },
    @{ family = 'component'; storage = 'sqlite'; operations = 3 },
    @{ family = 'gateway'; storage = 'memory'; operations = 9 },
    @{ family = 'gateway'; storage = 'sqlite'; operations = 9 },
    @{ family = 'journal'; storage = 'sqlite'; operations = 3 },
    @{ family = 'lifecycle'; storage = 'memory'; operations = 0 },
    @{ family = 'lifecycle'; storage = 'sqlite'; operations = 0 }
)
try {
    foreach ($count in $ObjectCounts) {
        foreach ($case in $cases) {
            $label = "$($case.family)-$($case.storage)-$count"
            $log = Join-Path $runRoot "$label.log"
            Write-Output "开始 $label"
            $timer = [Diagnostics.Stopwatch]::StartNew()
            & $binary --objects $count --samples $Samples --warmup $Warmup --cycles $Cycles `
                --family $case.family --storage $case.storage --output-root $runRoot > $log 2>&1
            $exitCode = $LASTEXITCODE
            $timer.Stop()
            if ($exitCode -ne 0) { throw "$label 退出码 $exitCode，请检查 $log" }
            $markers = @(Get-Content -LiteralPath $log | Where-Object { $_ -like 'benchmark-verified: *' })
            if ($markers.Count -ne 1) { throw "$label 缺少唯一成功报告。" }
            $reportPath = [IO.Path]::GetFullPath($markers[0].Substring('benchmark-verified: '.Length))
            if (-not $reportPath.StartsWith($runRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
                throw '报告路径超出本轮目录。'
            }
            $report = Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json
            if ($report.metadata.object_count -ne $count -or $report.metadata.family -ne $case.family `
                -or $report.metadata.storage -ne $case.storage -or $report.metadata.build_config -ne 'Release' `
                -or $report.metadata.samples -ne $Samples -or $report.metadata.warmup -ne $Warmup `
                -or $report.metadata.cycles -ne $Cycles -or $report.operations.Count -ne $case.operations) {
                throw "$label 报告元数据不匹配，正式基线要求 Release。"
            }
            foreach ($operation in $report.operations) {
                if ($operation.raw_samples.Count -ne $Samples) { throw "$label 原始样本不完整。" }
            }
            $expectedCycles = 0
            if ($case.family -eq 'lifecycle') { $expectedCycles = $Cycles }
            if ($report.lifecycle.Count -ne $expectedCycles) { throw "$label 生命周期轮数不完整。" }
            $index.runs += [ordered]@{ family = $case.family; storage = $case.storage; object_count = $count
                elapsed_seconds = $timer.Elapsed.TotalSeconds; report = $reportPath; log = $log
                report_sha256 = (Get-FileHash -LiteralPath $reportPath -Algorithm SHA256).Hash }
            Save-Index
            Write-Output "完成 $label，$([Math]::Round($timer.Elapsed.TotalSeconds, 2)) 秒"
        }
    }
    $index.status = 'verified'
} catch {
    $index.status = 'failed'
    $index.error = $_.Exception.Message
    throw
} finally {
    $index.completed_at = Get-Date -Format o
    Save-Index
    Write-Output "基线索引：$indexPath"
}
