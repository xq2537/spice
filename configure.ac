AC_PREREQ([2.57])

m4_define([SPICE_MAJOR], 0)
m4_define([SPICE_MINOR], 4)
m4_define([SPICE_MICRO], 2)

AC_INIT(spice, [SPICE_MAJOR.SPICE_MINOR.SPICE_MICRO], [], spice)

AC_CONFIG_MACRO_DIR([m4])

m4_include(configure.ac.shared)

AC_CONFIG_SUBDIRS([common server client])
AC_CONFIG_FILES([Makefile])
AC_OUTPUT

dnl ==========================================================================
echo "

        Spice $VERSION
        ==============

        prefix:                   ${prefix}
        c compiler:               ${CC}
        c++ compiler:	          ${CXX}

        Now type 'make' to build $PACKAGE
"
