#!/usr/bin/env bash
set -euo pipefail

build_helper=$1
test_directory=$(mktemp -d /tmp/mvstab-build-helper-test.XXXXXX)
trap 'rm -rf -- "$test_directory"' EXIT

mkdir "$test_directory/existing-source"
if "$build_helper" "$test_directory/existing-source" \
    "$test_directory/install" 1 >"$test_directory/source.log" 2>&1; then
    exit 1
fi
grep 'source directory already exists' "$test_directory/source.log" >/dev/null

mkdir "$test_directory/existing-install"
if "$build_helper" "$test_directory/source" \
    "$test_directory/existing-install" 1 >"$test_directory/install.log" 2>&1; then
    exit 1
fi
grep 'install prefix already exists' "$test_directory/install.log" >/dev/null
test ! -e "$test_directory/source"

if "$build_helper" "$test_directory/same" \
    "$test_directory/same" 1 >"$test_directory/same.log" 2>&1; then
    exit 1
fi
grep 'must differ' "$test_directory/same.log" >/dev/null
test ! -e "$test_directory/same"

if "$build_helper" "$test_directory/alias" \
    "$test_directory/alias/." 1 >"$test_directory/alias.log" 2>&1; then
    exit 1
fi
grep 'must differ' "$test_directory/alias.log" >/dev/null
test ! -e "$test_directory/alias"
