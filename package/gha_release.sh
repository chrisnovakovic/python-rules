#!/bin/sh

if [ "$GITHUB_ACTIONS" != true ]; then
    echo "This script should only be executed on GitHub Actions runners." >&2
    exit 1
fi

target="$1"

./pleasew -v notice -p build "$target"
echo ARTIFACT_PATH="$(./pleasew query output $target)" >> $GITHUB_ENV
