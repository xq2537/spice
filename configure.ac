AC_PREREQ([2.57])

m4_define([SPICE_MAJOR], 0)
m4_define([SPICE_MINOR], 5)
m4_define([SPICE_MICRO], 2)

AC_INIT(spice, [SPICE_MAJOR.SPICE_MINOR.SPICE_MICRO], [], spice)

AC_CONFIG_MACRO_DIR([m4])
AM_CONFIG_HEADER([config.h])
AC_CONFIG_AUX_DIR(.)

AM_INIT_AUTOMAKE([dist-bzip2])
AM_MAINTAINER_MODE

# Define default SPICE_COMMON_SRCDIR
SPICE_COMMON_SRCDIR='$(top_srcdir)'/common

AS_IF([test "$CFLAGS" = ""], [], [_cflags_is_set=yes])
AC_PROG_CC
AS_IF([test "$CXXFLAGS" = ""], [], [_cxxflags_is_set=yes])
AC_PROG_CXX
AC_PROG_INSTALL
AC_CANONICAL_HOST
AC_PROG_LIBTOOL
AM_PROG_CC_C_O

SPICE_LT_VERSION=m4_format("%d:%d:%d", SPICE_MAJOR, SPICE_MINOR, SPICE_MICRO)
AC_SUBST(SPICE_LT_VERSION)

# Check for the CPU we are using
#
AC_MSG_CHECKING(for x86 or x86-64 platform)
case $host_cpu in
  i386|i486|i586|i686|i786|k6|k7)
	variant=32
        ;;
  x86_64)
	variant=64
	;;
  *)
	AC_MSG_RESULT(no)
	echo Only x86 and x86-64 are supported
	exit 1
esac
AC_MSG_RESULT($variant bit)
AM_CONDITIONAL([X86_64], [test "$variant" = 64])

AC_MSG_CHECKING([for native Win32])
case "$host" in
  *-*-mingw*)
    os_win32=yes
    ;;
  *)
    os_win32=no
    ;;
esac
AC_MSG_RESULT([$os_win32])

case $host in
  *-*-linux*)
    os_linux=yes
    ;;
esac

dnl =========================================================================
dnl Check OS target

AC_MSG_CHECKING([for some Win32 platform])
case "$host" in
  *-*-mingw*|*-*-cygwin*)
    platform_win32=yes
    ;;
  *)
    platform_win32=no
    ;;
esac
AC_MSG_RESULT([$platform_win32])
if test "$platform_win32" = yes; then
  red_target=windows
else
  red_target=x11
fi
AC_SUBST(red_target)

AM_CONDITIONAL(OS_WIN32, test "$os_win32" = "yes")
AM_CONDITIONAL(OS_UNIX, test "$os_win32" != "yes")
AM_CONDITIONAL(OS_LINUX, test "$os_linux" = "yes")

dnl =========================================================================
dnl Chek optional features
have_tunnel=no
AC_ARG_ENABLE(tunnel,
[  --enable-tunnel         Enable network redirection],
[  have_tunnel=yes])
AM_CONDITIONAL(SUPPORT_TUNNEL, test "x$have_tunnel" = "xyes")
dnl =========================================================================
dnl Check deps

PKG_CHECK_MODULES(PROTOCOL, spice-protocol)
AC_SUBST(PROTOCOL_CFLAGS)
SPICE_REQUIRES+=" log4cpp"


AC_CHECK_LIBM
AC_SUBST(LIBM)

AC_CHECK_LIB(rt, clock_gettime,
   AC_DEFINE(HAVE_CLOCK_GETTIME, 1, [Defined if we have clock_gettime()])
   LIBRT=-lrt
   )
AC_SUBST(LIBRT)

SPICE_NONPKGCONFIG_LIBS+=" -pthread $LIBM $LIBRT"

SPICE_REQUIRES=""

PKG_CHECK_MODULES(LOG4CPP, log4cpp)
AC_SUBST(LOG4CPP_CFLAGS)
AC_SUBST(LOG4CPP_LIBS)
SPICE_REQUIRES+=" log4cpp"

