SUBDIRS = .

bin_PROGRAMS = nntp2sql viewer

nntp2sql_SOURCES = main.c src/export_html.c
nntp2sql_LDADD = @OPENSSL_LIBS@ @SQLITE3_LIBS@ @MYSQL_LIBS@ @PQ_LIBS@
nntp2sql_CFLAGS = @OPENSSL_CFLAGS@ @SQLITE3_CFLAGS@ @MYSQL_CFLAGS@

viewer_SOURCES = gui/viewer.c
viewer_LDADD = @SQLITE3_LIBS@ @GTK_LIBS@ @MYSQL_LIBS@ @PQ_LIBS@
viewer_CFLAGS = @SQLITE3_CFLAGS@ @GTK_CFLAGS@ @MYSQL_CFLAGS@

man_MANS = man/nntp2sql.1
dist_man_MANS = man/nntp2sql.1
EXTRA_DIST = README README.md configure.ac man/nntp2sql.1 src/export_html.c gui/viewer.c
