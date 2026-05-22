#!/usr/bin/env bash
set -euo pipefail

tap_name="${1:-cxlmemsim/bpftime}"
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
formula_src="${script_dir}/Formula/bpftime.rb"

if ! brew tap | grep -qx "${tap_name}"; then
  brew tap-new "${tap_name}"
fi

tap_repo="$(brew --repository "${tap_name}")"
mkdir -p "${tap_repo}/Formula"
cp "${formula_src}" "${tap_repo}/Formula/bpftime.rb"

brew install "${tap_name}/bpftime"
