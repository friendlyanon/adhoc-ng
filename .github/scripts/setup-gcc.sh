#!/bin/sh

f() { sudo ln -f -s "$(command -v "$1")" "/usr/local/bin/$2"; }

set -x -e

sudo add-apt-repository --yes --update ppa:ubuntu-toolchain-r/test
sudo apt-get update
sudo DEBIAN_FRONTEND=noninteractive apt-get install -yq --no-install-recommends "g++-$1"

f "gcc-$1" cc
f "gcc-$1" gcc
f "g++-$1" c++
f "g++-$1" g++
