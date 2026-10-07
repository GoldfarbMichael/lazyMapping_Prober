<#
.SYNOPSIS
  Start run_website_sweep.sh in YOUR DESKTOP SESSION, detached -- the Windows equivalent of
    DO_FINALIZE=0 DISPLAY=:0 setsid nohup ./run_website_sweep.sh > wf_X.log 2>&1 < /dev/null &

.DESCRIPTION
  Anything started over SSH (including VS Code Remote-SSH) runs in Windows session 0, which has
  no desktop and no GPU: Chrome would render invisibly, in software, and the traces would not be
  valid. This script hands the sweep to Task Scheduler, which starts it in the logged-in user's
  interactive session -- the same role DISPLAY=:0 plays on Ubuntu.

  The sweep is fully detached: closing this terminal or dropping the SSH connection does not
  stop it. Stop it with stop_website_sweep.ps1.

  Task settings that matter for the measurement (Task Scheduler's defaults would hurt it):
    * Priority 4 (normal). The default, 7, is BELOW NORMAL and is inherited by bash, the
      orchestrator and Chrome.
    * No execution time limit. The default kills a task after 72 h; full sweeps run longer.
    * Starts and keeps running on battery.
    * Limited (non-elevated) token, so Chrome keeps its sandbox even if this shell is elevated.

.PARAMETER Env
  Environment overrides for the sweep, as NAME=VALUE strings (comma-separated), e.g.
    -Env DO_FINALIZE=0,H5_LABEL=natalie

.PARAMETER Log
  Log file. Default: stable\wf_<yyyyMMdd_HHmmss>.log. A relative path is relative to stable\.

.PARAMETER Follow
  After starting, stream the log here (Ctrl+C stops WATCHING only, not the sweep).

.PARAMETER Force
  Start even if your session is not the one on the monitor, or the screen is locked.

.EXAMPLE
  .\run_website_sweep_desktop.ps1 -Env DO_FINALIZE=0 -Log wf_fullTranco_$(Get-Date -f yyyyMMdd_HHmmss).log
#>
[CmdletBinding(PositionalBinding = $false)]
param(
    [Alias('Env')][string[]]$SweepEnv = @(),
    [string]$Log = "",
    [switch]$Follow,
    [switch]$Force
)
$ErrorActionPreference = 'Stop'

$TaskPath = '\lazyMapping\'
$TaskName = 'WebsiteSweep'
$Stable   = $PSScriptRoot
$GitBash  = if ($env:GIT_BASH) { $env:GIT_BASH } else { 'C:\Program Files\Git\bin\bash.exe' }

function Fail([string]$msg) { Write-Host "[desktop] ERROR: $msg" -ForegroundColor Red; exit 1 }
function Warn([string]$msg) { Write-Host "[desktop] WARNING: $msg" -ForegroundColor Yellow }

# C:\a\b -> /c/a/b (Git Bash path), and bash single-quoting.
function To-Posix([string]$p) {
    $full = [IO.Path]::GetFullPath($p)
    '/' + $full.Substring(0, 1).ToLower() + $full.Substring(2).Replace('\', '/')
}
function Sh-Quote([string]$s) { "'" + $s.Replace("'", "'\''") + "'" }

if (-not (Test-Path $GitBash)) { Fail "Git Bash not found at '$GitBash' (set GIT_BASH)" }

# ---- 1. Which session will the sweep land in, and is it the one on the monitor? ----
Add-Type -Namespace LM -Name Wts -MemberDefinition `
    '[DllImport("kernel32.dll")] public static extern uint WTSGetActiveConsoleSessionId();'
# Match by SID, not by name: over SSH $env:USERDOMAIN reads WORKGROUP while the desktop's owner
# reads <COMPUTER>\<user>, so a name comparison would miss your own session.
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$me       = $identity.Name
$mySid    = $identity.User.Value
$mySession = $null
foreach ($p in Get-CimInstance Win32_Process -Filter "Name='explorer.exe'") {
    $o = Invoke-CimMethod -InputObject $p -MethodName GetOwnerSid
    if ($o.Sid -eq $mySid) { $mySession = [int]$p.SessionId }
}
if ($null -eq $mySession) {
    Fail "$me has no desktop session. Sign in as $me at the PC once (then you may lock it), and retry."
}
$console = [int][LM.Wts]::WTSGetActiveConsoleSessionId()
$locked  = [bool](Get-Process LogonUI -ErrorAction SilentlyContinue | Where-Object SessionId -eq $console)
$problems = @()
if ($mySession -ne $console) { $problems += "your session ($mySession) is not the one on the monitor ($console): another user is switched in" }
elseif ($locked)             { $problems += "the screen is locked: Windows may stop drawing your desktop, which can change how Chrome renders" }
foreach ($p in $problems) { Warn $p }
if ($problems -and -not $Force) { Fail "fix the above at the PC, or pass -Force to start anyway" }

# ---- 2. Never two sweeps at once (they would share :8080, :9222 and the Chrome profile) ----
$task = Get-ScheduledTask -TaskPath $TaskPath -TaskName $TaskName -ErrorAction SilentlyContinue
if ($task -and $task.State -eq 'Running') { Fail "a sweep task is already running (stop it with stop_website_sweep.ps1)" }
if (Get-Process WebsiteOrchestrator -ErrorAction SilentlyContinue) { Fail "a WebsiteOrchestrator is already running" }
if (Get-NetTCPConnection -LocalPort 8080 -State Listen -ErrorAction SilentlyContinue) {
    Fail "something is already listening on :8080 (a leftover server? stop_website_sweep.ps1 clears it)"
}

# ---- 3. Log path + the runner script the task executes ----
if (-not $Log) { $Log = "wf_$(Get-Date -Format yyyyMMdd_HHmmss).log" }
if (-not [IO.Path]::IsPathRooted($Log)) { $Log = Join-Path $Stable $Log }
$Log = [IO.Path]::GetFullPath($Log)
New-Item -ItemType Directory -Force (Split-Path $Log) | Out-Null

$lines = @('#!/usr/bin/env bash', '# Generated by run_website_sweep_desktop.ps1 -- safe to delete.')
foreach ($kv in $SweepEnv) {
    if ($kv -notmatch '^([A-Za-z_][A-Za-z0-9_]*)=(.*)$') { Fail "-Env entries must be NAME=VALUE, got '$kv'" }
    $lines += "export $($Matches[1])=$(Sh-Quote $Matches[2])"
}
$lines += "cd $(Sh-Quote (To-Posix $Stable)) || exit 1"
$lines += "exec ./run_website_sweep.sh > $(Sh-Quote (To-Posix $Log)) 2>&1 < /dev/null"
$runner = Join-Path $env:TEMP 'website_sweep_runner.sh'
# LF line endings and no BOM: bash reads this file.
[IO.File]::WriteAllText($runner, (($lines -join "`n") + "`n"), (New-Object Text.UTF8Encoding $false))

# ---- 4. Register + start the task in the interactive session ----
# conhost --headless gives bash a console without a window, so no terminal sits on the desktop
# (closing one would kill the sweep). Chrome is a GUI app and opens its own window as usual.
$action    = New-ScheduledTaskAction -Execute "$env:windir\System32\conhost.exe" `
                 -Argument "--headless `"$GitBash`" `"$runner`"" -WorkingDirectory $Stable
$principal = New-ScheduledTaskPrincipal -UserId $me -LogonType Interactive -RunLevel Limited
$settings  = New-ScheduledTaskSettingsSet -ExecutionTimeLimit ([TimeSpan]::Zero) -Priority 4 `
                 -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -MultipleInstances IgnoreNew
Register-ScheduledTask -TaskPath $TaskPath -TaskName $TaskName -Action $action `
    -Principal $principal -Settings $settings -Force | Out-Null
Start-ScheduledTask -TaskPath $TaskPath -TaskName $TaskName

# ---- 5. Confirm it actually started ----
$deadline = (Get-Date).AddSeconds(20)
while (-not (Test-Path $Log) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 500 }
if (-not (Test-Path $Log)) {
    $info = Get-ScheduledTaskInfo -TaskPath $TaskPath -TaskName $TaskName
    Fail "the sweep did not start within 20 s (task result: 0x$('{0:X}' -f $info.LastTaskResult))"
}

Write-Host "[desktop] sweep started in session $mySession (detached; survives closing this terminal)" -ForegroundColor Green
if ($SweepEnv) { Write-Host "[desktop] env    : $($SweepEnv -join ' ')" }
Write-Host "[desktop] log    : $Log"
Write-Host "[desktop] follow : Get-Content '$Log' -Wait -Tail 30"
Write-Host "[desktop] stop   : .\stop_website_sweep.ps1"

if ($Follow) { Get-Content $Log -Wait -Tail 30 }
