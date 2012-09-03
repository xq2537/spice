#!/bin/bash

ROOT=$(dirname $0)/../../
INCLUDE="-I$ROOT -I$ROOT/spice-common/spice-protocol/"
TEMP=`mktemp -d`

cat > $TEMP/check_version.c <<EOF
#include <stdio.h>
#include "server/spice.h"

int main(void)
{
        printf("%d.%d.%d\n", SPICE_SERVER_VERSION >> 16,
               (SPICE_SERVER_VERSION >> 8) & 0xff,
               SPICE_SERVER_VERSION & 0xff);
        return 0;
}
EOF

if ! gcc -o $TEMP/check_version $INCLUDE $TEMP/check_version.c ; then
    echo "failed to compile tester"
    exit -1
fi

INCLUDE_VERSION=`$TEMP/check_version`
rm -Rf $TEMP

CONF_MAJOR=`cat $ROOT/configure.ac | grep 'm4_define(\[SPICE_MAJOR\]' | sed -e 's/m4_define(\[SPICE_MAJOR\],\(.*\))/\1/'`
CONF_MINOR=`cat $ROOT/configure.ac | grep 'm4_define(\[SPICE_MINOR\]' | sed -e 's/m4_define(\[SPICE_MINOR\],\(.*\))/\1/'`
CONF_MICRO=`cat $ROOT/configure.ac | grep 'm4_define(\[SPICE_MICRO\]' | sed -e 's/m4_define(\[SPICE_MICRO\],\(.*\))/\1/'`
CONF_VERSION=`printf %d.%d.%d $CONF_MAJOR $CONF_MINOR $CONF_MICRO`
if [ $CONF_VERSION != $INCLUDE_VERSION ] ; then
    echo "BAD: version in server/spice.h"
    echo $INCLUDE_VERSION
    echo "doesn't match version in configure.ac"
    echo $CONF_VERSION
    echo "please fix before doing a release"
    exit 1
else
    echo "OK: both versions are $CONF_VERSION"
fi
exit 0
