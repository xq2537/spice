#!/bin/sh

set -e # exit on errors

ARGV0=$0

# Allow invocation from a separate build directory; in that case, we change
# to the source directory to run the auto*, then change back before running configure
srcdir=`dirname $ARGV0`
test -z "$srcdir" && srcdir=.

ORIGDIR=`pwd`
cd $srcdir

subdirs=$(grep '^AC_CONFIG_SUBDIRS' configure.ac | \
          sed 's/AC_CONFIG_SUBDIRS(\[\(.*\)\]) *$/\1/')
echo "subdirs are $subdirs"

for d in $subdirs; do
   echo "START  configuring     $d"
   cd $d
   ./autogen.sh
   cd ..
   echo "FINISH configuring     $d"
done

echo "configuring SPICE top dir"
./autogen.sh.shared

cd $ORIGDIR || exit $?
rm -f config.cache
