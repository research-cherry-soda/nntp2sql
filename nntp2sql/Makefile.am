# Makefile.am for nntp2sql (beta .3)

bin_PROGRAMS = nntp2sql
nntp2sql_SOURCES = main.c \
	src/logging.c src/logging.h \
	src/nntp.c src/nntp.h \
	src/db.c src/db.h

# Compiler/linker flags from configure
AM_CFLAGS = @OPENSSL_CFLAGS@ @SQLITE3_CFLAGS@ @MARIADB_CFLAGS@
AM_LDFLAGS = @OPENSSL_LIBS@ @SQLITE3_LIBS@ @MARIADB_LIBS@

# Pthreads added if available
if WITH_PTHREAD
AM_LDFLAGS += -lpthread
endif

# Viewer (GTK3) optional
if WITH_GTK3
bin_PROGRAMS += viewer
viewer_SOURCES = gui/viewer.c
viewer_CPPFLAGS = @GTK3_CFLAGS@
viewer_LDADD = @GTK3_LIBS@ -lsqlite3
endif

# Man page installation
dist_man_MANS = man/nntp2sql.1

# Extra distribution files
EXTRA_DIST = README README.md configure.ac src/logging.h src/nntp.h src/db.h gui/viewer.c
