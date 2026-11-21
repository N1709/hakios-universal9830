#!/bin/bash
# Host-only file-backed simulation and cross-compilation; never opens phone devices.
set -euo pipefail
cd "$(dirname "$0")/.."
go_tool=$(python3 tools/release/go_toolchain.py)
export GOMAXPROCS=4 GOFLAGS='-p=4'
export GOTOOLCHAIN=local CGO_ENABLED=0 GOPROXY=off GOSUMDB=off
export GOCACHE="$PWD/.cache/go-build" GOPATH="$PWD/.cache/go-path"
mkdir -p out/twrp
(cd tools/recovery && "$go_tool" test -count=1 -v ./... && "$go_tool" vet ./...)
(cd tools/recovery && GOOS=linux GOARCH=arm64 GOARM64=v8.0 "$go_tool" build -trimpath -buildvcs=false -o ../../out/twrp/update-binary-check .)
