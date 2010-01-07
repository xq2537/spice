AC_PREREQ([2.57])

m4_define([SPICE_MAJOR], 0)
m4_define([SPICE_MINOR], 4)
m4_define([SPICE_MICRO], 1)

AC_INIT(spice-client, [SPICE_MAJOR.SPICE_MINOR.SPICE_MICRO], [], spice-client)

AC_CONFIG_MACRO_DIR([m4])

# Define default SPICE_COMMON_SRCDIR
# Maybe overwritten by ../configure.ac.shared
SPICE_COMMON_SRCDIR='$(top_srcdir)'/../common

m4_include(configure.ac.shared)


dnl ================
dnl    Check platform
dnl ================
if test "$platform_win32" = yes; then
  red_target=windows
else
  red_target=x11
fi
AC_SUBST(red_target)

dnl =================
dnl    Check deps
dnl =================


PKG_CHECK_MODULES(XRANDR, xrandr)
AC_SUBST(XRANDR_CFLAGS)
AC_SUBST(XRANDR_LIBS)
SPICE_REQUIRES+=" xrandr"

PKG_CHECK_MODULES(XRANDR12,
        xrandr >= 1.2,
        have_xrandr12=yes,
        have_xrandr12=no)

AM_CONDITIONAL([HAVE_XRANDR12], [test "x$have_xrandr12" = "xyes"])
if test "x$have_xrandr12" = "xyes" ; then
  AC_DEFINE([HAVE_XRANDR12], [], [Define if we have XRANDR 12])
fi


PKG_CHECK_MODULES(LOG4CPP, log4cpp)
AC_SUBST(LOG4CPP_CFLAGS)
AC_SUBST(LOG4CPP_LIBS)
SPICE_REQUIRES+=" log4cpp"


AC_SUBST(SPICE_COMMON_SRCDIR)
AC_SUBST(SPICE_REQUIRES)
AC_SUBST(SPICE_NONPKGCONFIG_CFLAGS)
AC_SUBST(SPICE_NONPKGCONFIG_LIBS)

AC_CONFIG_FILES([Makefile x11/Makefile x11/images/Makefile x11/ffmpeg_inc.h])
AC_OUTPUT

dnl ==========================================================================
echo "

        Spice Client $VERSION
        ==============

        prefix:                   ${prefix}
        c compiler:               ${CC}
        c++ compiler:	          ${CXX}

        Have XRANDR 1.2:          ${have_xrandr12}

        Red target:               ${red_target}

        Now type 'make' to build $PACKAGE
"
