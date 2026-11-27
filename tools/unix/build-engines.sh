#!/usr/bin/env bash
# CAW - build every engine CAW can use on Linux / macOS: Csound 7, libpd, SuperCollider.
#   ./build-engines.sh                 all three
#   ./build-engines.sh csound pd       only those
# Versions:  CSOUND_TAG=7.0.0-beta.18  LIBPD_REF=master  SC_TAG=Version-3.14.0
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
want=("$@"); [ ${#want[@]} -eq 0 ] && want=(csound pd sc)
declare -A result
for e in "${want[@]}"; do
  case "$e" in
    csound) s=build-csound.sh ;;
    pd|libpd) s=build-libpd.sh ;;
    sc|supercollider) s=build-supercollider.sh ;;
    *) echo "unknown engine: $e (csound, pd, sc)"; exit 1 ;;
  esac
  echo; echo "################ $e ################"
  if bash "$here/$s"; then result[$e]="ok"; else result[$e]="FAILED"; fi
done
echo; echo "================ Summary ================"
for e in "${want[@]}"; do printf '  %-8s %s\n' "$e" "${result[$e]}"; done
echo "Restart CAW afterwards."
