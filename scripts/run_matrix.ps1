# run_matrix.ps1 - run each repro program x combo many times, log everything, write <OutDir>\summary.md
# Example: scripts\run_matrix.ps1 -Invocations 50 -Combos batch-none,inter-none,inter-hip,inter-vk -OutDir results\myrun
param(
    [string]$BuildDir = "$PSScriptRoot\..\build",
    [int]$Invocations = 20,
    [int]$Runs = 50,
    [string[]]$Combos = @('batch-none','inter-none','inter-hip','inter-vk','inter-all'),
    [string]$ExtraArgs = '',
    [string]$OutDir = "$PSScriptRoot\..\results\run"
)

if ($env:HIP_PATH) { $env:PATH = (Join-Path $env:HIP_PATH "bin") + ";" + $env:PATH }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$programs = @('repro_d3d12_fence', 'repro_opaque_win32')
$rows = @()
$envHeader = $null

foreach ($prog in $programs) {
    $exe = Get-ChildItem -Path $BuildDir -Recurse -Filter "$prog.exe" -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty FullName
    if (-not $exe) { "NO EXE for $prog in $BuildDir"; continue }
    foreach ($combo in $Combos) {
        $s = @{ clean=0; mismatched=0; hung=0; importfail=0; bad=0; stale=0; runs=0 }
        for ($i = 1; $i -le $Invocations; $i++) {
            $log = Join-Path $OutDir "${prog}_${combo}_${i}.log"
            $out = "$log.out"; $err = "$log.err"
            Remove-Item $out, $err -ErrorAction SilentlyContinue
            if ($combo -eq 'batch-none') { $comboArgs = @('--pattern', 'batch', '--sync', 'none') }
            else { $comboArgs = @('--pattern', 'interleave', '--sync', ($combo -replace '^inter-', '')) }
            $runArgs = @('--runs', "$Runs") + $comboArgs
            if ($ExtraArgs) { $runArgs += ($ExtraArgs -split '\s+' | Where-Object { $_ }) }
            $p = Start-Process -FilePath $exe -ArgumentList $runArgs `
                -NoNewWindow -PassThru -RedirectStandardOutput $out -RedirectStandardError $err
            $null = $p.Handle
            $exitCode = 'TIMEOUT'
            if (-not $p.WaitForExit(120000)) { try { $p.Kill() } catch {} } else { $exitCode = $p.ExitCode }

            $o = @(Get-Content $out -ErrorAction SilentlyContinue)
            $e = @(Get-Content $err -ErrorAction SilentlyContinue)
            Set-Content -Path $log -Value ($o + $e)
            Remove-Item $out, $err -ErrorAction SilentlyContinue

            if ($null -eq $envHeader) {
                $hdr = @()
                foreach ($line in $o) { if ($line -like 'Golden:*') { break }; $hdr += $line }
                if ($hdr.Count -gt 0) { $envHeader = $hdr }
            }

            $s.bad    += @($o | Where-Object { $_ -match 'match=no' }).Count
            $s.stale  += @($o | Where-Object { $_ -match 'stale_by_one=yes' }).Count
            $s.runs   += @($o | Where-Object { $_ -match '^run \d+:' }).Count
            if ($exitCode -eq 0) { $s.clean++ }
            elseif ($exitCode -eq 1) { $s.mismatched++ }
            elseif ($exitCode -eq 3 -or $exitCode -eq 'TIMEOUT') { $s.hung++ }
            elseif ($exitCode -eq 2) { $s.importfail++ }
            $badNow = @($o | Where-Object { $_ -match 'match=no' }).Count
            $color = if ($exitCode -eq 0) { 'Green' } elseif ($badNow -gt 0) { 'Red' } else { 'Yellow' }
            Write-Host ("[{0} {1} {2}/{3}] exit={4} bad_runs={5} (combo total bad={6})" -f $prog, $combo, $i, $Invocations, $exitCode, $badNow, $s.bad) -ForegroundColor $color
            foreach ($line in ($o | Where-Object { $_ -match 'match=no|^diag' })) { Write-Host "    $line" -ForegroundColor Red }
        }
        "$prog combo=$combo invocations=$Invocations clean=$($s.clean) mismatched=$($s.mismatched) hung=$($s.hung) importfail=$($s.importfail) bad_runs=$($s.bad) stale_by_one=$($s.stale) total_runs=$($s.runs)"
        $rows += "| $prog | $combo | $Invocations | $($s.runs) | $($s.bad) | $($s.stale) | $($s.mismatched) | $($s.hung) | $($s.importfail) |"
    }
}

$md = @()
$md += '# Results matrix'
$md += ''
if ($envHeader) {
    $md += 'Environment:'
    $md += ''
    $md += '```'
    $md += $envHeader
    $md += '```'
    $md += ''
}
$md += '| program | combo | invocations | runs | bad runs | stale_by_one | invocations with mismatch | hangs | import fail |'
$md += '|---|---|---|---|---|---|---|---|---|'
$md += $rows
Set-Content -Path (Join-Path $OutDir 'summary.md') -Value $md

'MATRIX DONE'
