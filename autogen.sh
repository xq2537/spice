#!/bin/sh

set -e # exit on errors

srcdir=`dirname $0`
test -z "$srcdir" && srcdir=.

olddir=`pwd`
cd "$srcdir"

git submodule update --init --recursive

mkdir -p m4
autoreconf --verbose --force --install

cd "$olddir"
if [ -z "$NOCONFIGURE" ]; then
    "$srcdir"/configure --enable-maintainer-mode ${1+"$@"}
fi
