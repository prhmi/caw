# CAW installer - step 1: collect everything the setup will contain.
#   stage\   = files that get installed (caw.exe, ffmpeg, SuperCollider runtime, libpd, Csound, examples)
#   redist\  = the Microsoft VC++ runtime setup that gets embedded
# Safe to run again; stage\ is rebuilt each time, downloads in redist\ are kept.
param(
    [string]$CsoundDir = $env:CSOUND_DIR,  # optional: folder of the Csound you want to ship (the one with bin\csound.exe)
    [switch]$IncludeFfmpeg,                # also ship ffmpeg.exe (about 100 MB) - normally NOT needed, CAW exports MP3/FLAC natively
    [switch]$FullCsound,                   # copy the WHOLE Csound folder (docs, examples, tools...) instead of the slim engine copy
    [string]$DropPlugins = ''              # optional regex of plugin DLL names to leave out, e.g. 'py|lua|java|jack'
)
$ErrorActionPreference = 'Stop'
$ProgressPreference    = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$here   = $PSScriptRoot
$root   = Split-Path $here -Parent
$stage  = Join-Path $here 'stage'
$redist = Join-Path $here 'redist'
$deps   = Join-Path $env:USERPROFILE '.caw_deps'
$warn   = New-Object System.Collections.Generic.List[string]

function Step($t) { Write-Host ""; Write-Host "=== $t ===" -ForegroundColor Cyan }
function Ok($t)   { Write-Host "  [ok]   $t" -ForegroundColor Green }
function Warn($t) { Write-Host "  [warn] $t" -ForegroundColor Yellow; $warn.Add($t) }
function SizeMB($p) { '{0:N1} MB' -f ((Get-ChildItem $p -Recurse -File | Measure-Object Length -Sum).Sum / 1MB) }

function Get-File($url, $dest) {
    Write-Host "  downloading $url"
    Invoke-WebRequest -Uri $url -OutFile $dest -UseBasicParsing -Headers @{ 'User-Agent' = 'CAW-installer' }
    $b = [IO.File]::ReadAllBytes($dest)
    if ($b.Length -lt 1MB -or $b[0] -ne 0x4D -or $b[1] -ne 0x5A) {   # must be a real .exe ("MZ")
        Remove-Item $dest -Force
        throw "Download is not a valid installer: $url"
    }
}

# ---------------------------------------------------------------- stage
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force $stage, $redist | Out-Null

# ---------------------------------------------------------------- 1. caw.exe
Step 'CAW'
$cawExe = Join-Path $root 'build\Release\caw.exe'
if (-not (Test-Path $cawExe)) { throw "caw.exe not found at $cawExe`nRun build.bat in the project folder first (it ends with 'Done.')." }
Copy-Item $cawExe $stage
Ok "caw.exe  ($(SizeMB $cawExe))"
# Staging an outdated exe ships old bugs (for example an 'abs' error that was
# already fixed in src) to every PC this installer runs on.
$srcNewest = Get-ChildItem (Join-Path $root 'src') -Recurse -File -Include '*.cpp','*.h','*.hpp' -ErrorAction SilentlyContinue |
             Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($srcNewest -and (Get-Item $cawExe).LastWriteTime -lt $srcNewest.LastWriteTime) {
    Warn "caw.exe is OLDER than the newest source file ($($srcNewest.Name), changed $($srcNewest.LastWriteTime)). Run build.bat again first - this setup would install the old code, and old errors come back on the installed PC."
}

$ff = Join-Path $root 'build\Release\ffmpeg.exe'
if ($IncludeFfmpeg) {
    if (Test-Path $ff) {
        Copy-Item $ff $stage; Ok "ffmpeg.exe ($(SizeMB $ff)) - optional component in the setup"
        $ffl = Join-Path $root 'build\Release\ffmpeg-licenses'
        if (Test-Path $ffl) { Copy-Item $ffl (Join-Path $stage 'ffmpeg-licenses') -Recurse }
    } else { Warn 'ffmpeg.exe not found beside caw.exe, so it cannot be included.' }
} else {
    Ok 'ffmpeg.exe NOT included (MP3/FLAC use the built-in encoder). Add -IncludeFfmpeg to ship it as an optional component.'
}

# ---------------------------------------------------------------- 2. SuperCollider runtime
Step 'SuperCollider runtime'
$scDir = Join-Path $deps 'supercollider\runtime-x64\SuperCollider'
if ((Test-Path (Join-Path $scDir 'sclang.exe')) -and (Test-Path (Join-Path $scDir 'libscsynth.dll'))) {
    Copy-Item $scDir (Join-Path $stage 'SuperCollider') -Recurse
    $scLic = Join-Path $deps 'supercollider\source\LICENSE'
    if (Test-Path $scLic) { Copy-Item $scLic (Join-Path $stage 'SuperCollider\SuperCollider-LICENSE.txt') }
    Ok "SuperCollider runtime  ($(SizeMB (Join-Path $stage 'SuperCollider')))"
} else {
    Warn "SuperCollider runtime not found in $scDir. Run tools\windows\BUILD-LIBSCSYNTH.bat once, then run this again. (The setup will still build, but the SuperCollider checkbox would install nothing.)"
}

