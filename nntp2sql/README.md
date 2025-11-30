# nntp2sql (beta 0.50)

NNTP → SQL ingestor with HTML export and a GTK4 viewer.

Features
- Connects to NNTP (SSL/STARTTLS, optional AUTH)
- Writes to SQLite, MySQL/MariaDB, and optionally PostgreSQL (via libpq)
- Update-first writes with optional `--upsert` fallback insert
- Unique index `(group_name, artnum)` to avoid duplicates
- Multithreaded HEAD fetching with progress bars
- Config save/load (`--conf`, `--write-conf`)
- DB init (`--init-db` / `--create-db`)
- HTML export: `--export-html` for a single group or `--group-list` to export many
- GTK4 viewer with search; supports SQLite/MySQL/PostgreSQL

## Requirements

- Tooling: Autotools (`autoconf`, `automake`, `libtool`), `pkg-config`, a C11 compiler.
- Core libraries:
	- `openssl` (SSL/STARTTLS),
	- `sqlite3`,
	- `mysqlclient` (MariaDB/MySQL),
	- optional `libpq` (PostgreSQL). Build enables PG when detected.
- Viewer libraries (GTK4):
	- `gtk4`, `glib2`, `pango`, `harfbuzz`, `gdk-pixbuf2`, `cairo`, `graphene`, plus typical font/image deps (`freetype`, `fontconfig`, `libpng`, `jpeg`, `tiff`, `xz`, `zstd`).

macOS (Homebrew)
```sh
brew install autoconf automake libtool pkg-config \
						 openssl@3 sqlite mysql gtk4 pango harfbuzz gdk-pixbuf cairo graphene
```

Debian/Ubuntu
```sh
sudo apt-get install -y build-essential autoconf automake libtool pkg-config \
		libssl-dev libsqlite3-dev libmysqlclient-dev libpq-dev \
		libgtk-4-dev libglib2.0-dev libpango1.0-dev libharfbuzz-dev \
		libgdk-pixbuf-2.0-dev libcairo2-dev libgraphene-1.0-dev
```

Fedora
```sh
sudo dnf install -y gcc gcc-c++ autoconf automake libtool pkgconf-pkg-config \
		openssl-devel sqlite-devel mysql-devel postgresql-devel \
		gtk4-devel glib2-devel pango-devel harfbuzz-devel \
		gdk-pixbuf2-devel cairo-devel graphene-devel
```

FreeBSD
```sh
pkg install autoconf automake libtool pkgconf \
		openssl sqlite3 mysql80-client postgresql15-client \
		gtk4 glib pango harfbuzz gdk-pixbuf2 cairo graphene
```

## Build (Autotools)
```sh
autoreconf -fi
./configure
make -j
sudo make install
```

- Viewer is enabled by default; disable with `./configure --disable-viewer`.
- PostgreSQL support is optional and built when `libpq` is found.

## Usage (ingest)
```sh
./nntp2sql --host news.example --db-type sqlite --db-name data.db --group comp.lang.c --headers-only
```

## HTML export
```sh
# single group
./nntp2sql --db-type sqlite --db-name data.db --export-html --group comp.lang.c --export-html-out comp.lang.c.html
# list of groups
./nntp2sql --db-type sqlite --db-name data.db --export-html --group-list groups.txt --export-html-out ./exports
```

## Viewer (`isotope-viewer`)
```sh
# SQLite
./isotope-viewer --db-type sqlite data.db
# MySQL
./isotope-viewer --db-type mysql localhost,mydb,user,pass,3306
# PostgreSQL (when libpq available at build time)
./isotope-viewer --db-type postgres "host=localhost dbname=mydb user=user password=pass"
```

Notes
- Binaries: CLI `nntp2sql` and GUI `isotope-viewer` (installed names).
- Ensure OpenSSL, sqlite3, mysqlclient, optional libpq, and GTK4 dev packages are installed before building.
- Man page `nntp2sql(1)` installs with `make install`.
