AC_PREREQ([2.57])

m4_define([SPICE_MAJOR], 0)
m4_define([SPICE_MINOR], 4)
m4_define([SPICE_MICRO], 2)

AC_INIT(spice-common, [SPICE_MAJOR.SPICE_MINOR.SPICE_MICRO], [], spice-common)

AC_CONFIG_MACRO_DIR([m4])
AM_CONFIG_HEADER([config.h])
AC_CONFIG_AUX_DIR(.)

AM_INIT_AUTOMAKE([dist-bzip2])
AM_MAINTAINER_MODE

AC_PROG_INSTALL
AC_CANONICAL_HOST

AC_CONFIG_FILES([Makefile spice-common.pc])
AC_OUTPUT

dnl ==========================================================================
echo "

        Spice Common $VERSION
        ==============

        prefix:                   ${prefix}
        c compiler:               ${CC}
        c++ compiler:	          ${CXX}

        Now type 'make' to build $PACKAGE
"
