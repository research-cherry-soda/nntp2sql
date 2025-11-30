# nntp2sql (beta 0.42)

nntp2sql is a compact NNTP → SQL ingestor supporting SSL/STARTTLS, optional AUTH, group selection, and storing headers either via XOVER or full HEAD requests. It supports SQLite and MariaDB/MySQL backends and includes multithreaded HEAD fetching, retries, progress bars, config save/load, and structured error logging.

## Features
- SSL/TLS via `--ssl` or STARTTLS via `--starttls`
- Authentication via `AUTHINFO USER/PASS`
- Headers-only via `XOVER` or full headers via `HEAD`
- Databases: SQLite and MariaDB/MySQL
- Schema initialization via `--init-db` and `--create-db`
- Progress bars with configurable width (`--progress-width`)
- Config load/save (`--conf`, `--write-conf`)
- Multithreaded HEAD workers (`--threads`), with retries (`--retries`)
- Update-only DB writes; optional upsert (`--upsert`)
- Structured errors with codes and optional logging (`--log`, `--verbose`)

## Build
On macOS, install dependencies:

```zsh
# SQLite and OpenSSL are typically present; install MySQL/MariaDB client headers
brew install mariadb-connector-c
# Or, alternatively:
brew install mysql-client
# If using mysql-client, you may need:
echo 'export PATH="/opt/homebrew/opt/mysql-client/bin:$PATH"' >> ~/.zshrc
export CPPFLAGS="-I/opt/homebrew/opt/mysql-client/include"
export LDFLAGS="-L/opt/homebrew/opt/mysql-client/lib"
```

Compile:

```zsh
cd Isotope
gcc -o nntp2sql main.c -lssl -lcrypto -lsqlite3 -lmysqlclient -lpthread
```

## Usage
Basic:

```zsh
./nntp2sql --host news.example.net --group comp.lang.c \
  --db-type sqlite --db-name headers.db --headers-only
```

StartTLS and auth:

```zsh
./nntp2sql --host news.example.net --starttls --user alice --pass secret \
  --db-type mariadb --db-name newsdb --db-host 127.0.0.1 --db-user root --db-pass pwd \
  --group comp.lang.c --headers-only
```

Create DB/schema then exit:

```zsh
./nntp2sql --db-type mariadb --db-name newsdb --db-host 127.0.0.1 \
  --init-db --create-db
```

Multithreaded HEAD with retries:

```zsh
./nntp2sql --host news.example.net --group comp.lang.c \
  --db-type sqlite --db-name headers.db --threads 8 --retries 3
```

Update-only vs upsert:

```zsh
# Update existing rows; if none updated, log a warning
./nntp2sql ... --headers-only

# Upsert: update if exists, else insert
./nntp2sql ... --headers-only --upsert
```

Progress bar width:

```zsh
./nntp2sql ... --progress-width 60
```

Config management:

```zsh
# Save current CLI config
./nntp2sql ... --write-conf my.conf

# Load config (CLI args override loaded values)
./nntp2sql --conf my.conf ...
```

Logging and verbosity:

```zsh
./nntp2sql ... --log app.log --verbose
```

## Command-Line Options
- `--host HOST`: NNTP server (default `localhost`).
- `--port PORT`: NNTP port (defaults to `119`, or `563` when `--ssl`).
- `--ssl`: Use TLS immediately on connect.
- `--starttls`: Negotiate STARTTLS after greeting.
- `--user USER --pass PASS`: NNTP authentication.
- `--group NAME`: Group name to ingest.
- `--headers-only`: Use `XOVER` instead of `HEAD` workers.
- `--limit N`: Limit to last N articles in the selected range.
- `--progress-width N`: Progress bar width (default 40).
- `--init-db`: Create database (for MySQL) before connecting to it.
- `--create-db`: Create DB/schema and exit.
- `--db-type {sqlite|mariadb|mysql}`: Backend type.
- `--db-name DBNAME`: SQLite file path or MySQL database name.
- `--db-host HOST --db-port PORT --db-user USER --db-pass PASS`: MySQL connection.
- `--threads N`: HEAD worker threads (1–64; default 1).
- `--retries N`: HEAD retry attempts (0–10; default 3).
- `--upsert`: Update-first; insert if no row updated.
- `--conf FILE`: Load configuration file (key=value).
- `--write-conf FILE`: Write configuration and exit.
- `--log FILE`: Append logs to a file.
- `--verbose`: Print INFO logs to stderr.

## Database Schema
Tables:
- `groups`: `(id, name UNIQUE, article_count, first, last)`
- `articles`: `(id, artnum, subject, author, date, message_id, refs, bytes, line_count, group_name)`

Constraints:
- Unique index on `articles(group_name, artnum)` to prevent duplicates and enable robust upserts.

## Error Handling
The program exits with structured error codes. Examples:
- `ERR_ARGS` (2): Invalid/missing arguments
- `ERR_NET_CONNECT` (11): Network connect failed
- `ERR_TLS` (12): TLS/SSL errors
- `ERR_NNTP_CMD` (14): NNTP command failed
- `ERR_DB_CONNECT` (20): DB connection failed
- `ERR_DB_SCHEMA` (21): Schema creation failed
- `ERR_DB_PREPARE` (22): Prepared statement failed

## Man Page
See `man/nntp2sql.1` for a reference man page.

## License
Proprietary or as per your project needs.

## Version
beta 0.42
