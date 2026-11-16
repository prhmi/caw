#!/usr/bin/env bash
set -Eeuo pipefail

# Run by BUILD-LIBPD.bat inside the MSYS2 MinGW 64-bit environment.
: "${USERPROFILE:?Windows user profile not found}"
: "${CAW_RESULT_FILE:?Result file not set}"
: "${CAW_HELPER_DIR:?Helper folder not set}"
base="$(cygpath -u "$USERPROFILE")/CAW-libpd"
mkdir -p "$base"
out="$(mktemp -d "$base/build-XXXXXXXX")"
exec > >(tee "$out/build.log") 2>&1
trap 'printf "\nBuild failed. Details are in: %s/build.log\n" "$out"' ERR
printf '\nBuild output: %s\n\n' "$out"

# Use official MSYS2 packages and official libpd source. No existing clone or
# output is overwritten. Do not run a broad system upgrade automatically.
# MSYS2 mirrors sometimes answer HTTP 500 for a few minutes. Skip the install
# when the tools already exist, otherwise retry, refresh the database, clear a
# possibly broken cache and put the official mirror first.
need_tools() { ! { command -v git && command -v make && command -v gcc; } >/dev/null 2>&1; }
prefer_official_mirror() {
    local f line
    for f in /etc/pacman.d/mirrorlist.msys /etc/pacman.d/mirrorlist.mingw /etc/pacman.d/mirrorlist.mingw64; do
        [[ -f "$f" ]] || continue
        grep -q 'repo.msys2.org' "$f" && continue
        case "$f" in
            *msys) line='Server = https://repo.msys2.org/msys/$arch/' ;;
            *) line='Server = https://repo.msys2.org/mingw/mingw64/' ;;
        esac
        cp -n "$f" "$f.caw-backup" 2>/dev/null || true
        { printf '%s\n' "$line"; cat "$f"; } > "$f.tmp" && mv "$f.tmp" "$f"
    done
}
install_tools() {
    local attempt
    for attempt in 1 2 3 4; do
        printf '\nInstalling build tools (attempt %s of 4)...\n' "$attempt"
        if pacman -Sy --needed --noconfirm git make mingw-w64-x86_64-gcc; then return 0; fi
        printf '\nDownload failed (a mirror may be down). Switching to the official mirror and retrying...\n'
        [[ "$attempt" == 1 ]] && prefer_official_mirror
        [[ "$attempt" == 2 ]] && yes | pacman -Scc >/dev/null 2>&1 || true
        sleep $((attempt * 10))
    done
    return 1
}
if need_tools; then
    install_tools || { echo 'Could not install the build tools: the MSYS2 mirrors keep failing. Wait a few minutes and run build.bat again.'; exit 1; }
else
    echo 'git, make and gcc are already installed; skipping package download.'
fi
printf '\nDownloading official libpd version 0.16.1 and its PD submodule...\n'
for attempt in 1 2 3; do
    rm -rf "$out/source"
    git clone --depth 1 --branch 0.16.1 --recurse-submodules https://github.com/libpd/libpd.git "$out/source" && break
    [[ "$attempt" == 3 ]] && { echo 'Could not download libpd source.'; exit 1; }
    echo 'Download failed, retrying...'; sleep 10
done
cd "$out/source"
actual="$(git rev-parse HEAD)"
if [[ "$actual" != "ba0dc63262901d658af8bbda5e619a60fa975e78" ]]; then
    echo 'Unexpected libpd revision. Stopping rather than building unknown code.'
    exit 1
fi
printf '\nCompiling with up to two workers...\n'
make -j2 'ADDITIONAL_CFLAGS=-DPD_LONGINTTYPE="long long"'
# The upstream Makefile calls the Windows output pd.dll, not libpd.dll.
if [[ ! -s libs/pd.dll ]]; then echo 'The expected pd.dll was not created.'; exit 1; fi
cp libs/pd.dll "$out/libpd.dll"
thread_dll="${MINGW_PREFIX:-/mingw64}/bin/libwinpthread-1.dll"
if [[ ! -s "$thread_dll" ]]; then echo 'The MinGW runtime DLL was not found.'; exit 1; fi
cp "$thread_dll" "$out/libwinpthread-1.dll"
helper="$(cygpath -u "$CAW_HELPER_DIR")"
cp "$helper/OPEN-CAW.bat" "$out/OPEN-CAW.bat"
printf '%s\n' 'Build uses official libpd 0.16.1 and its checked-out Pure Data submodule.' \
 'Start CAW with OPEN-CAW.bat so Windows can find libwinpthread-1.dll.' \
 'Then choose libpd.dll in CAW PD Preview.' > "$out/READ-ME.txt"
cygpath -w "$out" > "$(cygpath -u "$CAW_RESULT_FILE")"
printf '\nDone! The libpd.dll and the CAW launcher are ready.\n'