PKG_CHECK_MODULES(CEGUI, CEGUI >= 0.6.0 CEGUI < 0.7.0)
AC_SUBST(CEGUI_CFLAGS)
AC_SUBST(CEGUI_LIBS)
SPICE_REQUIRES+=" CEGUI"

if test "x$have_tunnel" = "xyes"; then
	PKG_CHECK_MODULES(SLIRP, slirp)
	AC_SUBST(SLIRP_CFLAGS)
	AC_SUBST(SLIRP_LIBS)
	SPICE_REQUIRES+=" slirp"
	AC_DEFINE([HAVE_SLIRP], [], [Define if we have slirp])
fi

PKG_CHECK_MODULES(PIXMAN, pixman-1 >= 0.17.7)
AC_SUBST(PIXMAN_CFLAGS)
AC_SUBST(PIXMAN_LIBS)
SPICE_REQUIRES+=" pixman-1 >= 0.17.7"

PKG_CHECK_MODULES(CELT051, celt051 >= 0.5.1.1)
AC_SUBST(CELT051_CFLAGS)
AC_SUBST(CELT051_LIBS)
AC_SUBST(CELT051_LIBDIR)
SPICE_REQUIRES+=" celt051 >= 0.5.1.1"

PKG_CHECK_MODULES(ALSA, alsa)
AC_SUBST(ALSA_CFLAGS)
AC_SUBST(ALSA_LIBS)
SPICE_REQUIRES+=" alsa"

PKG_CHECK_MODULES(SSL, openssl)
AC_SUBST(SSL_CFLAGS)
AC_SUBST(SSL_LIBS)
SPICE_REQUIRES+=" openssl"

# These are commented out because the gl libraries on RHEL 5 do not have pkgconfig files
#
# PKG_CHECK_MODULES(GL, gl glu)
# AC_SUBST(GL_CFLAGS)
# AC_SUBST(GL_LIBS)
# SPICE_REQUIRES+=" gl glu"

have_gl=yes
AC_CHECK_LIB(GL, glBlendFunc, GL_LIBS="$GL_LIBS -lGL", have_gl=no)
AC_CHECK_LIB(GLU, gluSphere, GL_LIBS="$GL_LIBS -lGLU", have_gl=no)
GL_CFLAGS="-DGL_GLEXT_PROTOTYPES"

if test "x$have_gl" = "xno"; then
   AC_MSG_ERROR([GL libraries not available])
fi

AC_SUBST(GL_CFLAGS)
AC_SUBST(GL_LIBS)
SPICE_NONPKGCONFIG_LIBS+=" $GL_LIBS"

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

PKG_CHECK_MODULES(MISC_X, x11 xext xrender)
AC_SUBST(MISC_X_CFLAGS)
AC_SUBST(MISC_X_LIBS)
SPICE_REQUIRES+=" x11 xext xrender"

# Add parameter for (partial) static linkage of spice client.
# this is used to achive single binary package for all (?) distros.
AC_ARG_ENABLE(static-linkage, 
	[  --enable-static-linkage will generate spice client binary with static linkage to external libraries ],
	[SPICEC_STATIC_LINKAGE_BSTATIC=["-Wl,-Bstatic"];
	SPICEC_STATIC_LINKAGE_BDYNAMIC=["-Wl,-Bdynamic"]])


AS_IF([test "$_cflags_is_set" = "yes"], [], [
    CFLAGS="-g -O2"
])


AS_IF([test "$_cxxflags_is_set" = "yes"], [], [
    CXXFLAGS="-g -O2"
])

