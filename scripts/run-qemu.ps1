# scripts/run-qemu.ps1 - Windows QEMU GUI acceptance for Nova OS
#
# Usage (PowerShell):
#   powershell -ExecutionPolicy Bypass -File scripts\run-qemu.ps1              # UEFI + GTK display
#   powershell -ExecutionPolicy Bypass -File scripts\run-qemu.ps1 -Display sdl # SDL display
#   powershell -ExecutionPolicy Bypass -File scripts\run-qemu.ps1 -Mode bios  # BIOS mode (boot from installed disk)
#
# Notes:
#   - Locates QEMU (default D:\qemu, else PATH qemu-system-x86_64)
#   - UEFI mode needs OVMF firmware build\ovmf.fd; auto-copies from WSL if missing
#   - q35 + PIIX4 IDE controller (the kernel ATA PIO driver uses legacy 0x1F0 ports;
#     plain if=ide would attach to the ICH9 AHCI and the kernel would not detect it)
#   - MTTCG (-accel tcg,thread=multi) + cache=unsafe + monitor keepalive:
#     without these, the long PIO install write ([4/4] Writing system files)
#     starves the QEMU main loop and hangs. Same recipe as scripts/test-install.sh.
#   - Two fresh blank disks are attached:
#       build\hd512.img (512 MB) -> ide0.0 (dev0)
#       build\hd256.img (256 MB) -> ide0.1 (dev2)
#     Use the kernel `install` command in the shell to install Nova onto either one.
#     After installing, close QEMU and re-run with `-Mode bios` to boot from the
#     installed disk (adds -boot order=c so BIOS tries the hard disk first).
#   - Display backend: gtk (default) or sdl; if one fails, switch to the other
#   - Close the window to exit; release mouse grab with Ctrl+Alt+G

param(
    [ValidateSet('uefi', 'bios')]
    [string]$Mode = 'uefi',
    [ValidateSet('gtk', 'sdl')]
    [string]$Display = 'gtk',
    [ValidateSet('512M', '768M', '1G')]
    [string]$Mem = '512M',
    [ValidateSet('512', '256')]
    [string]$PrimaryDisk = '512',
    [switch]$NoCdrom
)

$ErrorActionPreference = 'Stop'

# Locate QEMU
$qemu = 'D:\qemu\qemu-system-x86_64.exe'
if (-not (Test-Path $qemu)) {
    $cmd = Get-Command qemu-system-x86_64 -ErrorAction SilentlyContinue
    if ($cmd) { $qemu = $cmd.Source } else { throw 'QEMU not found' }
}

$root = Split-Path -Parent $PSScriptRoot
$iso  = Join-Path $root 'build\nova.iso'
if (-not $NoCdrom -and -not (Test-Path $iso)) {
    throw "Missing build\nova.iso - build it first in WSL: wsl -e bash -lc 'cd /mnt/d/Users/Coffee/Desktop/nova && make iso'"
}

# Two fresh blank disks for the installer test. If missing, create them blank.
$disk512 = Join-Path $root 'build\hd512.img'
$disk256 = Join-Path $root 'build\hd256.img'
foreach ($d in @(@($disk512, '512M'), @($disk256, '256M'))) {
    if (-not (Test-Path $d[0])) {
        $wslPath = "/mnt/d/Users/Coffee/Desktop/nova"
        $oldEAP = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try {
            & wsl.exe -e bash -lc "cd $wslPath && truncate -s $($d[1]) $($d[0] -replace '\\','/')" 2>$null | Out-Null
        } catch { } finally {
            $ErrorActionPreference = $oldEAP
        }
    }
}

# WSL file existence check (tolerant of WSL stderr proxy warnings)
function Test-WslFile([string]$path) {
    $oldEAP = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & wsl.exe -e bash -lc "test -f $path" 2>$null | Out-Null
        return ($LASTEXITCODE -eq 0)
    } catch {
        return $false
    } finally {
        $ErrorActionPreference = $oldEAP
    }
}

