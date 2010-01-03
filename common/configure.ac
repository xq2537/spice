AC_PREREQ([2.57])

m4_define([SPICE_MAJOR], 0)
m4_define([SPICE_MINOR], 4)
m4_define([SPICE_MICRO], 1)

AC_INIT(spice-common, [SPICE_MAJOR.SPICE_MINOR.SPICE_MICRO], [], spice-common)

AC_CONFIG_MACRO_DIR([m4])

m4_include(../configure.ac.shared)

AC_CONFIG_FILES([Makefile linux/ffmpeg_inc.h])
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
