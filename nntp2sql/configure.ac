# configure.ac for nntp2sql (beta .3)
AC_PREREQ([2.69])
AC_INIT([nntp2sql], [0.3], [you@example.com])
AC_CONFIG_SRCDIR([main.c])
AC_CONFIG_HEADERS([config.h])
AM_INIT_AUTOMAKE([foreign subdir-objects])

# Checks for programs.
AC_PROG_CC
AC_PROG_INSTALL

# Default C flags
if test -z "$CFLAGS"; then
  CFLAGS="-O2 -Wall -Wextra -std=c99"
fi

# Checks for libraries via pkg-config
PKG_CHECK_MODULES([OPENSSL], [openssl], [HAVE_OPENSSL=yes], [HAVE_OPENSSL=no])
PKG_CHECK_MODULES([SQLITE3], [sqlite3], [HAVE_SQLITE3=yes], [HAVE_SQLITE3=no])
PKG_CHECK_MODULES([GTK3], [gtk+-3.0], [HAVE_GTK3=yes], [HAVE_GTK3=no])

# MySQL client: try pkg-config (libmariadb), then fallback to manual check
PKG_CHECK_MODULES([MARIADB], [libmariadb], [HAVE_MARIADB=yes], [HAVE_MARIADB=no])
if test "$HAVE_MARIADB" = "no"; then
  AC_CHECK_HEADERS([mysql/mysql.h], [HAVE_MYSQL_H=yes], [HAVE_MYSQL_H=no])
  AC_CHECK_LIB([mysqlclient], [mysql_init], [HAVE_MYSQLCLIENT=yes], [HAVE_MYSQLCLIENT=no])
fi

# Pthreads
AC_CHECK_LIB([pthread], [pthread_create], [HAVE_PTHREAD=yes], [HAVE_PTHREAD=no])

# Summaries
AM_CONDITIONAL([WITH_OPENSSL], [test "$HAVE_OPENSSL" = "yes"])
AM_CONDITIONAL([WITH_SQLITE3], [test "$HAVE_SQLITE3" = "yes"])
AM_CONDITIONAL([WITH_GTK3], [test "$HAVE_GTK3" = "yes"])
AM_CONDITIONAL([WITH_MARIADB], [test "$HAVE_MARIADB" = "yes" -o "$HAVE_MYSQLCLIENT" = "yes"])
AM_CONDITIONAL([WITH_PTHREAD], [test "$HAVE_PTHREAD" = "yes"])

# Define feature macros
if test "$HAVE_OPENSSL" = "yes"; then
  AC_DEFINE([HAVE_OPENSSL], [1], [Define if OpenSSL is available])
fi
if test "$HAVE_SQLITE3" = "yes"; then
  AC_DEFINE([HAVE_SQLITE3], [1], [Define if SQLite3 is available])
fi
if test "$HAVE_GTK3" = "yes"; then
  AC_DEFINE([HAVE_GTK3], [1], [Define if GTK3 is available])
fi
if test "$HAVE_MARIADB" = "yes" -o "$HAVE_MYSQLCLIENT" = "yes"; then
  AC_DEFINE([HAVE_MYSQLCLIENT], [1], [Define if MySQL/MariaDB client is available])
fi
if test "$HAVE_PTHREAD" = "yes"; then
  AC_DEFINE([HAVE_PTHREAD], [1], [Define if pthreads is available])
fi

# Export flags
if test "$HAVE_OPENSSL" = "yes"; then
  OPENSSL_CFLAGS="$OPENSSL_CFLAGS"
  OPENSSL_LIBS="$OPENSSL_LIBS"
fi
if test "$HAVE_SQLITE3" = "yes"; then
  SQLITE3_CFLAGS="$SQLITE3_CFLAGS"
  SQLITE3_LIBS="$SQLITE3_LIBS"
fi
if test "$HAVE_GTK3" = "yes"; then
  GTK3_CFLAGS="$GTK3_CFLAGS"
  GTK3_LIBS="$GTK3_LIBS"
fi
if test "$HAVE_MARIADB" = "yes"; then
  MARIADB_CFLAGS="$MARIADB_CFLAGS"
  MARIADB_LIBS="$MARIADB_LIBS"
fi

AC_SUBST([OPENSSL_CFLAGS])
AC_SUBST([OPENSSL_LIBS])
AC_SUBST([SQLITE3_CFLAGS])
AC_SUBST([SQLITE3_LIBS])
AC_SUBST([GTK3_CFLAGS])
AC_SUBST([GTK3_LIBS])
AC_SUBST([MARIADB_CFLAGS])
AC_SUBST([MARIADB_LIBS])

# Outputs
AC_CONFIG_FILES([
  Makefile
])
AC_OUTPUT