# Two disks (512M + 256M) on a PIIX4 IDE controller + installer ISO.
# -PrimaryDisk 512|256 decides which disk is on ide0.0 (BIOS tries first).
# Default 512; use -PrimaryDisk 256 to boot the 256M disk.
$img0 = Join-Path $root 'build\hd512.img'
$img1 = Join-Path $root 'build\hd256.img'
if ($PrimaryDisk -eq '256') {
    $img0 = Join-Path $root 'build\hd256.img'
    $img1 = Join-Path $root 'build\hd512.img'
}
$qemuArgs = @('-M', 'q35',
              '-m', $Mem,
              # MTTCG: vCPU threads run outside the main loop. Single-threaded
              # TCG starves the main loop during the long PIO install write
              # (57MB kernel.elf); ATA write completions never dispatch and
              # [4/4] Writing system files hangs (WSL and Windows alike).
              '-accel', 'tcg,thread=multi',
              '-device', 'piix4-ide,id=ide0',
              # cache=unsafe: skip flush waits. Combined with the polling ATA
              # driver this avoids late write-completion signals (the combo
              # validated in scripts/test-install.sh)
              '-netdev', 'user,id=net0',
              '-device', 'e1000,netdev=net0',
              '-smp', 'cores=2,threads=1,sockets=1',
              '-cpu', 'qemu64',
              '-display', $Display,
              '-name', 'Nova OS - click inside window to type')

# Hard disks: in UEFI mode give bootindex so the installer CD (bootindex=0)
# boots first; OVMF would otherwise prefer an installed disk's bootable ESP
# and you could never install a new system. BIOS mode stays as-is
# (-boot order=c controls).
if ($Mode -eq 'uefi') {
    $qemuArgs += @('-drive', "file=$img0,format=raw,if=none,id=disk0,cache=unsafe",
                   '-device', 'ide-hd,drive=disk0,bus=ide0.0,bootindex=1',
                   '-drive', "file=$img1,format=raw,if=none,id=disk1,cache=unsafe",
                   '-device', 'ide-hd,drive=disk1,bus=ide0.1,bootindex=2')
} else {
    $qemuArgs += @('-drive', "file=$img0,format=raw,if=none,id=disk0,cache=unsafe",
                   '-device', 'ide-hd,drive=disk0,bus=ide0.0',
                   '-drive', "file=$img1,format=raw,if=none,id=disk1,cache=unsafe",
                   '-device', 'ide-hd,drive=disk1,bus=ide0.1')
}

# Installer ISO: attached by default (install/acceptance); -NoCdrom removes it
# (installed-disk direct-boot verification). In UEFI mode use ide-cd with
# bootindex=0 to force the CD first (-boot order=d is ignored by OVMF).
if (-not $NoCdrom) {
    if ($Mode -eq 'uefi') {
        $qemuArgs += @('-drive', "file=$iso,if=none,id=cd0,readonly=on",
                       '-device', 'ide-cd,drive=cd0,bootindex=0')
    } else {
        $qemuArgs += @('-cdrom', $iso)
    }
}

# Serial log: kernel UART output (install progress/timestamps) for diagnostics
$serLog = Join-Path $root 'build\run-install.log'
Remove-Item $serLog -ErrorAction SilentlyContinue
$qemuArgs += @('-serial', "file:$serLog")

