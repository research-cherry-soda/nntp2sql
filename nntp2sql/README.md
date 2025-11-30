# nntp2sql

NNTP → SQL ingestor with HTML export and a GTK viewer.

Features
- Connects to NNTP (SSL/STARTTLS, optional AUTH)
- Writes to SQLite, MySQL/MariaDB, or PostgreSQL (via libpq)
- Update-first writes with optional `--upsert` fallback insert
- Unique index `(group_name, artnum)` to avoid duplicates
- Multithreaded HEAD fetching with progress bars
- Config save/load (`--conf`, `--write-conf`)
- DB init (`--init-db` / `--create-db`)
- HTML export: `--export-html` for a single group or `--group-list` to export many
- GTK viewer with search; supports SQLite/MySQL/PostgreSQL

Build (Autotools)
```sh
autoreconf -fi
./configure --with-postgres
make -j
sudo make install
```

Usage (ingest)
```sh
./nntp2sql --host news.example --db-type sqlite --db-name data.db --group comp.lang.c --headers-only
```

HTML export
```sh
# single group
./nntp2sql --db-type sqlite --db-name data.db --export-html --group comp.lang.c --export-html-out comp.lang.c.html
# list of groups
./nntp2sql --db-type sqlite --db-name data.db --export-html --group-list groups.txt --export-html-out ./exports
```

Viewer
```sh
# SQLite
./viewer --db-type sqlite data.db
# MySQL
./viewer --db-type mysql localhost,mydb,user,pass,3306
# PostgreSQL
./viewer --db-type postgres "host=localhost dbname=mydb user=user password=pass"
```

Notes
- Program name is `nntp2sql`. All prior references to Isotope browser were removed.
- Ensure OpenSSL, sqlite3, mysqlclient, libpq, and GTK4 dev packages are installed.
- Man page `nntp2sql(1)` installs with `make install` and appears under your system's man directory (e.g., `/usr/local/share/man/man1`).
	- View it with: `man nntp2sql`
