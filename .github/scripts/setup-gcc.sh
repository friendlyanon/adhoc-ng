#!/bin/sh

set -x -e

suffix=
target=${2-}
case $target in
  arm64) target=aarch64 ;;
  arm*) suffix=eabihf ;;
  amd64) target=x86_64 ;;
  x86) target=i686 ;;
  *) ;;
esac

sudo apt-get update

case $target in '')
  sudo DEBIAN_FRONTEND=noninteractive \
    apt-get install -yq --no-install-recommends "gcc-$1" "g++-$1"

  printf 'CC=gcc-%d\nCXX=g++-%d\n' "$1" "$1" >> "$GITHUB_ENV"
  exit 0
esac

prefix=$target-linux-musl$suffix
url=https://github.com/friendlyanon/musl-cross-make/releases/download/2637/x86_64-$prefix.tar.xz

curl -fsSL -o "$prefix.tar.xz" "$url"
tar -xf "$prefix.tar.xz"

sudo DEBIAN_FRONTEND=noninteractive \
  apt-get install -yq --no-install-recommends musl

for a in gcc g++
do "$prefix/bin/$prefix-$a" --version
done