# TCP monitor + keepalive: send 'info registers' every 1.5s and DRAIN the
# responses. Every passing configuration (test-install.sh pipe, headless
# tests) drains the monitor output; an undrained client lets QEMU's monitor
# output accumulate and can block the main loop, freezing the vCPUs
# mid-install (observed: install stalls at ~96% of kernel.elf).
$kaJob = $null
$monPort = 0
try {
    $l = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 0)
    $l.Start()
    $monPort = ([System.Net.IPEndPoint]$l.LocalEndpoint).Port
    $l.Stop()
    $qemuArgs += @('-monitor', "tcp:127.0.0.1:$monPort,server,nowait")
    $kaJob = Start-Job -ArgumentList $monPort -ScriptBlock {
        param($port)
        $client = New-Object System.Net.Sockets.TcpClient
        for ($i = 0; $i -lt 60 -and -not $client.Connected; $i++) {
            try { $client.Connect('127.0.0.1', $port) } catch { }
            if (-not $client.Connected) { Start-Sleep -Milliseconds 500 }
        }
        if ($client.Connected) {
            $stream = $client.GetStream()
            $stream.ReadTimeout = 500
            $cmd = [Text.Encoding]::ASCII.GetBytes("info registers`n")
            $buf = New-Object byte[] 4096
            while ($true) {
                try {
                    $stream.Write($cmd, 0, $cmd.Length)
                    $stream.Flush()
                    # drain whatever QEMU answers (timeout = no more data)
                    try {
                        while ($true) {
                            $n = $stream.Read($buf, 0, $buf.Length)
                            if ($n -le 0) { break }
                        }
                    } catch { }
                } catch { break }
                Start-Sleep -Milliseconds 1500
            }
        }
    }
} catch {
    Write-Host "[Nova] monitor keepalive disabled: $_" -ForegroundColor Yellow
    $kaJob = $null
}

if ($Mode -eq 'bios') {
    # Boot from the installed hard disk (MBR + Limine BIOS stages)
    $qemuArgs += @('-boot', 'order=c')
}

if ($Mode -eq 'uefi' -and -not $NoCdrom) {
    # Legacy -boot order=d: harmless; OVMF ignores it (bootindex above wins)
    $qemuArgs += @('-boot', 'order=d')
}

if ($Mode -eq 'uefi') {
    $ovmf = Join-Path $root 'build\ovmf.fd'
    if (-not (Test-Path $ovmf)) {
        Write-Host "[Nova] OVMF missing, trying to copy from WSL..." -ForegroundColor Yellow
        $wsl = Get-Command wsl.exe -ErrorAction SilentlyContinue
        if ($wsl) {
            # Windows path -> WSL path (drive letter must be lowercase: /mnt/d/...)
            $drive = $ovmf.Substring(0, 1).ToLower()
            $rest = $ovmf.Substring(3) -replace '\\', '/'
            $ovmfWsl = "/mnt/$drive/$rest"
            foreach ($src in @('/usr/share/ovmf/OVMF.fd',
                               '/usr/share/ovmf/OVMF_CODE.fd',
                               '/usr/share/OVMF/OVMF_CODE.fd')) {
                if (Test-WslFile $src) {
                    $oldEAP = $ErrorActionPreference
                    $ErrorActionPreference = 'Continue'
                    try {
                        & wsl.exe -e bash -lc "cp $src $ovmfWsl" 2>$null | Out-Null
                    } catch { } finally {
                        $ErrorActionPreference = $oldEAP
                    }
                    if (Test-Path $ovmf) { break }
                }
            }
        }
        if (-not (Test-Path $ovmf)) {
            throw "Missing OVMF firmware build\ovmf.fd. Install ovmf in WSL (wsl -u root -e apt-get install -y ovmf) and re-run."
        }
        Write-Host "[Nova] OVMF copied to build\ovmf.fd" -ForegroundColor Green
    }
    # Use a fresh copy of the firmware per run: QEMU writes NVRAM state into
    # the -bios file; an abruptly-killed QEMU can corrupt it (OVMF then hangs
    # on the "Select Language" setup screen). The pristine ovmf.fd stays clean.
    $ovmfRun = Join-Path $root 'build\ovmf-run.fd'
    Copy-Item $ovmf $ovmfRun -Force
    $qemuArgs += @('-bios', $ovmfRun)
}

Write-Host "[Nova] Launching QEMU ($Mode mode, $Display display)..." -ForegroundColor Green
Write-Host "[Nova] CPU: 2 cores, Memory: $Mem (Software Emulation)" -ForegroundColor Cyan
Write-Host "[Nova] Primary disk: $img0 (ide0.0/dev0) - use -PrimaryDisk 256 to boot the 256M disk" -ForegroundColor Cyan
Write-Host "[Nova] Secondary: $img1 (ide0.1/dev2)" -ForegroundColor Cyan
if ($Mode -eq 'bios') {
    Write-Host "[Nova] BIOS boot order: hard disk first (boot from installed system)" -ForegroundColor Cyan
}
Write-Host "[Nova] Net: e1000 NIC via slirp (guest 10.0.2.15, gateway 10.0.2.2)" -ForegroundColor Cyan

