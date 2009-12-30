AC_PREREQ([2.57])

m4_define([SPICE_MAJOR], 0)
m4_define([SPICE_MINOR], 4)
m4_define([SPICE_MICRO], 1)

AC_INIT(spice, [SPICE_MAJOR.SPICE_MINOR.SPICE_MICRO], [], spice)

AC_CONFIG_MACRO_DIR([m4])

# Define default SPICE_COMMON_SRCDIR
# Maybe overwritten by ./configure.ac.shared
SPICE_COMMON_SRCDIR='$(top_srcdir)'/common

m4_include(configure.ac.shared)

AC_PROG_LIBTOOL

SPICE_LT_VERSION=m4_format("%d:%d:%d", SPICE_MAJOR, SPICE_MINOR, SPICE_MICRO)
AC_SUBST(SPICE_LT_VERSION)


if test "$platform_win32" = yes; then
  red_target=windows
else
  red_target=x11
fi
AC_SUBST(red_target)

dnl =========================================================================
dnl Check deps


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

dnl =========================================================================
dnl -fvisibility stuff

have_gcc4=no
AC_MSG_CHECKING(for -fvisibility)
AC_COMPILE_IFELSE([
#if defined(__GNUC__) && (__GNUC__ >= 4)
#else
error Need GCC 4.0 for visibility
#endif
int main () { return 0; }
], have_gcc4=yes)

if test "x$have_gcc4" = "xyes"; then
   VISIBILITY_HIDDEN_CFLAGS="-fvisibility=hidden"
fi
AC_MSG_RESULT($have_gcc4)
AC_SUBST(VISIBILITY_HIDDEN_CFLAGS)

AC_SUBST(SPICE_COMMON_SRCDIR)
AC_SUBST(SPICE_REQUIRES)
AC_SUBST(SPICE_NONPKGCONFIG_CFLAGS)
AC_SUBST(SPICE_NONPKGCONFIG_LIBS)

AC_OUTPUT([
Makefile
spice.pc
common/Makefile
common/linux/ffmpeg_inc.h
server/Makefile
client/Makefile
client/x11/Makefile
client/x11/images/Makefile
])

dnl ==========================================================================
echo "

        Spice $VERSION
        ==============

        prefix:                   ${prefix}
        c compiler:               ${CC}
        c++ compiler:	          ${CXX}

        Have XRANDR 1.2:          ${have_xrandr12}

        Red target:               ${red_target}

        Now type 'make' to build $PACKAGE
"
