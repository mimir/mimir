#!/usr/bin/env bash

set -euo pipefail

root=$(dirname "$0")/..
exec "$root/lit/lit" "$root/build/lit" -a --filter "$@"
