# ---------------------------------------------------------------------------
# claude-session.ps1 -- let two Claude tabs share this engine without
# standing on each other.
#
# WHY THIS EXISTS. On 2026-09-14 two sessions worked C:/voxelbit/v1 at the same
# time and every one of these happened inside an hour:
#
#   * One tab's build died with "LNK1104: cannot open file Falcor.dll" because
#     the other tab's v1.exe was holding it.
#   * Two builds overlapped and fought over the same 342 MB precompiled header:
#     "C1083: cannot open compiler intermediate file ... Invalid argument".
#   * A tab killed two running v1.exe processes to clear its own build, and
#     they belonged to the OTHER tab's render.
#
# None of those say what is really wrong, and all three cost a full round trip.
# The whole problem is that this tree has exactly one build directory, one
# v1.exe and one Falcor.dll, and nothing anywhere says who is using them.
#
# WHAT A SESSION ID IS HERE. Every Claude tab has its own scratchpad under
#   C:\Users\<user>\AppData\Local\Temp\claude\<project>\<session-uuid>\scratchpad
# and knows that uuid. That uuid is the identity used throughout: pass it as
# -Session. It is also recoverable from a running render's own command line,
# because renders write their .png into that scratchpad -- which is how
# `status` can tell you whose process something is without any cooperation from
# the tab that started it.
#
# USAGE
#   claude-session.ps1 status  [-Session <uuid>]
#   claude-session.ps1 build   -Session <uuid> [-Wait]
#   claude-session.ps1 run     -Session <uuid> -Args '<v1 args>' [-TimeoutSec N]
#   claude-session.ps1 release -Session <uuid> [-Force]
#
# Exit codes: 0 ok, 1 failed, 2 the lock is held by somebody else.
# ---------------------------------------------------------------------------
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [ValidateSet('status', 'build', 'run', 'release')]
    [string]$Action,

    # This tab's scratchpad uuid. Required for anything that takes the lock.
    [string]$Session = '',

    # Arguments for `run`, as one string, e.g. '--background --shot out.png --shot-frame 2'
    [string]$Args = '',

    # `run`: where to put the engine's stdout. WITHOUT this a caller has to
    # reach for Start-Process itself to capture output -- which is exactly how
    # the focus rule got broken, so the capture lives here instead.
    [string]$Out = '',

    # `build`: wait for the lock instead of giving up.
    [switch]$Wait,

    # `release`: break a lock this session does not own. Say why out loud.
    [switch]$Force,

    [int]$TimeoutSec = 900
)

$ErrorActionPreference = 'Stop'
$Root = 'C:\voxelbit\v1'
$Lock = Join-Path $Root 'build\.claude-build.lock'

# A render's .png lands in the owning tab's scratchpad, so the uuid is in the
# command line. 36 hex-and-dash characters after the project directory.
$SessionRe = 'claude\\[^\\]+\\([0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12})'

function Short($id) { if ($id) { $id.Substring(0, 8) } else { '????????' } }

# --- who is running the engine right now ----------------------------------
function Get-V2Processes {
    $out = @()
    foreach ($p in (Get-CimInstance Win32_Process -Filter "Name='v1.exe'" -ErrorAction SilentlyContinue)) {
        $owner = ''
        if ($p.CommandLine -and $p.CommandLine -match $SessionRe) { $owner = $Matches[1] }
        $out += [pscustomobject]@{
            Pid     = $p.ProcessId
            Owner   = $owner
            Started = $p.CreationDate
            Command = $p.CommandLine
        }
    }
    return $out
}

# --- the lock --------------------------------------------------------------
#
# A LOCK AND NOT A MUTEX, because the thing being protected is a DIRECTORY that
# outlives every process that touches it. A named mutex would vanish the moment
# a tab's shell exited and leave the build dir half written with nothing to say
# so. A file carries the PID, so a lock whose owner is gone can be recognised as
# stale rather than waited on forever.
function Read-Lock {
    if (-not (Test-Path $Lock)) { return $null }
    try { return (Get-Content $Lock -Raw | ConvertFrom-Json) } catch { return $null }
}

