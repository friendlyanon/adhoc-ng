#!/bin/sh

os=linux
case $1 in windows-*) os=windows; esac
archive=adhoc-ng-$os-${2#*_}-$3

case $os in
  linux) tar -C prefix -cvJf "${archive}.tar.xz" adhoc-server-pro ;;
  windows) tar -C prefix -cvaf "${archive}.zip" adhoc-server-pro.exe ;;
esac
echo "key=$archive" >> "$GITHUB_OUTPUT"
