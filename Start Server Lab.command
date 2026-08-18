#!/usr/bin/env bash
set -e
repo_root="$(cd "$(dirname "$0")" && pwd)"
cd "${repo_root}"
exec "${repo_root}/start.sh"
