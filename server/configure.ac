AC_PREREQ([2.57])

m4_define([SPICE_MAJOR], 0)
m4_define([SPICE_MINOR], 4)
m4_define([SPICE_MICRO], 1)

AC_INIT(spice-server, [SPICE_MAJOR.SPICE_MINOR.SPICE_MICRO], [], spice-server)

AC_CONFIG_MACRO_DIR([m4])

# Define default SPICE_COMMON_SRCDIR
# Maybe overwritten by ../configure.ac.shared
SPICE_COMMON_SRCDIR='$(top_srcdir)'/../common

m4_include(configure.ac.shared)

AC_PROG_LIBTOOL

SPICE_LT_VERSION=m4_format("%d:%d:%d", SPICE_MAJOR, SPICE_MINOR, SPICE_MICRO)
AC_SUBST(SPICE_LT_VERSION)

# Support only x86_64 arch
echo " ************** host cpu is $host_cpu  "
if test $host_cpu  != x86_64; then
    echo "Only x86_64 arch is supported"
    exit 1
fi


AC_SUBST(SPICE_COMMON_SRCDIR)
AC_SUBST(SPICE_REQUIRES)
AC_SUBST(SPICE_NONPKGCONFIG_CFLAGS)
AC_SUBST(SPICE_NONPKGCONFIG_LIBS)

AC_CONFIG_FILES([Makefile spice-server.pc ffmpeg_inc.h])
AC_OUTPUT

dnl ==========================================================================
echo "

        Spice Server $VERSION
        ==============

        prefix:                   ${prefix}
        c compiler:               ${CC}
        c++ compiler:	          ${CXX}

        Now type 'make' to build $PACKAGE
"
