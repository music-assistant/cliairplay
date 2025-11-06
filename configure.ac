#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ([2.71])
AC_INIT([cliap2], [0.1])

AC_CONFIG_SRCDIR([config.h.in])
AC_CONFIG_MACRO_DIR([m4])
AC_CONFIG_AUX_DIR([build-aux])
AC_CONFIG_HEADERS([config.h])
AM_INIT_AUTOMAKE([foreign subdir-objects 1.11])
AM_SILENT_RULES([yes])

AC_USE_SYSTEM_EXTENSIONS

# Checks for programs.
AC_PROG_CC
AC_PROG_CC_C_O
AC_PROG_SED
AC_PROG_MKDIR_P
LT_INIT([])

AC_PATH_PROG([GPERF], [[gperf]])
# AS_IF([[test -z "$GPERF"]],
# 	[AS_IF([[test -f "$srcdir/src/dmap_fields_hash.h"]],
# 		[AM_MISSING_PROG([GPERF], [[gperf]])
# 		AC_MSG_NOTICE([[

# GNU gperf not found, but it's output appears to be present.
# If you modify any .gperf files, you will need to install it.]])],
# 		[AC_MSG_ERROR([[GNU gperf required, please install it.]])])
# 	])

AX_PROG_FLEX([AC_DEFINE([LEX], [flex], [flex found])],
	[AS_IF([test ! -f "$srcdir/src/parsers/smartpl_lexer.c"],
		[AC_MSG_ERROR([flex required, please install it])])
	])
AX_PROG_BISON([AC_DEFINE([YACC], [bison], [GNU bison found])],
	[AS_IF([test ! -f "$srcdir/src/parsers/smartpl_parser.c"],
		[AC_MSG_ERROR([GNU bison required, please install it])])
	])

dnl Enable all warnings by default.
AM_CPPFLAGS="-Wall"
AC_SUBST([AM_CPPFLAGS])

dnl Checks for header files.
AC_CHECK_HEADERS_ONCE([regex.h pthread_np.h])
AC_CHECK_HEADERS([sys/wait.h sys/param.h dirent.h getopt.h stdint.h], [],
	[AC_MSG_ERROR([[Missing header required to build OwnTone]])])
AC_CHECK_HEADERS([time.h], [],
	[AC_MSG_ERROR([[Missing header required to build OwnTone]])])
AC_CHECK_FUNCS_ONCE([posix_fadvise pipe2 gettid])
AC_CHECK_FUNCS([strptime strtok_r], [],
	[AC_MSG_ERROR([[Missing function required to build OwnTone]])])

dnl check for clock_gettime or replace it
AC_SEARCH_LIBS([clock_gettime], [rt],
	[AC_DEFINE([HAVE_CLOCK_GETTIME], 1,
		[Define to 1 if have clock_gettime function])],
	[AC_CHECK_HEADER([mach/mach_time.h],
	[AC_DEFINE([HAVE_MACH_CLOCK], 1,
		[Define to 1 if mach kernel clock replacement available])],
	[AC_MSG_ERROR([[Missing clock_gettime and any replacement]])])])

dnl check for timer_settime or replace it
AC_SEARCH_LIBS([timer_settime], [rt],
	[AC_DEFINE([HAVE_TIMER_SETTIME], 1,
		[Define to 1 if have timer_settime function])],
	[AC_CHECK_HEADER([mach/mach_time.h],
	[AC_DEFINE([HAVE_MACH_TIMER], 1,
		[Define to 1 if mach kernel clock replacement available])],
	[AC_MSG_ERROR([[Missing timer_settime and any replacement]])])])

AC_SEARCH_LIBS([pthread_exit], [pthread], [],
	[AC_MSG_ERROR([[pthreads library is required]])])
AC_SEARCH_LIBS([pthread_setname_np], [pthread],
	[dnl Validate pthread_setname_np with 2 args (some have 1)
	 AC_MSG_CHECKING([[for two-parameter pthread_setname_np]])
	 AC_LINK_IFELSE([AC_LANG_PROGRAM([[@%:@include <pthread.h>]],
		[[pthread_setname_np(pthread_self(), "name");]])],
		[AC_MSG_RESULT([yes])
		 AC_DEFINE([HAVE_PTHREAD_SETNAME_NP], 1,
			[Define to 1 if you have pthread_setname_np])],
		[AC_MSG_RESULT([[no]])])],
	[AC_SEARCH_LIBS([pthread_set_name_np], [pthread],
		[AC_CHECK_FUNCS([pthread_set_name_np])])])
AC_SEARCH_LIBS([pthread_getname_np], [pthread],
	[AC_DEFINE([HAVE_PTHREAD_GETNAME_NP], 1,
		[Define to 1 if you have pthread_getname_np])]
	[AC_SEARCH_LIBS([pthread_get_name_np], [pthread],
		[AC_DEFINE([HAVE_PTHREAD_GETNAME_NP], 1,
			[Define to 1 if you have pthread_get_name_np])])])
