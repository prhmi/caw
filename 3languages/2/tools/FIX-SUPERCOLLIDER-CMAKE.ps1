param(
    [Parameter(Mandatory = $true)]
    [string]$SourceRoot
)

$ErrorActionPreference = 'Stop'
$utf8NoBom = [System.Text.UTF8Encoding]::new($false)

function Read-ScText([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "SuperCollider source file not found: $Path" }
    return [System.IO.File]::ReadAllText($Path)
}
function Write-ScText([string]$Path, [string]$Text) {
    [System.IO.File]::WriteAllText($Path, $Text, $utf8NoBom)
}
function Replace-ExactlyOne([string]$Path, [string]$Pattern, [string]$Replacement, [string]$Marker, [string]$Label) {
    $text = Read-ScText $Path
    if ($text.Contains($Marker)) { return }
    $matches = [System.Text.RegularExpressions.Regex]::Matches($text, $Pattern)
    if ($matches.Count -ne 1) { throw "Could not find exactly one $Label patch point in $Path" }
    $text = [System.Text.RegularExpressions.Regex]::Replace($text, $Pattern, $Replacement, 1)
    Write-ScText $Path $text
    Write-Host "Applied $Label."
}
function Insert-BeforeOnce([string]$Path, [string]$Pattern, [string]$Insertion, [string]$Marker, [string]$Label) {
    $text = Read-ScText $Path
    if ($text.Contains($Marker)) { return }
    $matches = [System.Text.RegularExpressions.Regex]::Matches($text, $Pattern)
    if ($matches.Count -ne 1) { throw "Could not find exactly one $Label insertion point in $Path" }
    $text = $text.Insert($matches[0].Index, $Insertion)
    Write-ScText $Path $text
    Write-Host "Applied $Label."
}

# CMake install(CODE) accepts neither COMMENT nor VERBATIM.
$langCmake = Join-Path $SourceRoot 'lang\CMakeLists.txt'
$langText = Read-ScText $langCmake
$installPattern = '(?m)^[\t ]*COMMENT "Looking for libraries\.\.\."\r?\n[\t ]*VERBATIM\r?\n'
$installCount = [System.Text.RegularExpressions.Regex]::Matches($langText, $installPattern).Count
if ($installCount -eq 1) {
    $langText = [System.Text.RegularExpressions.Regex]::Replace($langText, $installPattern, '')
    Write-ScText $langCmake $langText
    Write-Host 'Applied CMake install(CODE) compatibility fix.'
} elseif ($installCount -gt 0 -or $langText.Contains('Looking for libraries...')) {
    throw "Unexpected install(CODE) block in $langCmake"
}

# Copy the custom driver into the pinned SuperCollider checkout.
$driverSource = Join-Path $PSScriptRoot 'SC_ExternalDriver.cpp'
$driverTarget = Join-Path $SourceRoot 'server\scsynth\SC_ExternalDriver.cpp'
if (-not (Test-Path -LiteralPath $driverSource -PathType Leaf)) { throw "CAW external driver missing: $driverSource" }
Copy-Item -LiteralPath $driverSource -Destination $driverTarget -Force

# Define and select AUDIOAPI=external in the SuperCollider build.
$coreAudioHeader = Join-Path $SourceRoot 'server\scsynth\SC_CoreAudio.h'
Replace-ExactlyOne $coreAudioHeader '(?m)^#define SC_AUDIO_API_WEBAUDIO 7\r?$' "#define SC_AUDIO_API_WEBAUDIO 7`r`n#define SC_AUDIO_API_EXTERNAL 8" '#define SC_AUDIO_API_EXTERNAL 8' 'external audio API constant'

$scsynthCmake = Join-Path $SourceRoot 'server\scsynth\CMakeLists.txt'
$apiValidation = 'if\(NOT AUDIOAPI MATCHES "\^\(jack\|coreaudio\|portaudio\|bela\|webaudio\)\$"\)'
$apiReplacement = 'if(NOT AUDIOAPI MATCHES "^(jack|coreaudio|portaudio|bela|webaudio|external)$")'
Replace-ExactlyOne $scsynthCmake $apiValidation $apiReplacement 'external)$' 'external audio API validation'
Insert-BeforeOnce $scsynthCmake '(?m)^set[ \t]*\([ \t]*FINAL_BUILD[ \t]+0\)' "if(AUDIOAPI STREQUAL external)`r`n list(APPEND scsynth_sources SC_ExternalDriver.cpp)`r`nendif()`r`n`r`n" 'list(APPEND scsynth_sources SC_ExternalDriver.cpp)' 'host-driven driver source'
Insert-BeforeOnce $scsynthCmake '(?m)^target_compile_definitions\(libscsynth PRIVATE BUILDING_SCSYNTH\)' "if(AUDIOAPI STREQUAL external)`r`n target_compile_definitions(libscsynth PRIVATE `"SC_AUDIO_API=SC_AUDIO_API_EXTERNAL`")`r`nendif()`r`n" 'SC_AUDIO_API=SC_AUDIO_API_EXTERNAL' 'host-driven driver compile definition'

# sclang is only an OSC client in CAW; allow it to build without a local device backend.
$langAudioPattern = 'if\(NOT AUDIOAPI MATCHES "\^\(jack\|coreaudio\|portaudio\|bela\|webaudio\)\$"\)'
$langAudioReplacement = 'if(NOT AUDIOAPI MATCHES "^(jack|coreaudio|portaudio|bela|webaudio|external)$")'
$langText = Read-ScText $langCmake
if (-not $langText.Contains('external)$')) {
    $count = [System.Text.RegularExpressions.Regex]::Matches($langText, $langAudioPattern).Count
    if ($count -ne 1) { throw "Could not patch sclang audio API validation in $langCmake" }
    $langText = [System.Text.RegularExpressions.Regex]::Replace($langText, $langAudioPattern, $langAudioReplacement, 1)
    Write-ScText $langCmake $langText
    Write-Host 'Allowed the host-driven server mode in sclang.'
}