# ---------------------------------------------------------------- 3. libpd
Step 'Pure Data (libpd)'
$pd = $null
$cfg = Join-Path $env:USERPROFILE '.caw_libpd_path'
if (Test-Path $cfg) { $p = (Get-Content $cfg -Raw).Trim(); if ($p -and (Test-Path $p)) { $pd = $p } }
if (-not $pd) {
    $c = Get-ChildItem (Join-Path $env:USERPROFILE 'CAW-libpd') -Directory -Filter 'build-*' -ErrorAction SilentlyContinue |
         Where-Object { Test-Path (Join-Path $_.FullName 'libpd.dll') } | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($c) { $pd = Join-Path $c.FullName 'libpd.dll' }
}
if ($pd) {
    $pdOut = Join-Path $stage 'PureData'; New-Item -ItemType Directory $pdOut | Out-Null
    Get-ChildItem (Split-Path $pd -Parent) -Filter '*.dll' | Copy-Item -Destination $pdOut
    Ok "libpd from $pd"
    Get-ChildItem $pdOut | ForEach-Object { Write-Host "         $($_.Name)" }
    if (-not (Test-Path (Join-Path $pdOut 'libwinpthread-1.dll'))) { Warn 'libwinpthread-1.dll is not beside libpd.dll - libpd will fail to load on other PCs.' }
} else {
    Warn 'libpd.dll not found. Run tools\windows\BUILD-LIBPD.bat once, then run this again.'
}

# ---------------------------------------------------------------- 4. examples + licence text
Step 'Examples and licence text'
$ex = Join-Path $root 'examples'
if (Test-Path $ex) { Copy-Item $ex (Join-Path $stage 'examples') -Recurse; Ok 'examples' }

@"
CAW - Code Audio Workstation  $((Get-Date).ToString('yyyy'))

This setup installs CAW together with third-party software. Each part keeps its own licence:

  FFmpeg (ffmpeg.exe)      - see the ffmpeg-licenses folder in the install folder (GPL build: source is available from ffmpeg.org / gyan.dev)
  SuperCollider            - GNU GPL v3, see SuperCollider\SuperCollider-LICENSE.txt, source: github.com/supercollider/supercollider
  libpd / Pure Data        - BSD-style licence, source: github.com/libpd/libpd
  Csound (optional)        - GNU LGPL v2.1, source and licence: csound.com / github.com/csound/csound
  PortAudio, Dear ImGui    - MIT licence (linked into caw.exe)
  VST3 SDK                 - Steinberg VST3 licence / GPLv3 (linked into caw.exe)
  ASIO                     - ASIO is a trademark and software of Steinberg Media Technologies GmbH
  Microsoft Visual C++ Redistributable - installed from Microsoft's own setup if it is missing

By continuing you confirm you have read this list.
"@ | Set-Content -Path (Join-Path $stage 'LICENSES-READ-ME.txt') -Encoding ASCII
Ok 'LICENSES-READ-ME.txt'

# ---------------------------------------------------------------- 5. Microsoft VC++ runtime
Step 'Microsoft Visual C++ 2015-2022 runtime (x64)'
$vc = Join-Path $redist 'vc_redist.x64.exe'
if (Test-Path $vc) { Ok 'already downloaded' }
else {
    try { Get-File 'https://aka.ms/vs/17/release/vc_redist.x64.exe' $vc; Ok 'downloaded' }
    catch { Warn "Could not download the VC++ runtime ($($_.Exception.Message)). Download it by hand from https://aka.ms/vs/17/release/vc_redist.x64.exe and save it as $vc" }
}