AC_SEARCH_LIBS([pthread_getthreadid_np], [pthread],
	[AC_DEFINE([HAVE_PTHREAD_GETTHREADID_NP], 1,
		[Define to 1 if you have pthread_getthreadid_np])])
AC_SEARCH_LIBS([uuid_generate_random], [uuid],
	[AC_DEFINE([HAVE_UUID], 1,
		[Define to 1 if you have uuid_generate_random])])
AC_SEARCH_LIBS([copy_file_range], [c],
	[AC_DEFINE([HAVE_COPY_FILE_RANGE], 1,
		[Define to 1 if you have copy_file_range])])
AC_SEARCH_LIBS([fcopyfile], [c],
	[AC_DEFINE([HAVE_FCOPYFILE], 1,
		[Define to 1 if you have fcopyfile])])

AC_SEARCH_LIBS([log10], [m])
AC_SEARCH_LIBS([lrint], [m])
AC_SEARCH_LIBS([fabs], [m])

dnl Large File Support (LFS)
AC_SYS_LARGEFILE
AC_TYPE_OFF_T

# Checks for libraries.
AC_SUBST([COMMON_LIBS])
AC_SUBST([COMMON_CPPFLAGS])
AC_SUBST([OWNTONE_LIBS])
AC_SUBST([OWNTONE_CPPFLAGS])
AC_SUBST([OWNTONE_OPTS_LIBS])
AC_SUBST([OWNTONE_OPTS_CPPFLAGS])

AM_ICONV
# All OWNTONE_ macros defined in m4/owntone_checks.m4
OWNTONE_FUNC_REQUIRE([COMMON], [GNU libunistring], [LIBUNISTRING], [unistring],
	[u8_strconv_from_locale], [uniconv.h], [],
	[# Retry test with iconv library
	 OWNTONE_VARS_PREPEND([COMMON], [LIBICONV], [INCICONV])
	 OWNTONE_FUNC_REQUIRE([COMMON], [GNU libunistring], [LIBUNISTRING],
		[unistring], [u8_strconv_from_locale], [uniconv.h])])

OWNTONE_MODULES_CHECK([OWNTONE], [ZLIB], [zlib], [deflate], [zlib.h])
OWNTONE_MODULES_CHECK([OWNTONE], [CONFUSE], [libconfuse >= 3.0], [cfg_init], [confuse.h])
OWNTONE_MODULES_CHECK([OWNTONE], [LIBCURL], [libcurl], [curl_global_init], [curl/curl.h])
OWNTONE_MODULES_CHECK([OWNTONE], [LIBSODIUM], [libsodium], [sodium_init], [sodium.h])
OWNTONE_MODULES_CHECK([OWNTONE], [LIBXML2], [libxml-2.0], [xmlInitParser], [libxml/parser.h])

OWNTONE_MODULES_CHECK([OWNTONE], [LIBEVENT], [libevent >= 2.1.4],
	[event_base_new], [event2/event.h],
	[# check for version 2.2 (with websocket server support)
	 PKG_CHECK_EXISTS([libevent >= 2.2.1],
	 	[AC_DEFINE([HAVE_LIBEVENT22], 1,
			[Define to 1 if you have libevent > 2.2])],
		[])
	])
OWNTONE_MODULES_CHECK([OWNTONE], [LIBEVENT_PTHREADS], [libevent_pthreads],
	[evthread_use_pthreads], [event2/thread.h])

# Check for evhttp_connection_get_peer() signature
AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[
	#include <event2/http.h>
	void evhttp_connection_get_peer(struct evhttp_connection *, const char **, ev_uint16_t *);
	]], [[ ]]
	)],
	[AC_MSG_RESULT([yes]); AC_DEFINE([HAVE_EVHTTP_CONNECTION_GET_PEER_CONST_CHAR], 1,
		[Define to 1 if evhttp_connection_get_peer expects const char**])]
	)

OWNTONE_MODULES_CHECK([OWNTONE], [JSON_C], [json-c],
	[json_tokener_parse], [json.h],
	[dnl check for old version
	 PKG_CHECK_EXISTS([json-c >= 0.11], [],
		[AC_DEFINE([HAVE_JSON_C_OLD], 1,
			[Define to 1 if you have json-c < 0.11])])
	])

# Build with libplist (2.2.0 does not ship libplist.pc, only libplist-2.0.pc)
PKG_CHECK_EXISTS([libplist],
	[OWNTONE_MODULES_CHECK([OWNTONE], [LIBPLIST], [libplist >= 0.16],
		[plist_dict_get_item], [plist/plist.h])],
	[OWNTONE_MODULES_CHECK([OWNTONE], [LIBPLIST], [libplist-2.0],
		[plist_dict_get_item], [plist/plist.h])])

