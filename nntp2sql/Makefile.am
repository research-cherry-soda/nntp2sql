SUBDIRS = .

bin_PROGRAMS = nntp2sql
if BUILD_VIEWER
bin_PROGRAMS += isotope-viewer
endif

nntp2sql_SOURCES = main.c src/export_html.c
nntp2sql_LDADD = @OPENSSL_LIBS@ @SQLITE3_LIBS@ @MYSQL_LIBS@ @PQ_LIBS@
nntp2sql_CFLAGS = @OPENSSL_CFLAGS@ @SQLITE3_CFLAGS@ @MYSQL_CFLAGS@

# Ensure headers in src/ are found
AM_CPPFLAGS = -Isrc

isotope_viewer_SOURCES = gui/viewer.c
isotope_viewer_LDADD = @SQLITE3_LIBS@ @GTK_LIBS@ @MYSQL_LIBS@ @PQ_LIBS@
isotope_viewer_CFLAGS = @SQLITE3_CFLAGS@ @GTK_CFLAGS@ @MYSQL_CFLAGS@

man_MANS = man/nntp2sql.1
dist_man_MANS = man/nntp2sql.1
EXTRA_DIST = README README.md configure.ac man/nntp2sql.1 src/export_html.c src/db.h gui/viewer.c