AC_CHECK_LIB(jpeg, jpeg_destroy_decompress,
    AC_MSG_CHECKING([for jpeglib.h])
    AC_TRY_CPP(
[#include <stdio.h>
#undef PACKAGE
#undef VERSION
#undef HAVE_STDLIB_H
#include <jpeglib.h>],
        JPEG_LIBS='-ljpeg'
        AC_MSG_RESULT($jpeg_ok),
	AC_MSG_ERROR([jpeglib.h not found])),
    AC_MSG_ERROR([libjpeg not found]))
AC_SUBST(JPEG_LIBS)

dnl ===========================================================================
dnl check compiler flags

AC_DEFUN([SPICE_CC_TRY_FLAG], [
  AC_MSG_CHECKING([whether $CC supports $1])

  spice_save_CFLAGS="$CFLAGS"
  CFLAGS="$CFLAGS $1"

  AC_COMPILE_IFELSE([ ], [spice_cc_flag=yes], [spice_cc_flag=no])
  CFLAGS="$spice_save_CFLAGS"

  if test "x$spice_cc_flag" = "xyes"; then
    ifelse([$2], , :, [$2])
  else
    ifelse([$3], , :, [$3])
  fi
  AC_MSG_RESULT([$spice_cc_flag])
])


dnl Use lots of warning flags with with gcc and compatible compilers

dnl Note: if you change the following variable, the cache is automatically
dnl skipped and all flags rechecked.  So there's no need to do anything
dnl else.  If for any reason you need to force a recheck, just change
dnl MAYBE_WARN in an ignorable way (like adding whitespace)

dnl MAYBE_WARN="-Wall -Wno-sign-compare -Werror -Wno-deprecated-declarations"

MAYBE_WARN="-Wall -Wno-sign-compare -Werror -Wno-deprecated-declarations"


# invalidate cached value if MAYBE_WARN has changed
if test "x$spice_cv_warn_maybe" != "x$MAYBE_WARN"; then
	unset spice_cv_warn_cflags
fi
AC_CACHE_CHECK([for supported warning flags], spice_cv_warn_cflags, [
	echo
	WARN_CFLAGS=""

	# Some warning options are not supported by all versions of
	# gcc, so test all desired options against the current
	# compiler.
	#
	# Note that there are some order dependencies
	# here. Specifically, an option that disables a warning will
	# have no net effect if a later option then enables that
	# warnings, (perhaps implicitly). So we put some grouped
	# options (-Wall and -Wextra) up front and the -Wno options
	# last.

	for W in $MAYBE_WARN; do
		SPICE_CC_TRY_FLAG([$W], [WARN_CFLAGS="$WARN_CFLAGS $W"])
	done

	spice_cv_warn_cflags=$WARN_CFLAGS
	spice_cv_warn_maybe=$MAYBE_WARN

	AC_MSG_CHECKING([which warning flags were supported])])
WARN_CFLAGS="$spice_cv_warn_cflags"
SPICE_CFLAGS="$SPICE_CFLAGS $WARN_CFLAGS"

# We only wish to enable attribute(warn_unused_result) if we can prevent
# gcc from generating thousands of warnings about the misapplication of the
# attribute to void functions and variables.
AC_MSG_CHECKING([how to enable unused result warnings])
warn_unused_result=""
if echo $WARN_CFLAGS | grep -e '-Wno-attributes' >/dev/null; then
    AC_TRY_COMPILE([__attribute__((__warn_unused_result__))
	int f (int i) { return i; }], [],
	[warn_unused_result="__attribute__((__warn_unused_result__))"])
fi
AC_DEFINE_UNQUOTED([WARN_UNUSED_RESULT], [$warn_unused_result],
	  [Define to the value your compiler uses to support the warn-unused-result attribute])
AC_MSG_RESULT([$warn_unused_result])

AC_SUBST(WARN_CFLAGS)
AC_SUBST(CFLAGS_CFLAGS)

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

AC_SUBST([SPICEC_STATIC_LINKAGE_BSTATIC])
AC_SUBST([SPICEC_STATIC_LINKAGE_BDYNAMIC])

AC_OUTPUT([
Makefile
spice-server.pc
common/Makefile
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

        Support tunneling:        ${have_tunnel}

        Red target:               ${red_target}

        Now type 'make' to build $PACKAGE
"