AM_PATH_LIBGCRYPT([1:1.7.0])
OWNTONE_FUNC_REQUIRE([OWNTONE], [GNU Crypt Library], [LIBGCRYPT], [gcrypt],
	[gcry_control], [gcrypt.h])
AM_PATH_GPG_ERROR([1.6])
OWNTONE_FUNC_REQUIRE([OWNTONE], [GNUPG Error Values], [GPG_ERROR_MT], [gpg-error],
	[gpg_err_init], [gpg-error.h])


AC_CHECK_HEADER([sys/eventfd.h], [AC_CHECK_FUNCS([eventfd])])

AC_CHECK_HEADER([sys/timerfd.h], [AC_CHECK_FUNC([timerfd_create],
	[AC_DEFINE([HAVE_TIMERFD], 1, [Define to 1 if you have timerfd])])])

AS_IF([test "`uname -s`" = "Darwin"],
	[AC_MSG_NOTICE([Skipping inotify checks on macOS])],
	[OWNTONE_FUNC_REQUIRE([OWNTONE], [inotify], [INOTIFY], [inotify],
		[inotify_add_watch], [sys/inotify.h])])

have_signal=no
AC_CHECK_HEADER([sys/signalfd.h], [AC_CHECK_FUNCS([signalfd], [have_signal=yes])])
AC_CHECK_HEADER([sys/event.h], [AC_CHECK_FUNCS([kqueue], [have_signal=yes])])
AS_IF([[test "$have_signal" = "no"]],
	[AC_MSG_ERROR([[Either signalfd or kqueue are required]])])

AC_CHECK_HEADERS_ONCE([endian.h sys/endian.h])
AC_CHECK_DECL([htobe16], [],
	[AC_CHECK_HEADERS([libkern/OSByteOrder.h], [],
		[AC_MSG_ERROR([[Missing functions to swap byte order]])])],
	[AC_INCLUDES_DEFAULT[
#ifdef HAVE_ENDIAN_H
# include <endian.h>
#elif defined(HAVE_SYS_ENDIAN_H)
# include <sys/endian.h>
#endif
	]])

# libav checks should be last, as they are sometimes both installed
# and the CPPFLAGS/LIBS needs to be at the front of the search list.
# Handle alternative package names for libav
PKG_CHECK_EXISTS([libavcodec-libav], [LIBAV=-libav], [LIBAV=])
# Preference for ffmpeg if we have both (this could be an option...)
PKG_CHECK_EXISTS([libavcodec], [LIBAV=])
# Option to choose libav even if ffmpeg is detected first
AC_ARG_WITH([libav], [AS_HELP_STRING([--with-libav],
	[choose libav even if ffmpeg present (default=no)])],
	[[LIBAV=-libav]], [[LIBAV=]])
# libav/ffmpeg requires many feature checks
OWNTONE_MODULES_CHECK([OWNTONE], [LIBAV],
	[libavformat$LIBAV libavcodec$LIBAV libavutil$LIBAV libavfilter$LIBAV],
	[av_packet_alloc], [libavcodec/avcodec.h],
	[# Checks for misc libav and ffmpeg API differences
	 AC_MSG_CHECKING([whether libav libraries are ffmpeg])
	 AC_LINK_IFELSE([AC_LANG_PROGRAM([[#include <libavcodec/avcodec.h>
	 ]], [[
#if LIBAVCODEC_VERSION_MICRO >= 100
  /* ffmpeg uses 100+ as its micro version */
#else
#error libav provider is not ffmpeg
#endif
	 ]])], [
	  [is_ffmpeg=yes]
	  AC_DEFINE([HAVE_FFMPEG], 1,
		[Define to 1 if you have ffmpeg (not libav)])],
	 [[is_ffmpeg=no]])
	 AC_MSG_RESULT([$is_ffmpeg])
	 OWNTONE_CHECK_DECLS([av_version_info],
		[libavutil/avutil.h])
	 OWNTONE_CHECK_DECLS([avformat_network_init],
		[libavformat/avformat.h])
	 OWNTONE_CHECK_DECLS([av_dict_iterate],
		[libavutil/dict.h])
	])

AC_CHECK_SIZEOF([void *])

dnl --- Begin configuring the options ---
dnl Install config file
OWNTONE_ARG_DISABLE([install configuration file], [install_conf_file], [INSTALL_CONF_FILE])
AM_CONDITIONAL([COND_INSTALL_CONF_FILE], [[test "x$enable_install_conf_file" = "xyes"]])

dnl --- End options ---

AC_CONFIG_SUBDIRS([src])

AC_CONFIG_FILES([
    src/Makefile
    Makefile
])
AC_OUTPUT
