<#
.SYNOPSIS
  Stop a website sweep and everything it started: the scheduled task, the bash sweep script, the
  orchestrator, the Flask server on :8080, and the sweep's Chrome. Your own Chrome is untouched
  (only processes using the sweep's 'chrome-website' profile are matched).

.DESCRIPTION
  Works from any session (SSH included), since everything runs as you. Order matters: the sweep
  script goes first so it cannot launch the next batch while the rest is being torn down.

  Collected CSVs are kept. An interrupted sweep never finalizes; the last trace of the run may be
  partial, so delete JavaScript\data\website_<config> before reusing that tree for a NEW run.
#>
$ErrorActionPreference = 'Continue'

function Kill-Tree([int]$procId) { taskkill /F /T /PID $procId 2>&1 | Out-Null }

# 1. The scheduled task and the bash running the sweep (its whole tree: sleep, tee, ...).
$task = Get-ScheduledTask -TaskPath '\lazyMapping\' -TaskName 'WebsiteSweep' -ErrorAction SilentlyContinue
if ($task -and $task.State -eq 'Running') { Stop-ScheduledTask -InputObject $task }
Get-CimInstance Win32_Process -Filter "Name='bash.exe' OR Name='conhost.exe'" |
    Where-Object { $_.CommandLine -like '*run_website_sweep*' -or $_.CommandLine -like '*website_sweep_runner*' } |
    ForEach-Object { Kill-Tree $_.ProcessId }

# 2. The orchestrator. Chrome runs in its job object, so this alone takes the browser down.
Stop-Process -Name WebsiteOrchestrator -Force -ErrorAction SilentlyContinue

# 3. The Flask coordinator (whatever listens on :8080).
Get-NetTCPConnection -LocalPort 8080 -State Listen -ErrorAction SilentlyContinue |
    ForEach-Object { Kill-Tree $_.OwningProcess }

# 4. Backstop: any Chrome still using the sweep's profile.
Start-Sleep -Seconds 1
Get-CimInstance Win32_Process -Filter "Name='chrome.exe'" |
    Where-Object { $_.CommandLine -like '*chrome-website*' } |
    ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }

# 5. Report.
Start-Sleep -Seconds 1
$left = @{
    'sweep script'  = @(Get-CimInstance Win32_Process -Filter "Name='bash.exe'" | Where-Object CommandLine -like '*run_website_sweep*').Count
    'orchestrator'  = @(Get-Process WebsiteOrchestrator -ErrorAction SilentlyContinue).Count
    'sweep chrome'  = @(Get-CimInstance Win32_Process -Filter "Name='chrome.exe'" | Where-Object CommandLine -like '*chrome-website*').Count
    'ports 8080/9222' = @(Get-NetTCPConnection -LocalPort 8080, 9222 -State Listen -ErrorAction SilentlyContinue).Count
}
$clean = $true
foreach ($k in $left.Keys) {
    "{0,-16}: {1}" -f $k, $left[$k]
    if ($left[$k] -ne 0) { $clean = $false }
}
if ($clean) { Write-Host "[stop] everything stopped." -ForegroundColor Green }
else        { Write-Host "[stop] something is still running -- run this script again." -ForegroundColor Yellow; exit 1 }
