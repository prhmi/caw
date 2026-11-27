# shared helpers for the CAW engine build scripts (sourced, not run)
OS="$(uname -s)"
JOBS="$( (command -v nproc >/dev/null && nproc) || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
DEPS="$HOME/.caw_deps"
step() { printf '\n=== %s ===\n' "$1"; }
ok()   { printf '  [ok]   %s\n' "$1"; }
warn() { printf '  [warn] %s\n' "$1"; }
die()  { printf '[ERROR] %s\n' "$1" >&2; exit 1; }
need() { command -v "$1" >/dev/null 2>&1 || die "'$1' not found. $2"; }
is_apt() { [ "$OS" = "Linux" ] && command -v apt-get >/dev/null 2>&1; }
is_mac() { [ "$OS" = "Darwin" ]; }
mac_brew() { need brew "Install Homebrew from https://brew.sh"; brew install "$@" >/dev/null || true; }