function Lock-IsStale($info) {
    if (-not $info) { return $true }
    # The holder's shell is gone -> the build died with it.
    return -not (Get-Process -Id $info.pid -ErrorAction SilentlyContinue)
}

function Take-Lock($session) {
    $info = Read-Lock
    if ($info -and -not (Lock-IsStale $info)) {
        if ($info.session -eq $session) { return $true }   # already ours, reentrant
        return $false
    }
    if ($info) {
        Write-Host "  (breaking a stale lock from $(Short $info.session), pid $($info.pid) is gone)"
    }
    New-Item -ItemType Directory -Force -Path (Split-Path $Lock) | Out-Null
    @{
        session = $session
        pid     = $PID
        started = (Get-Date).ToString('o')
        host    = $env:COMPUTERNAME
    } | ConvertTo-Json | Out-File -FilePath $Lock -Encoding utf8
    return $true
}

function Free-Lock($session, $force) {
    $info = Read-Lock
    if (-not $info) { return }
    if ($info.session -ne $session -and -not $force) {
        Write-Host "not yours: the lock belongs to $(Short $info.session). -Force to break it."
        return
    }
    Remove-Item $Lock -Force -ErrorAction SilentlyContinue
}

# ===========================================================================
switch ($Action) {

    'status' {
        $info = Read-Lock
        if ($info) {
            $stale = if (Lock-IsStale $info) { '  [STALE -- owner gone]' } else { '' }
            $mine = if ($Session -and $info.session -eq $Session) { '  (yours)' } else { '' }
            Write-Host "build lock : $(Short $info.session)  pid $($info.pid)  since $($info.started)$stale$mine"
        }
        else {
            Write-Host "build lock : free"
        }

        $ps = Get-V2Processes
        if (-not $ps) {
            Write-Host "v1.exe     : none running"
        }
        else {
            foreach ($p in $ps) {
                $mine = if ($Session -and $p.Owner -eq $Session) { 'YOURS ' }
                        elseif ($p.Owner) { 'OTHER ' }
                        else { '?????? ' }
                Write-Host "v1.exe     : $mine pid $($p.Pid)  owner $(Short $p.Owner)  started $($p.Started)"
            }
            Write-Host ""
            Write-Host "A build cannot link while ANY of these is alive. Only ever stop your own:"
            Write-Host "  claude-session.ps1 run ... reaps what it starts."
        }
    }

    'build' {
        if (-not $Session) { Write-Host "build needs -Session <uuid>"; exit 1 }

        # 1. THE LOCK FIRST, so two tabs cannot be in ninja at once. This is the
        #    PCH corruption above, and it does not announce itself as contention.
        $deadline = (Get-Date).AddSeconds($TimeoutSec)
        while (-not (Take-Lock $Session)) {
            $info = Read-Lock
            if (-not $Wait) {
                Write-Host "build lock held by $(Short $info.session) (pid $($info.pid)) since $($info.started)."
                Write-Host "Re-run with -Wait, or ask that tab to finish. Nothing was built."
                exit 2
            }
            if ((Get-Date) -gt $deadline) { Write-Host "gave up waiting for the build lock."; exit 2 }
            Start-Sleep -Seconds 5
        }

        try {
            # 2. AND THEN THE EXE, which is a different owner and a different
            #    message. build.bat checks this too, but it says "ESC twice in
            #    the window" -- advice that means nothing for a headless
            #    --background render, and it never says whose window.
            $foreign = @(Get-V2Processes | Where-Object { $_.Owner -ne $Session })
            if ($foreign) {
                Write-Host "cannot build -- another session's engine is running and holds the exe:"
                foreach ($p in $foreign) {
                    Write-Host "    pid $($p.Pid)  owner $(Short $p.Owner)  $($p.Command)"
                }
                Write-Host ""
                Write-Host "DO NOT kill it. It is another tab's render and killing it loses their work."
                Write-Host "Wait, or ask that tab to reap it."
                exit 2
            }
            # Ours we may reap, because we know what it is.
            foreach ($p in (Get-V2Processes | Where-Object { $_.Owner -eq $Session })) {
                Write-Host "  (reaping your own v1.exe, pid $($p.Pid))"
                Stop-Process -Id $p.Pid -Force -ErrorAction SilentlyContinue
            }

            # 3. POWERSHELL, NOT GIT BASH, and build.bat's own guard is why.
            #    It tests `tasklist | find /I "v1.exe"`, and Git Bash puts its
            #    OWN find(1) ahead of C:\Windows\System32\find.exe -- which
            #    fails with "find: '/I': No such file or directory" and makes
            #    the guard misfire. The link then dies at the very end of the
            #    build with the bare LNK1104 the guard exists to prevent.
            & cmd.exe /c "cd /d $Root && $Root\build.bat"
            exit $LASTEXITCODE
        }
        finally {
            Free-Lock $Session $false
        }
    }

    'run' {
        if (-not $Session) { Write-Host "run needs -Session <uuid>"; exit 1 }

        # NO LOCK. Two tabs may render at once -- they only collide over the
        # BUILD directory, not over reading the exe. What matters is that this
        # one is reaped, which is the whole reason `run` exists rather than
        # calling v1.exe directly.
        #
        # --background is forced on: an engine that takes the screen and the
        # mouse in the middle of somebody else's session is the one unrecoverable
        # rudeness here, and it has been reported three times.
        $a = $Args
        if ($a -notmatch '--background') { $a = "--background $a" }

        Write-Host "running: v1.exe $a"
        # -- NoNewWindow, NOT -WindowStyle Minimized ------------------------
        #
        # "stop opening up the terminal on my screen and stop taking my mouse
        #  away. make this a rule. I thought we already made this a rule but
        #  its not working"                               -- user 2026-09-17
        #
        # THE ENGINE WAS NOT AT FAULT THIS TIME. --background already opens the
        # render window straight to the taskbar and REFUSES THE CURSOR outright
        # (App::setLooking returns early on it), so what kept appearing over the
        # user's work was the CONSOLE Start-Process makes for the child.
        # -WindowStyle Minimized does not prevent that: the console is created
        # and THEN minimised, which is a flash across the screen and a focus
        # change however brief.
        #
        # -NoNewWindow creates no console at all -- the child inherits this
        # session's, which is not on screen. It cannot flash because it does not
        # exist. Redirection is required with it or two processes interleave on
        # one console, which is why -Out is a parameter now: a caller that wants
        # the output must never have to reach for Start-Process itself again.
        $sp = @{ FilePath = "$Root\build\bin\Release\v1.exe"
                 ArgumentList = $a; WorkingDirectory = $Root
                 NoNewWindow = $true; PassThru = $true }
        if ($Out) {
            $sp['RedirectStandardOutput'] = $Out
            $sp['RedirectStandardError'] = "$Out.err"
        }
        $p = Start-Process @sp
        if (-not $p.WaitForExit($TimeoutSec * 1000)) {
            Write-Host "timed out after ${TimeoutSec}s -- stopping pid $($p.Id)"
            Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
        }

        # THE REAP, AND IT IS THE POINT OF THIS WHOLE SCRIPT. A --shot run
        # returns to the shell while the engine is still alive; that survivor is
        # what holds Falcor.dll and breaks the NEXT build, in this tab or
        # another one. Sweep anything of ours that outlived the call.
        Start-Sleep -Milliseconds 400
        foreach ($q in (Get-V2Processes | Where-Object { $_.Owner -eq $Session })) {
            Write-Host "  (reaping leftover v1.exe, pid $($q.Pid))"
            Stop-Process -Id $q.Pid -Force -ErrorAction SilentlyContinue
        }
        exit 0
    }

    'release' { Free-Lock $Session $Force.IsPresent }
}