# Phase 19: HTTP repo server for the network package repository.
# The kernel downloads index.nvp and webdemo.nvp from 10.0.2.2:8080
# (slirp gateway -> this host) during boot, then serves them via the CLI.
$repoDir = Join-Path $root 'build\repo'
$httpProc = $null
if (Test-Path (Join-Path $repoDir 'index.nvp')) {
    $py = (Get-Command python -ErrorAction SilentlyContinue).Source
    if (-not $py) { $py = (Get-Command py -ErrorAction SilentlyContinue).Source }
    if ($py) {
        try {
            $httpProc = Start-Process -FilePath $py `
                -ArgumentList @('-m', 'http.server', '8080', '--bind', '127.0.0.1',
                                '--directory', $repoDir) `
                -PassThru -WindowStyle Hidden
            Start-Sleep -Milliseconds 800
            Write-Host "[Nova] HTTP repo server started (pid $($httpProc.Id), dir build\repo)" -ForegroundColor Green
        } catch {
            Write-Host "[Nova] HTTP repo server start failed (ignored): $_" -ForegroundColor Yellow
            $httpProc = $null
        }
        # Verify the server is actually serving before launching QEMU
        if ($httpProc) {
            $ready = $false
            for ($i = 0; $i -lt 20 -and -not $ready; $i++) {
                Start-Sleep -Milliseconds 300
                try {
                    $r = Invoke-WebRequest -Uri 'http://127.0.0.1:8080/index.nvp' -UseBasicParsing -TimeoutSec 1 -ErrorAction Stop
                    if ($r.StatusCode -eq 200) { $ready = $true }
                } catch { }
            }
            if ($ready) {
                Write-Host "[Nova] HTTP repo server verified (index.nvp served)" -ForegroundColor Green
            } else {
                Write-Host "[Nova] WARNING: repo server not responding - kernel network test may fail" -ForegroundColor Yellow
            }
        }
    } else {
        Write-Host "[Nova] python not found - HTTP repo server skipped" -ForegroundColor Yellow
    }
} else {
    Write-Host "[Nova] build\repo not found - rebuild disk via WSL prepare-ext2-disk.sh" -ForegroundColor Yellow
}

Write-Host "[Nova] Acceptance: 1) click INSIDE the window first, then type" -ForegroundColor Green
Write-Host "[Nova]            2) run `install` to see the disk chooser, install onto 512M/256M" -ForegroundColor Green
Write-Host "[Nova]            3) if [4/4] seems stuck: WAIT 10-20s (kernel auto-retries ATA" -ForegroundColor Yellow
Write-Host "[Nova]               writes); if it prints a failure line, just run `install` again;" -ForegroundColor Yellow
Write-Host "[Nova]               only if frozen >2 min, close and re-run (TCG write hiccup)" -ForegroundColor Yellow
Write-Host "[Nova]            4) close QEMU, re-run with -Mode bios to boot the installed disk" -ForegroundColor Green
Write-Host "[Nova]            5) reboot via QEMU menu: Machine -> Send Ctrl+Alt+Del" -ForegroundColor Green
Write-Host "[Nova]               (Windows itself intercepts the real Ctrl+Alt+Del keystroke)" -ForegroundColor Yellow
& $qemu @qemuArgs

if ($kaJob) {
    Stop-Job $kaJob -ErrorAction SilentlyContinue
    Remove-Job $kaJob -ErrorAction SilentlyContinue
    Write-Host "[Nova] monitor keepalive stopped" -ForegroundColor Green
}
Write-Host "[Nova] UART log: $serLog (install progress diagnostics)" -ForegroundColor Cyan

if ($httpProc -and -not $httpProc.HasExited) {
    Stop-Process -Id $httpProc.Id -Force -ErrorAction SilentlyContinue
    Write-Host "[Nova] HTTP repo server stopped" -ForegroundColor Green
}