# ---------------------------------------------------------------- 6. Csound (private copy, no installer)
Step 'Csound'
$csDir = $null
if ($CsoundDir -and (Test-Path (Join-Path $CsoundDir 'bin\csound.exe')) -or ($CsoundDir -and (Test-Path (Join-Path $CsoundDir 'csound.exe')))) { $csDir = $CsoundDir }
if (-not $csDir) {
    # newest folder called Csound* that holds csound.exe (Csound7 is preferred over Csound6)
    $cands = foreach ($r in @($env:ProgramFiles, ${env:ProgramFiles(x86)}, (Join-Path $env:LOCALAPPDATA 'Programs'))) {
        if ($r -and (Test-Path $r)) { Get-ChildItem $r -Directory -Filter 'Csound*' -ErrorAction SilentlyContinue |
            Where-Object { (Test-Path (Join-Path $_.FullName 'bin\csound.exe')) -or (Test-Path (Join-Path $_.FullName 'csound.exe')) } }
    }
    $pick = $cands | Sort-Object Name -Descending | Select-Object -First 1
    if ($pick) { $csDir = $pick.FullName }
}
if ($csDir) {
    $csOut = Join-Path $stage 'Csound'
    if ($FullCsound) {
        Copy-Item $csDir $csOut -Recurse
        Get-ChildItem $csOut -Recurse -File -Include 'unins*.exe','unins*.dat','unins*.msg' | Remove-Item -Force
        Ok "Csound copied completely from $csDir  ($(SizeMB $csOut))"
    } else {
        # Slim engine copy: only what loads and runs the Csound engine
        #   - csound.exe            (opcode list, device list, offline render)
        #   - every *.dll beside it (csound64 / csound7_64 API library + the libraries it needs)
        #   - the opcode plugin folders ("plugins*", *.dll only)
        # Left out: docs, examples, include files, samples, other command-line tools, uninstaller, frontends.
        $binDir = if (Test-Path (Join-Path $csDir 'bin\csound.exe')) { Join-Path $csDir 'bin' } else { $csDir }
        $rel = { param($p) $p.Substring($csDir.TrimEnd('\').Length).TrimStart('\') }
        $copied = 0
        function CopyKeep($file) {
            $dest = Join-Path $csOut (& $rel $file.FullName)
            New-Item -ItemType Directory -Force (Split-Path $dest -Parent) | Out-Null
            Copy-Item $file.FullName $dest; $script:copied++
        }
        Get-ChildItem $binDir -File | Where-Object { $_.Name -ieq 'csound.exe' -or $_.Extension -ieq '.dll' } | ForEach-Object { CopyKeep $_ }
        # EVERY other DLL anywhere in the Csound folder (opcode plugins can live in plugins64, plugins64-7.0, lib\csound\plugins..., etc.).
        # Only documentation / examples / samples / frontends folders are skipped.
        $skipDirs = '\\(doc|docs|examples|samples|tutorials?|include|manual|csoundqt|cabbage|uninstall|python|java)\\'
        $dropped = 0
        Get-ChildItem $csDir -Recurse -File -Filter '*.dll' | ForEach-Object {
            if ($_.FullName -match $skipDirs) { return }
            if (Test-Path (Join-Path $csOut (& $rel $_.FullName))) { return }   # already copied from bin
            if ($DropPlugins -and $_.BaseName -match $DropPlugins) { $dropped++ } else { CopyKeep $_ }
        }
        $full = (Get-ChildItem $csDir -Recurse -File | Measure-Object Length -Sum).Sum / 1MB
        Ok ("Csound engine only: {0} files, {1} (installed folder is {2:N0} MB){3}" -f $copied, (SizeMB $csOut), $full, $(if ($dropped) { ", $dropped plugin(s) left out" } else { '' }))
    }
    $dlls = Get-ChildItem $csOut -Recurse -File -Include 'csound64*.dll','csound7*.dll' | ForEach-Object { $_.Name }
    if ($dlls) { Write-Host "         API library: $($dlls -join ', ')" }
    else { Warn 'No csound64.dll / csound7_64.dll found in that Csound folder - live Csound clips need it.' }
    if (-not (Get-ChildItem $csOut -Directory -Recurse -Filter 'plugins*' -ErrorAction SilentlyContinue)) { Warn 'No "plugins*" folder found in that Csound copy - many opcodes would be missing.' }
    if (-not $IncludeFfmpeg -and -not (Get-ChildItem $csOut -Recurse -File | Where-Object { $_.Name -match '^(lib)?sndfile(-1)?\.dll$' })) {
        Warn 'No sndfile DLL in the Csound copy: CAW has no native MP3/FLAC encoder then. Run with -IncludeFfmpeg, or use -FullCsound.'
    }
    # Check: the copy must start AND know every opcode the full install knows.
    # csound.exe prints to stderr, so output goes to files. The plugin path is pointed at the copy for the test,
    # because an installed Csound sets OPCODE*DIR64 system-wide and would hide missing plugins.
    function ListOpcodes($exe, $pluginDir) {
        $o = [IO.Path]::GetTempFileName(); $e = [IO.Path]::GetTempFileName()
        $old6 = $env:OPCODE6DIR64; $old7 = $env:OPCODE7DIR64
        try {
            if ($pluginDir) { $env:OPCODE6DIR64 = $pluginDir; $env:OPCODE7DIR64 = $pluginDir }
            $proc = Start-Process -FilePath $exe -ArgumentList '-z' -NoNewWindow -Wait -PassThru -RedirectStandardOutput $o -RedirectStandardError $e
            $names = @(Get-Content $o, $e -ErrorAction SilentlyContinue | Where-Object { $_ -and $_.Trim() } | ForEach-Object { @($_.Trim() -split '\s+')[0] } | Where-Object { $_ -match '^[A-Za-z_][A-Za-z0-9_.:]*$' } | Sort-Object -Unique)
            return [pscustomobject]@{ Code = $proc.ExitCode; Names = $names }
        } finally { $env:OPCODE6DIR64 = $old6; $env:OPCODE7DIR64 = $old7; Remove-Item $o, $e -Force -ErrorAction SilentlyContinue }
    }
    $testExe = Get-ChildItem $csOut -Recurse -File -Filter 'csound.exe' | Select-Object -First 1
    $origExe = Get-ChildItem $csDir -Recurse -File -Filter 'csound.exe' | Select-Object -First 1
    $report = Join-Path $stage 'csound-copy-report.txt'
    if ($testExe -and -not $FullCsound) {
        # every DLL of the install that did NOT make it into the copy
        $srcDlls = Get-ChildItem $csDir -Recurse -File -Filter '*.dll' | ForEach-Object { & $rel $_.FullName }
        $missingDlls = @($srcDlls | Where-Object { -not (Test-Path (Join-Path $csOut $_)) })
        # plugin folder = the folder (not bin) that holds most DLLs in the copy
        $plugDir = Get-ChildItem $csOut -Recurse -File -Filter '*.dll' | Where-Object { $_.DirectoryName -ne $testExe.DirectoryName } |
                   Group-Object DirectoryName | Sort-Object Count -Descending | Select-Object -First 1
        $plug = if ($plugDir) { $plugDir.Name } else { $null }
        $slim = $null; $orig = $null
        # A broken copy can fail to launch, and Start-Process would then abort the
        # whole script ($ErrorActionPreference is Stop). Turn that into the same
        # warning the verdict below intends, instead of dying mid-way.
        try { $slim = ListOpcodes $testExe.FullName $plug } catch { Warn "The slim Csound copy does not start ($($_.Exception.Message)) - a needed DLL is missing. Run again with -FullCsound, or tell me the DLL name." }
        try { if ($origExe) { $orig = ListOpcodes $origExe.FullName $null } } catch { $orig = $null }
        $missingOps = if ($orig -and $slim) { @($orig.Names | Where-Object { $slim.Names -notcontains $_ }) } else { @() }
        @("Csound copy check  $(Get-Date)", "source: $csDir", "copy  : $csOut", "plugin folder used for the test: $plug",
          "opcodes known: copy=$(if ($slim) { $slim.Names.Count } else { 'did not start' })  full install=$(if ($orig) { $orig.Names.Count } else { 'n/a' })", '',
          "DLLs of the install NOT in the copy ($($missingDlls.Count)):") + ($missingDlls | ForEach-Object { "  $_" }) +
          @('', "Opcodes the full install knows but the copy does NOT ($($missingOps.Count)):") + ($missingOps | ForEach-Object { "  $_" }) |
          Set-Content $report -Encoding UTF8
        if (-not $slim) { }   # the copy would not start - the warning above already says it
        elseif ($slim.Code -lt -1000000) { Warn "The slim Csound copy does not start (exit code $($slim.Code)) - a needed DLL is missing. Run again with -FullCsound, or tell me the DLL name." }
        elseif ($missingOps.Count) { Warn "The Csound copy is missing $($missingOps.Count) opcode(s), e.g. $(($missingOps | Select-Object -First 8) -join ', '). See stage\csound-copy-report.txt (it lists the missing DLLs and opcodes). Run again with -FullCsound to be safe." }
        else { Ok "Csound copy knows all $($slim.Names.Count) opcodes of the full install ($($missingDlls.Count) non-engine DLLs left out, listed in stage\csound-copy-report.txt)" }
    } elseif (-not $testExe) { Warn 'csound.exe was not copied.' }
} else {
    Warn 'No installed Csound found (looked for Program Files\Csound*\bin\csound.exe). Install Csound on this PC once, or run again with -CsoundDir "C:\path\to\Csound7".'
}

# ---------------------------------------------------------------- summary
Step 'Summary'
Write-Host "  stage  : $(SizeMB $stage)"
Write-Host "  redist : $(SizeMB $redist)"
if ($warn.Count) {
    Write-Host ''
    Write-Host "  $($warn.Count) warning(s) above. Fix them before building the installer, or the matching part will be missing." -ForegroundColor Yellow
    exit 2
}
Write-Host ''
Write-Host '  Everything is ready. Now run 2-BUILD-INSTALLER.bat' -ForegroundColor Green
