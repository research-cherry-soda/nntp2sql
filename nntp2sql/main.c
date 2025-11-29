/*
 * main.c
 *
 * Simple NNTP -> SQL dumper supporting SSL/STARTTLS, AUTH, group selection,
 * headers-only (via XOVER) or full-headers (via HEAD/ARTICLE), and backends:
 *   sqlite3, mariadb/mysql
 *
 * Build (example):
 *   gcc -o nntp2sql main.c -lssl -lcrypto -lsqlite3 -lmysqlclient
 *
 * Note: link only the DB libs you need. Adjust include/library paths as required.
 *
 * Usage:
 *   nntp2sql --host HOST [--port PORT] [--ssl] [--starttls]
 *            [--user USER --pass PASS]
 *            --db-type {sqlite|mariadb|mysql} --db-name DBNAME
 *            [--db-host HOST --db-port PORT --db-user USER --db-pass PASS]
 *            --group GROUPNAME [--headers-only] [--limit N]
 *            [--progress-width N] [--init-db]
 *            [--conf FILE] [--write-conf FILE]
 *
 * This is a compact example for demonstration. Production code should add more
 * robust error handling, retries, better SQL escaping/prepared statements,
 * and concurrency considerations.
 */

#define _POSIX_C_SOURCE 200112L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <netdb.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <fcntl.h>
#include <time.h>

/* Optional DB client headers */
#include <sqlite3.h>
#include <mysql/mysql.h>  /* MariaDB/MySQL */
#include <pthread.h>

#include <openssl/ssl.h>
#include <openssl/err.h>

#define BUFSZ 8192

/* Simple logging */
typedef enum {
    ERR_OK = 0,
    ERR_ARGS = 2,
    ERR_CONFIG = 3,
    ERR_NET_DNS = 10,
    ERR_NET_CONNECT = 11,
    ERR_TLS = 12,
    ERR_NNTP_GREETING = 13,
    ERR_NNTP_CMD = 14,
    ERR_AUTH = 15,
    ERR_DB_CONNECT = 20,
    ERR_DB_SCHEMA = 21,
    ERR_DB_PREPARE = 22,
    ERR_RUNTIME = 30
} AppError;

static FILE *g_log = NULL;
static int g_verbose = 0;
static int g_upsert = 0; /* when enabled: update-then-insert if missing */

static const char *describe_error(AppError e) {
    switch (e) {
        case ERR_OK: return "ok";
        case ERR_ARGS: return "invalid or missing arguments";
        case ERR_CONFIG: return "configuration error";
        case ERR_NET_DNS: return "DNS resolution failed";
        case ERR_NET_CONNECT: return "network connect failed";
        case ERR_TLS: return "TLS/SSL error";
        case ERR_NNTP_GREETING: return "NNTP greeting failed";
        case ERR_NNTP_CMD: return "NNTP command failed";
        case ERR_AUTH: return "authentication failed";
        case ERR_DB_CONNECT: return "database connection failed";
        case ERR_DB_SCHEMA: return "database schema creation failed";
        case ERR_DB_PREPARE: return "database prepared statement failed";
        default: return "runtime error";
    }
}

static void log_open(const char *path) {
    if (path && *path) {
        g_log = fopen(path, "a");
        if (!g_log) {
            fprintf(stderr, "Could not open log file %s: %s\n", path, strerror(errno));
        }
    }
}

static void log_close(void) {
    if (g_log) { fclose(g_log); g_log = NULL; }
}

static void log_msg(const char *level, const char *fmt, va_list ap) {
    char tbuf[64];
    time_t now = time(NULL);
    struct tm tm; localtime_r(&now, &tm);
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &tm);
    FILE *out = g_log ? g_log : stderr;
    fprintf(out, "%s [%s] ", tbuf, level);
    vfprintf(out, fmt, ap);
    fprintf(out, "\n");
    fflush(out);
}

static void infof(const char *fmt, ...) {
    if (!g_verbose) return;
    va_list ap; va_start(ap, fmt); log_msg("INFO", fmt, ap); va_end(ap);
}

static void warnf(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); log_msg("WARN", fmt, ap); va_end(ap);
}

static void fatal(AppError code, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); log_msg("ERROR", fmt, ap); va_end(ap);
    fprintf(stderr, "Error (code %d): %s\n", code, describe_error(code));
    log_close();
    exit(code);
}

/* Network/SSL wrapper */

typedef struct {
    int sock;
    SSL_CTX *ctx;
    SSL *ssl;
    int use_ssl;
} Conn;

static int tcp_connect(const char *host, const char *port) {
    struct addrinfo hints, *res, *rp;
    int s, fd = -1;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    if ((s = getaddrinfo(host, port, &hints, &res)) != 0) {
        warnf("getaddrinfo failed for %s:%s: %s", host, port, gai_strerror(s));
        return -1;
    }
    for (rp = res; rp != NULL; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd == -1) continue;
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd == -1) {
        warnf("Unable to connect to %s:%s: %s", host, port, strerror(errno));
        return -1;
    }
    return fd;
}

static void conn_init(Conn *c) {
    memset(c, 0, sizeof(*c));
    c->sock = -1;
}

static void conn_cleanup(Conn *c) {
    if (!c) return;
    if (c->ssl) { SSL_shutdown(c->ssl); SSL_free(c->ssl); c->ssl = NULL; }
    if (c->ctx) { SSL_CTX_free(c->ctx); c->ctx = NULL; }
    if (c->sock >= 0) close(c->sock);
}

/* read one line (CRLF terminated) into buf (null-terminated) */
static ssize_t conn_readline(Conn *c, char *buf, size_t bufsz) {
    size_t pos = 0;
    while (pos + 1 < bufsz) {
        char ch;
        ssize_t r = c->use_ssl ? SSL_read(c->ssl, &ch, 1) : recv(c->sock, &ch, 1, 0);
        if (r <= 0) return -1;
        buf[pos++] = ch;
        if (pos >= 2 && buf[pos-2] == '\r' && buf[pos-1] == '\n') break;
    }
    buf[pos] = '\0';
    return (ssize_t)pos;
}

/* read multiline dot-terminated response; caller must free returned char*; lines are '\n' separated */
static char *conn_read_multiline(Conn *c) {
    size_t cap = 8192, len = 0;
    char *out = malloc(cap);
    if (!out) return NULL;
    out[0] = '\0';
    char line[BUFSZ];
    while (1) {
        if (conn_readline(c, line, sizeof(line)) <= 0) { free(out); return NULL; }
        /* strip CRLF */
        size_t l = strlen(line);
        while (l && (line[l-1] == '\r' || line[l-1] == '\n')) line[--l] = '\0';
        if (strcmp(line, ".") == 0) break;
        /* handle transparent dot-stuffing */
        if (line[0] == '.' ) memmove(line, line+1, strlen(line));
        size_t need = len + l + 2;
        if (need > cap) {
            cap = need * 2;
            out = realloc(out, cap);
            if (!out) return NULL;
        }
        memcpy(out + len, line, l);
        len += l;
        out[len++] = '\n';
        out[len] = '\0';
    }
    return out;
}

static ssize_t conn_sendf(Conn *c, const char *fmt, ...) {
    char buf[BUFSZ];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    size_t len = strlen(buf);
    /* ensure CRLF at end */
    if (len < 2 || buf[len-2] != '\r' || buf[len-1] != '\n') {
        if (len + 2 >= sizeof(buf)) return -1;
        buf[len++] = '\r'; buf[len++] = '\n'; buf[len] = '\0';
    }
    if (c->use_ssl) {
        int r = SSL_write(c->ssl, buf, (int)len);
        return r;
    } else {
        return send(c->sock, buf, len, 0);
    }
}

/* initialize SSL for client */
static void ssl_init(void) {
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();
}

/* start tls on existing conn */
static int conn_starttls(Conn *c) {
    if (c->use_ssl) return 1;
    ssl_init();
    c->ctx = SSL_CTX_new(TLS_client_method());
    if (!c->ctx) return 0;
    c->ssl = SSL_new(c->ctx);
    if (!c->ssl) return 0;
    if (!SSL_set_fd(c->ssl, c->sock)) return 0;
    if (SSL_connect(c->ssl) <= 0) {
        ERR_print_errors_fp(stderr);
        return 0;
    }
    c->use_ssl = 1;
    return 1;
}

/* DB abstraction */

typedef enum { DB_SQLITE, DB_MYSQL } DBType;

typedef struct {
    DBType type;
    sqlite3 *sqlite;
    MYSQL *mysql;
    /* prepared statements */
    sqlite3_stmt *sqlite_insert_article;
    sqlite3_stmt *sqlite_insert_group;
    MYSQL_STMT *mysql_insert_article;
    /* additional prepared statements for upsert */
    sqlite3_stmt *sqlite_article_ins;
    sqlite3_stmt *sqlite_group_ins;
    MYSQL_STMT *mysql_article_ins;
} DB;

static void db_close(DB *db) {
    if (!db) return;
    if (db->type == DB_SQLITE) {
        if (db->sqlite_insert_article) sqlite3_finalize(db->sqlite_insert_article);
        if (db->sqlite_insert_group) sqlite3_finalize(db->sqlite_insert_group);
        if (db->sqlite_article_ins) sqlite3_finalize(db->sqlite_article_ins);
        if (db->sqlite_group_ins) sqlite3_finalize(db->sqlite_group_ins);
    } else if (db->type == DB_MYSQL) {
        if (db->mysql_insert_article) mysql_stmt_close(db->mysql_insert_article);
        if (db->mysql_article_ins) mysql_stmt_close(db->mysql_article_ins);
    }
    if (db->sqlite) sqlite3_close(db->sqlite);
    if (db->mysql) mysql_close(db->mysql);
    /* no postgres */
    memset(db, 0, sizeof(*db));
}

static void db_exec(DB *db, const char *sql) {
    if (db->type == DB_SQLITE) {
        char *err = NULL;
        if (sqlite3_exec(db->sqlite, sql, NULL, NULL, &err) != SQLITE_OK) {
            warnf("sqlite exec error: %s", err ? err : "unknown");
            if (err) sqlite3_free(err);
        }
    } else if (db->type == DB_MYSQL) {
        if (mysql_query(db->mysql, sql)) {
            warnf("mysql exec error: %s", mysql_error(db->mysql));
        }
    }
}

/* Basic escaping for SQL insertion; for production use prepared statements. */
static char *db_escape(DB *db, const char *s) {
    if (!s) return strdup("");
    if (db->type == DB_SQLITE) {
        /* simple safe allocator: replace single quote with two single quotes */
        size_t n = strlen(s);
        size_t cap = n * 2 + 3;
        char *out = malloc(cap);
        char *p = out;
        *p++ = '\'';
        for (; *s; s++) {
            if (*s == '\'') { *p++ = '\''; *p++ = '\''; }
            else *p++ = *s;
        }
        *p++ = '\'';
        *p++ = '\0';
        return out;
    } else if (db->type == DB_MYSQL) {
        unsigned long len = (unsigned long)strlen(s);
        char *buf = malloc(len*2 + 3);
        mysql_real_escape_string(db->mysql, buf, s, len);
        /* wrap in quotes */
        char *out = malloc(strlen(buf) + 3);
        sprintf(out, "'%s'", buf);
        free(buf);
        return out;
    }
    return strdup("''");
}

/* Create tables */
static void db_init_schema(DB *db) {
    /* Note: MySQL reserves GROUPS, REFERENCES, LINES. Avoid reserved words: use article_count, refs, line_count. Quote MySQL identifiers. */
    if (db->type == DB_SQLITE) {
        db_exec(db,
            "CREATE TABLE IF NOT EXISTS groups (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT UNIQUE, article_count INTEGER, first INTEGER, last INTEGER);"
        );
        db_exec(db,
            "CREATE TABLE IF NOT EXISTS articles (id INTEGER PRIMARY KEY AUTOINCREMENT, artnum INTEGER, subject TEXT, author TEXT, date TEXT, message_id TEXT, refs TEXT, bytes INTEGER, line_count INTEGER, group_name TEXT);"
        );
        db_exec(db,
            "CREATE UNIQUE INDEX IF NOT EXISTS idx_articles_group_artnum ON articles(group_name, artnum);"
        );
        if (sqlite3_prepare_v2(db->sqlite,
            "UPDATE articles SET subject=?, author=?, date=?, message_id=?, refs=?, bytes=?, line_count=? WHERE group_name=? AND artnum=?",
            -1, &db->sqlite_insert_article, NULL) != SQLITE_OK) {
            warnf("sqlite prepare article-update failed: %s", sqlite3_errmsg(db->sqlite));
        }
        if (sqlite3_prepare_v2(db->sqlite,
            "INSERT INTO articles (artnum, subject, author, date, message_id, refs, bytes, line_count, group_name) VALUES (?,?,?,?,?,?,?,?,?)",
            -1, &db->sqlite_article_ins, NULL) != SQLITE_OK) {
            warnf("sqlite prepare article-insert failed: %s", sqlite3_errmsg(db->sqlite));
        }
        if (sqlite3_prepare_v2(db->sqlite,
            "UPDATE groups SET article_count=?, first=?, last=? WHERE name=?",
            -1, &db->sqlite_insert_group, NULL) != SQLITE_OK) {
            /* fallback without quoted parameter name */
        }
        if (!db->sqlite_insert_group) {
            if (sqlite3_prepare_v2(db->sqlite,
                "UPDATE groups SET article_count=?, first=?, last=? WHERE name=?",
                -1, &db->sqlite_insert_group, NULL) != SQLITE_OK) {
                warnf("sqlite prepare group-update failed: %s", sqlite3_errmsg(db->sqlite));
            }
        }
        if (sqlite3_prepare_v2(db->sqlite,
            "INSERT INTO groups (name, article_count, first, last) VALUES (?,?,?,?)",
            -1, &db->sqlite_group_ins, NULL) != SQLITE_OK) {
            warnf("sqlite prepare group-insert failed: %s", sqlite3_errmsg(db->sqlite));
        }
    } else if (db->type == DB_MYSQL) {
        if (mysql_query(db->mysql, "CREATE TABLE IF NOT EXISTS `groups` (id INT AUTO_INCREMENT PRIMARY KEY, name VARCHAR(255) UNIQUE, article_count INT, first INT, last INT) ENGINE=InnoDB;")) {
            fatal(ERR_DB_SCHEMA, "mysql schema error (groups): %s", mysql_error(db->mysql));
        }
        /* include unique key in table create for fresh databases */
        if (mysql_query(db->mysql, "CREATE TABLE IF NOT EXISTS `articles` (id INT AUTO_INCREMENT PRIMARY KEY, `artnum` INT, `subject` TEXT, `author` TEXT, `date` TEXT, `message_id` TEXT, `refs` TEXT, `bytes` INT, `line_count` INT, `group_name` VARCHAR(255), UNIQUE KEY `idx_articles_group_artnum` (`group_name`,`artnum`)) ENGINE=InnoDB;")) {
            fatal(ERR_DB_SCHEMA, "mysql schema error (articles): %s", mysql_error(db->mysql));
        }
        /* for existing tables, try to add the unique key; ignore error if it exists */
        if (mysql_query(db->mysql, "ALTER TABLE `articles` ADD UNIQUE KEY `idx_articles_group_artnum` (`group_name`,`artnum`);")) {
            /* duplicate key name or existing index -> non-fatal */
            const char *err = mysql_error(db->mysql);
            if (err && *err) {
                infof("mysql index add note: %s", err);
            }
        }
        db->mysql_insert_article = mysql_stmt_init(db->mysql);
        if (!db->mysql_insert_article) warnf("mysql_stmt_init failed for article");
        const char *art_sql = "UPDATE `articles` SET `subject`=?, `author`=?, `date`=?, `message_id`=?, `refs`=?, `bytes`=?, `line_count`=? WHERE `group_name`=? AND `artnum`=?";
        if (mysql_stmt_prepare(db->mysql_insert_article, art_sql, (unsigned long)strlen(art_sql))) {
            fatal(ERR_DB_PREPARE, "mysql prepare article-update failed: %s", mysql_error(db->mysql));
        }
        db->mysql_article_ins = mysql_stmt_init(db->mysql);
        if (!db->mysql_article_ins) warnf("mysql_stmt_init failed for article-insert");
        const char *art_ins = "INSERT INTO `articles` (`artnum`, `subject`, `author`, `date`, `message_id`, `refs`, `bytes`, `line_count`, `group_name`) VALUES (?,?,?,?,?,?,?,?,?)";
        if (db->mysql_article_ins && mysql_stmt_prepare(db->mysql_article_ins, art_ins, (unsigned long)strlen(art_ins))) {
            warnf("mysql prepare article-insert failed: %s", mysql_error(db->mysql));
            mysql_stmt_close(db->mysql_article_ins); db->mysql_article_ins = NULL;
        }
        /* MySQL group upsert uses quoted table/columns */
    }
}

/* Insert group info */
static void db_insert_group(DB *db, const char *name, int count, int first, int last) {
    if (db->type == DB_SQLITE && db->sqlite_insert_group) {
        if (sqlite3_bind_int(db->sqlite_insert_group, 1, count) != SQLITE_OK ||
            sqlite3_bind_int(db->sqlite_insert_group, 2, first) != SQLITE_OK ||
            sqlite3_bind_int(db->sqlite_insert_group, 3, last) != SQLITE_OK ||
            sqlite3_bind_text(db->sqlite_insert_group, 4, name, -1, SQLITE_TRANSIENT) != SQLITE_OK) {
            warnf("sqlite bind group-update failed: %s", sqlite3_errmsg(db->sqlite));
            sqlite3_reset(db->sqlite_insert_group); sqlite3_clear_bindings(db->sqlite_insert_group); return;
        }
        if (sqlite3_step(db->sqlite_insert_group) != SQLITE_DONE) {
            warnf("sqlite group update step failed: %s", sqlite3_errmsg(db->sqlite));
        }
        int ch = sqlite3_changes(db->sqlite);
        sqlite3_reset(db->sqlite_insert_group); sqlite3_clear_bindings(db->sqlite_insert_group);
        if (ch == 0) {
            if (g_upsert && db->sqlite_group_ins) {
                if (sqlite3_bind_text(db->sqlite_group_ins, 1, name, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
                    sqlite3_bind_int(db->sqlite_group_ins, 2, count) != SQLITE_OK ||
                    sqlite3_bind_int(db->sqlite_group_ins, 3, first) != SQLITE_OK ||
                    sqlite3_bind_int(db->sqlite_group_ins, 4, last) != SQLITE_OK) {
                    warnf("sqlite bind group-insert failed: %s", sqlite3_errmsg(db->sqlite));
                    sqlite3_reset(db->sqlite_group_ins); sqlite3_clear_bindings(db->sqlite_group_ins);
                } else {
                    if (sqlite3_step(db->sqlite_group_ins) != SQLITE_DONE) {
                        warnf("sqlite group insert step failed: %s", sqlite3_errmsg(db->sqlite));
                    }
                    sqlite3_reset(db->sqlite_group_ins); sqlite3_clear_bindings(db->sqlite_group_ins);
                    infof("group inserted: %s", name);
                }
            } else {
                warnf("group not found for update: %s", name);
            }
        }
        return;
    }
    if (db->type == DB_MYSQL) {
        char esc_name[512];
        mysql_real_escape_string(db->mysql, esc_name, name, (unsigned long)strlen(name));
        char sql[1024];
        snprintf(sql, sizeof(sql), "UPDATE `groups` SET article_count=%d, first=%d, last=%d WHERE name='%s'", count, first, last, esc_name);
        if (mysql_query(db->mysql, sql)) {
            warnf("mysql group update error: %s", mysql_error(db->mysql));
        }
        my_ulonglong ch = mysql_affected_rows(db->mysql);
        if (ch == 0) {
            if (g_upsert) {
                snprintf(sql, sizeof(sql), "INSERT INTO `groups` (name,article_count,first,last) VALUES ('%s',%d,%d,%d)", esc_name, count, first, last);
                if (mysql_query(db->mysql, sql)) {
                    warnf("mysql group insert error: %s", mysql_error(db->mysql));
                } else {
                    infof("group inserted: %s", name);
                }
            } else {
                warnf("group not found for update: %s", name);
            }
        }
        return;
    }
}

/* Insert article header data */
static void db_insert_article(DB *db, const char *group, int artnum, const char *subject,
                              const char *author, const char *date, const char *message_id,
                              const char *references, int bytes, int lines) {
    if (db->type == DB_SQLITE && db->sqlite_insert_article) {
        if (sqlite3_bind_text(db->sqlite_insert_article, 1, subject, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
            sqlite3_bind_text(db->sqlite_insert_article, 2, author, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
            sqlite3_bind_text(db->sqlite_insert_article, 3, date, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
            sqlite3_bind_text(db->sqlite_insert_article, 4, message_id, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
            sqlite3_bind_text(db->sqlite_insert_article, 5, references, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
            sqlite3_bind_int(db->sqlite_insert_article, 6, bytes) != SQLITE_OK ||
            sqlite3_bind_int(db->sqlite_insert_article, 7, lines) != SQLITE_OK ||
            sqlite3_bind_text(db->sqlite_insert_article, 8, group, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
            sqlite3_bind_int(db->sqlite_insert_article, 9, artnum) != SQLITE_OK) {
            warnf("sqlite bind article-update failed: %s", sqlite3_errmsg(db->sqlite));
            sqlite3_reset(db->sqlite_insert_article); sqlite3_clear_bindings(db->sqlite_insert_article); return;
        }
        if (sqlite3_step(db->sqlite_insert_article) != SQLITE_DONE) {
            warnf("sqlite article update step failed: %s", sqlite3_errmsg(db->sqlite));
        }
        int ch = sqlite3_changes(db->sqlite);
        sqlite3_reset(db->sqlite_insert_article); sqlite3_clear_bindings(db->sqlite_insert_article);
        if (ch == 0) {
            if (g_upsert && db->sqlite_article_ins) {
                if (sqlite3_bind_int(db->sqlite_article_ins, 1, artnum) != SQLITE_OK ||
                    sqlite3_bind_text(db->sqlite_article_ins, 2, subject, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
                    sqlite3_bind_text(db->sqlite_article_ins, 3, author, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
                    sqlite3_bind_text(db->sqlite_article_ins, 4, date, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
                    sqlite3_bind_text(db->sqlite_article_ins, 5, message_id, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
                    sqlite3_bind_text(db->sqlite_article_ins, 6, references, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
                    sqlite3_bind_int(db->sqlite_article_ins, 7, bytes) != SQLITE_OK ||
                    sqlite3_bind_int(db->sqlite_article_ins, 8, lines) != SQLITE_OK ||
                    sqlite3_bind_text(db->sqlite_article_ins, 9, group, -1, SQLITE_TRANSIENT) != SQLITE_OK) {
                    warnf("sqlite bind article-insert failed: %s", sqlite3_errmsg(db->sqlite));
                    sqlite3_reset(db->sqlite_article_ins); sqlite3_clear_bindings(db->sqlite_article_ins);
                } else {
                    if (sqlite3_step(db->sqlite_article_ins) != SQLITE_DONE) {
                        warnf("sqlite article insert step failed: %s", sqlite3_errmsg(db->sqlite));
                    }
                    sqlite3_reset(db->sqlite_article_ins); sqlite3_clear_bindings(db->sqlite_article_ins);
                    infof("article inserted: %s #%d", group, artnum);
                }
            } else {
                warnf("article not found for update: %s #%d", group, artnum);
            }
        }
        return;
    }
    if (db->type == DB_MYSQL && db->mysql_insert_article) {
        MYSQL_BIND b[9]; memset(b, 0, sizeof(b));
        unsigned long sl = (unsigned long)strlen(subject);
        unsigned long al = (unsigned long)strlen(author);
        unsigned long dl = (unsigned long)strlen(date);
        unsigned long ml = (unsigned long)strlen(message_id);
        unsigned long rl = (unsigned long)strlen(references);
        unsigned long gl = (unsigned long)strlen(group);
        b[0].buffer_type = MYSQL_TYPE_STRING; b[0].buffer=(void*)subject; b[0].buffer_length=sl;
        b[1].buffer_type = MYSQL_TYPE_STRING; b[1].buffer=(void*)author; b[1].buffer_length=al;
        b[2].buffer_type = MYSQL_TYPE_STRING; b[2].buffer=(void*)date; b[2].buffer_length=dl;
        b[3].buffer_type = MYSQL_TYPE_STRING; b[3].buffer=(void*)message_id; b[3].buffer_length=ml;
        b[4].buffer_type = MYSQL_TYPE_STRING; b[4].buffer=(void*)references; b[4].buffer_length=rl;
        b[5].buffer_type = MYSQL_TYPE_LONG; b[5].buffer=(void*)&bytes;
        b[6].buffer_type = MYSQL_TYPE_LONG; b[6].buffer=(void*)&lines;
        b[7].buffer_type = MYSQL_TYPE_STRING; b[7].buffer=(void*)group; b[7].buffer_length=gl;
        b[8].buffer_type = MYSQL_TYPE_LONG; b[8].buffer=(void*)&artnum;
        if (mysql_stmt_bind_param(db->mysql_insert_article, b)) {
            warnf("mysql bind article-update failed: %s", mysql_stmt_error(db->mysql_insert_article));
            mysql_stmt_reset(db->mysql_insert_article); return;
        }
        if (mysql_stmt_execute(db->mysql_insert_article)) {
            warnf("mysql execute article-update failed: %s", mysql_stmt_error(db->mysql_insert_article));
        }
        my_ulonglong ch = mysql_stmt_affected_rows(db->mysql_insert_article);
        mysql_stmt_reset(db->mysql_insert_article);
        if (ch == 0) {
            if (g_upsert && db->mysql_article_ins) {
                MYSQL_BIND bi[9]; memset(bi, 0, sizeof(bi));
                bi[0].buffer_type = MYSQL_TYPE_LONG; bi[0].buffer=(void*)&artnum;
                bi[1].buffer_type = MYSQL_TYPE_STRING; bi[1].buffer=(void*)subject; bi[1].buffer_length=sl;
                bi[2].buffer_type = MYSQL_TYPE_STRING; bi[2].buffer=(void*)author; bi[2].buffer_length=al;
                bi[3].buffer_type = MYSQL_TYPE_STRING; bi[3].buffer=(void*)date; bi[3].buffer_length=dl;
                bi[4].buffer_type = MYSQL_TYPE_STRING; bi[4].buffer=(void*)message_id; bi[4].buffer_length=ml;
                bi[5].buffer_type = MYSQL_TYPE_STRING; bi[5].buffer=(void*)references; bi[5].buffer_length=rl;
                bi[6].buffer_type = MYSQL_TYPE_LONG; bi[6].buffer=(void*)&bytes;
                bi[7].buffer_type = MYSQL_TYPE_LONG; bi[7].buffer=(void*)&lines;
                bi[8].buffer_type = MYSQL_TYPE_STRING; bi[8].buffer=(void*)group; bi[8].buffer_length=gl;
                if (mysql_stmt_bind_param(db->mysql_article_ins, bi)) {
                    warnf("mysql bind article-insert failed: %s", mysql_stmt_error(db->mysql_article_ins));
                    mysql_stmt_reset(db->mysql_article_ins);
                } else if (mysql_stmt_execute(db->mysql_article_ins)) {
                    warnf("mysql execute article-insert failed: %s", mysql_stmt_error(db->mysql_article_ins));
                    mysql_stmt_reset(db->mysql_article_ins);
                } else {
                    mysql_stmt_reset(db->mysql_article_ins);
                    infof("article inserted: %s #%d", group, artnum);
                }
            } else {
                warnf("article not found for update: %s #%d", group, artnum);
            }
        }
        return;
    }
    /* fallback to legacy escaping if prepared failed */
    char *g = db_escape(db, group);
    char *s = db_escape(db, subject);
    char *a = db_escape(db, author);
    char *d = db_escape(db, date);
    char *m = db_escape(db, message_id);
    char *r = db_escape(db, references);
    char sql[4096];
    snprintf(sql, sizeof(sql),
             "UPDATE `articles` SET `subject`=%s, `author`=%s, `date`=%s, `message_id`=%s, `refs`=%s, `bytes`=%d, `line_count`=%d WHERE `group_name`=%s AND `artnum`=%d;",
             s, a, d, m, r, bytes, lines, g, artnum);
    db_exec(db, sql);
    if (g_upsert) {
        snprintf(sql, sizeof(sql),
            "INSERT INTO `articles` (`artnum`, `subject`, `author`, `date`, `message_id`, `refs`, `bytes`, `line_count`, `group_name`) VALUES (%d,%s,%s,%s,%s,%s,%d,%d,%s);",
            artnum, s, a, d, m, r, bytes, lines, g);
        db_exec(db, sql);
    }
    free(g); free(s); free(a); free(d); free(m); free(r);
}

/* NNTP commands */

/* read server greeting */

/* Authenticate using AUTHINFO USER/PASS */
static int nntp_auth(Conn *c, const char *user, const char *pass) {
    char resp[BUFSZ];
    conn_sendf(c, "AUTHINFO USER %s", user);
    if (conn_readline(c, resp, sizeof(resp)) <= 0) return -1;
    int code = atoi(resp);
    if (code == 381) {
        conn_sendf(c, "AUTHINFO PASS %s", pass);
        if (conn_readline(c, resp, sizeof(resp)) <= 0) return -1;
        return atoi(resp);
    }
    return code;
}

/* request STARTTLS */
static int nntp_starttls(Conn *c) {
    char resp[BUFSZ];
    conn_sendf(c, "STARTTLS");
    if (conn_readline(c, resp, sizeof(resp)) <= 0) return -1;
    return atoi(resp);
}

static int nntp_group(Conn *c, const char *group, int *count, int *first, int *last) {
    char buf[BUFSZ];
    conn_sendf(c, "GROUP %s", group);
    if (conn_readline(c, buf, sizeof(buf)) <= 0) return -1;
    int code = atoi(buf);
    if (code >= 200 && code < 300) {
        /* parse: 211 <count> <first> <last> <groupname> */
        int cnt=0, f=0, l=0;
        sscanf(buf, "%*d %d %d %d", &cnt, &f, &l);
        if (count) *count = cnt;
        if (first) *first = f;
        if (last) *last = l;
    }
    return code;
}

/* fetch XOVER for a range; returns malloc'd content with lines separated by \n */
static char *nntp_xover(Conn *c, int first, int last) {
    char buf[BUFSZ];
    conn_sendf(c, "XOVER %d-%d", first, last);
    if (conn_readline(c, buf, sizeof(buf)) <= 0) return NULL;
    int code = atoi(buf);
    if (code < 200 || code >= 300) {
        warnf("XOVER rejected: %s", buf);
        return NULL;
    }
    return conn_read_multiline(c);
}

/* fetch full header for article number */
static char *nntp_head(Conn *c, int artnum) {
    char buf[BUFSZ];
    conn_sendf(c, "HEAD %d", artnum);
    if (conn_readline(c, buf, sizeof(buf)) <= 0) return NULL;
    int code = atoi(buf);
    if (code < 200 || code >= 300) {
        warnf("HEAD rejected for %d: %s", artnum, buf);
        return NULL;
    }
    return conn_read_multiline(c);
}

/* parse XOVER line: fields are tab-separated. Common format:
   article-number TAB subject TAB author TAB date TAB message-id TAB references TAB bytes TAB lines TAB xref
   We'll read first 8 fields.
*/
static void parse_xover_line(const char *line, int *artnum, char **subject, char **author, char **date,
                             char **message_id, char **references, int *bytes, int *lines) {
    const char *p = line;
    char *fields[9] = {0};
    int f = 0;
    size_t len = strlen(line);
    char *tmp = malloc(len+1);
    strcpy(tmp, line);
    char *tk = tmp;
    char *saveptr = NULL;
    char *tok = strtok_r(tk, "\t", &saveptr);
    while (tok && f < 9) {
        fields[f++] = strdup(tok);
        tok = strtok_r(NULL, "\t", &saveptr);
    }
    if (fields[0]) *artnum = atoi(fields[0]);
    *subject = fields[1] ? fields[1] : strdup("");
    *author = fields[2] ? fields[2] : strdup("");
    *date = fields[3] ? fields[3] : strdup("");
    *message_id = fields[4] ? fields[4] : strdup("");
    *references = fields[5] ? fields[5] : strdup("");
    *bytes = fields[6] ? atoi(fields[6]) : 0;
    *lines = fields[7] ? atoi(fields[7]) : 0;
    /* free tmp structure holders except those we returned */
    free(tmp);
}

/* Strip header values: find "Subject: ..." lines etc. For HEAD responses, we can parse basic fields */
static void extract_from_headers(const char *hdrs, char **subject, char **from, char **date, char **message_id, char **references, int *bytes, int *lines) {
    *subject = strdup("");
    *from = strdup("");
    *date = strdup("");
    *message_id = strdup("");
    *references = strdup("");
    *bytes = 0; *lines = 0;
    char *copy = strdup(hdrs);
    char *line = strtok(copy, "\n");
    while (line) {
        while (*line && isspace((unsigned char)*line)) line++;
        if (strncasecmp(line, "Subject:", 8) == 0) {
            free(*subject); *subject = strdup(line + 8);
            while (**subject == ' ') memmove(*subject, *subject+1, strlen(*subject));
        } else if (strncasecmp(line, "From:", 5) == 0) {
            free(*from); *from = strdup(line + 5);
            while (**from == ' ') memmove(*from, *from+1, strlen(*from));
        } else if (strncasecmp(line, "Date:", 5) == 0) {
            free(*date); *date = strdup(line + 5);
            while (**date == ' ') memmove(*date, *date+1, strlen(*date));
        } else if (strncasecmp(line, "Message-ID:", 11) == 0) {
            free(*message_id); *message_id = strdup(line + 11);
            while (**message_id == ' ') memmove(*message_id, *message_id+1, strlen(*message_id));
        } else if (strncasecmp(line, "References:", 11) == 0) {
            free(*references); *references = strdup(line + 11);
            while (**references == ' ') memmove(*references, *references+1, strlen(*references));
        } else if (strncasecmp(line, "Lines:", 6) == 0) {
            *lines = atoi(line + 6);
        } else if (strncasecmp(line, "Bytes:", 6) == 0) {
            *bytes = atoi(line + 6);
        }
        line = strtok(NULL, "\n");
    }
    free(copy);
}

/* Work queue + threading (C99 compatible) */
typedef struct {
    int *items;
    int head;
    int tail;
    int capacity;
    pthread_mutex_t m;
} WorkQueue;

static void queue_init(WorkQueue *q, int capacity) {
    q->items = malloc(sizeof(int) * capacity);
    q->head = 0; q->tail = 0; q->capacity = capacity;
    pthread_mutex_init(&q->m, NULL);
}

static void queue_push(WorkQueue *q, int v) {
    if (q->tail < q->capacity) q->items[q->tail++] = v;
}

static int queue_pop(WorkQueue *q, int *v) {
    int ok = 0;
    pthread_mutex_lock(&q->m);
    if (q->head < q->tail) { *v = q->items[q->head++]; ok = 1; }
    pthread_mutex_unlock(&q->m);
    return ok;
}

static void queue_destroy(WorkQueue *q) {
    if (q->items) free(q->items);
    pthread_mutex_destroy(&q->m);
    memset(q, 0, sizeof(*q));
}

typedef struct {
    DB *db;
    const char *group;
    int retries;
    int progress_width;
    int total;
    volatile int *processed;
    pthread_mutex_t *progress_mutex;
    pthread_mutex_t *db_mutex;
    WorkQueue *queue;
    const char *host; const char *port; int use_ssl; int do_starttls; const char *user; const char *pass;
} WorkerArgs;

static int thread_connect(Conn *tc, const char *host, const char *port, int use_ssl, int do_starttls, const char *user, const char *pass, const char *group) {
    conn_init(tc);
    tc->sock = tcp_connect(host, port);
    if (tc->sock < 0) return 0;
    if (use_ssl) {
        ssl_init();
        tc->ctx = SSL_CTX_new(TLS_client_method()); if (!tc->ctx) return 0;
        tc->ssl = SSL_new(tc->ctx); if (!tc->ssl) return 0;
        if (!SSL_set_fd(tc->ssl, tc->sock)) return 0;
        if (SSL_connect(tc->ssl) <= 0) { ERR_print_errors_fp(stderr); return 0; }
        tc->use_ssl = 1;
    }
    {
        char line[BUFSZ]; if (conn_readline(tc, line, sizeof(line)) <= 0) return 0; if (atoi(line) >= 400) return 0;
    }
    if (do_starttls && !use_ssl) {
        int rcx = nntp_starttls(tc); if (rcx < 200 || rcx >= 300) return 0; if (!conn_starttls(tc)) return 0;
    }
    if (user && pass) { int rcx = nntp_auth(tc, user, pass); if (rcx >= 400) return 0; }
    {
        int dummyC=0,dummyF=0,dummyL=0; int rcg = nntp_group(tc, group, &dummyC, &dummyF, &dummyL); if (rcg < 200 || rcg >= 300) return 0;
    }
    return 1;
}

static void *head_worker(void *arg) {
    WorkerArgs *wa = (WorkerArgs*)arg;
    Conn tc;
    if (!thread_connect(&tc, wa->host, wa->port, wa->use_ssl, wa->do_starttls, wa->user, wa->pass, wa->group)) {
        warnf("thread connect failed");
        return NULL;
    }
    int artnum;
    while (queue_pop(wa->queue, &artnum)) {
        int attempt = 0; char *hdrs = NULL;
        while (attempt <= wa->retries) { hdrs = nntp_head(&tc, artnum); if (hdrs) break; attempt++; }
        if (!hdrs) continue;
        char *subject, *from, *datev, *msgid, *refs; int bytes=0, linesz=0;
        extract_from_headers(hdrs, &subject, &from, &datev, &msgid, &refs, &bytes, &linesz);
        pthread_mutex_lock(wa->db_mutex);
        db_insert_article(wa->db, wa->group, artnum, subject, from, datev, msgid, refs, bytes, linesz);
        pthread_mutex_unlock(wa->db_mutex);
        free(subject); free(from); free(datev); free(msgid); free(refs); free(hdrs);
        pthread_mutex_lock(wa->progress_mutex);
        (*wa->processed)++;
        {
            int local = *wa->processed; int pct = (int)((local * 100.0)/(wa->total?wa->total:1));
            int filled = (int)(wa->progress_width * (local/(double)(wa->total?wa->total:1))); if (filled > wa->progress_width) filled = wa->progress_width;
            char *bar = malloc(wa->progress_width + 1);
            if (bar) {
                int bi; for (bi = 0; bi < wa->progress_width; ++bi) bar[bi] = (bi < filled) ? '#' : '.'; bar[wa->progress_width] = '\0';
                fprintf(stdout, "\rHeaders (HEAD MT): [%s] %3d%% (%d/%d)", bar, pct, local, wa->total);
                free(bar);
            }
            fflush(stdout);
        }
        pthread_mutex_unlock(wa->progress_mutex);
    }
    conn_cleanup(&tc);
    return NULL;
}

/* CLI parsing - very simple */
static void usage_and_exit(const char *prog, AppError code, const char *detail) {
    printf("Usage: %s --host HOST [--port PORT] [--ssl] [--starttls] [--user USER --pass PASS]\n"
           "          --db-type {sqlite|mariadb|mysql} --db-name DBNAME [--db-host HOST --db-port PORT --db-user USER --db-pass PASS]\n"
            "          --group GROUPNAME [--headers-only] [--limit N] [--progress-width N] [--init-db|--create-db]\n"
           "          [--threads N] [--retries N] [--conf FILE] [--write-conf FILE] [--log FILE] [--verbose] (write-conf exits after saving)\n", prog);
    if (detail && *detail) {
        fprintf(stderr, "Error (code %d): %s\n", code, describe_error(code));
        fprintf(stderr, "Details: %s\n", detail);
    }
    exit(code);
}

/* Simple key=value config file loader */
static void trim(char *s) {
    char *p = s; while (*p && isspace((unsigned char)*p)) p++; if (p!=s) memmove(s,p,strlen(p)+1);
    size_t n = strlen(s); while (n && isspace((unsigned char)s[n-1])) s[--n] = '\0';
}

static int load_conf(const char *path,
                     const char **host, const char **port, int *use_ssl, int *do_starttls,
                     const char **user, const char **pass,
                     const char **db_type_s, const char **db_name, const char **db_host,
                     const char **db_port, const char **db_user, const char **db_pass,
                     const char **group, int *headers_only, int *limit, int *progress_width) {
    FILE *f = fopen(path, "r");
    if (!f) { warnf("Could not open conf: %s", path); return 0; }
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        trim(line);
        if (!*line || *line=='#' || *line==';') continue;
        char *eq = strchr(line,'=');
        if (!eq) continue;
        *eq='\0'; char *key=line; char *val=eq+1; trim(key); trim(val);
        if (strcasecmp(key,"host")==0) *host = strdup(val);
        else if (strcasecmp(key,"port")==0) *port = strdup(val);
        else if (strcasecmp(key,"ssl")==0) *use_ssl = atoi(val)?1:0;
        else if (strcasecmp(key,"starttls")==0) *do_starttls = atoi(val)?1:0;
        else if (strcasecmp(key,"user")==0) *user = strdup(val);
        else if (strcasecmp(key,"pass")==0) *pass = strdup(val);
        else if (strcasecmp(key,"db_type")==0 || strcasecmp(key,"db-type")==0) *db_type_s = strdup(val);
        else if (strcasecmp(key,"db_name")==0 || strcasecmp(key,"db-name")==0) *db_name = strdup(val);
        else if (strcasecmp(key,"db_host")==0 || strcasecmp(key,"db-host")==0) *db_host = strdup(val);
        else if (strcasecmp(key,"db_port")==0 || strcasecmp(key,"db-port")==0) *db_port = strdup(val);
        else if (strcasecmp(key,"db_user")==0 || strcasecmp(key,"db-user")==0) *db_user = strdup(val);
        else if (strcasecmp(key,"db_pass")==0 || strcasecmp(key,"db-pass")==0) *db_pass = strdup(val);
        else if (strcasecmp(key,"group")==0) *group = strdup(val);
        else if (strcasecmp(key,"headers_only")==0 || strcasecmp(key,"headers-only")==0) *headers_only = atoi(val)?1:0;
        else if (strcasecmp(key,"limit")==0) *limit = atoi(val);
        else if (strcasecmp(key,"progress_width")==0 || strcasecmp(key,"progress-width")==0) *progress_width = atoi(val);
    }
    fclose(f);
    return 1;
}

static int write_conf(const char *path,
                      const char *host, const char *port, int use_ssl, int do_starttls,
                      const char *user, const char *pass,
                      const char *db_type_s, const char *db_name, const char *db_host,
                      const char *db_port, const char *db_user, const char *db_pass,
                      const char *group, int headers_only, int limit, int progress_width) {
    FILE *f = fopen(path, "w");
    if (!f) { warnf("Could not write conf: %s", path); return 0; }
    fprintf(f,"# nntp2sql configuration\n");
    if (host) fprintf(f,"host=%s\n", host);
    if (port) fprintf(f,"port=%s\n", port);
    fprintf(f,"ssl=%d\n", use_ssl);
    fprintf(f,"starttls=%d\n", do_starttls);
    if (user) fprintf(f,"user=%s\n", user);
    if (pass) fprintf(f,"pass=%s\n", pass);
    if (db_type_s) fprintf(f,"db_type=%s\n", db_type_s);
    if (db_name) fprintf(f,"db_name=%s\n", db_name);
    if (db_host) fprintf(f,"db_host=%s\n", db_host);
    if (db_port) fprintf(f,"db_port=%s\n", db_port);
    if (db_user) fprintf(f,"db_user=%s\n", db_user);
    if (db_pass) fprintf(f,"db_pass=%s\n", db_pass);
    if (group) fprintf(f,"group=%s\n", group);
    fprintf(f,"headers_only=%d\n", headers_only);
    fprintf(f,"limit=%d\n", limit);
    fprintf(f,"progress_width=%d\n", progress_width);
    fclose(f);
    return 1;
}

int main(int argc, char **argv) {
    const char *host = NULL, *port = NULL, *user = NULL, *pass = NULL;
    int use_ssl = 0, do_starttls = 0;
    const char *db_type_s = NULL, *db_name = NULL, *db_host = NULL, *db_user = NULL, *db_pass = NULL, *db_port = NULL;
    const char *group = NULL;
    int headers_only = 0;
    int limit = 0;
    int progress_width = 40; /* default bar width */
    int init_db = 0; /* create database if needed */
    int create_db_exit = 0; /* if --create-db used: create DB + schema then exit */
    const char *conf_path = NULL; /* load settings */
    const char *write_conf_path = NULL; /* save settings */
    int threads = 1; /* multithread workers for HEAD */
    int retries = 3; /* HEAD retry attempts */
    const char *log_path = NULL; /* optional log file */
    int i = 1;
    while (i < argc) {
        if (strcmp(argv[i], "--host") == 0) { host = argv[++i]; }
        else if (strcmp(argv[i], "--port") == 0) { port = argv[++i]; }
        else if (strcmp(argv[i], "--ssl") == 0) { use_ssl = 1; }
        else if (strcmp(argv[i], "--starttls") == 0) { do_starttls = 1; }
        else if (strcmp(argv[i], "--user") == 0) { user = argv[++i]; }
        else if (strcmp(argv[i], "--pass") == 0) { pass = argv[++i]; }
        else if (strcmp(argv[i], "--db-type") == 0) { db_type_s = argv[++i]; }
        else if (strcmp(argv[i], "--db-name") == 0) { db_name = argv[++i]; }
        else if (strcmp(argv[i], "--db-host") == 0) { db_host = argv[++i]; }
        else if (strcmp(argv[i], "--db-port") == 0) { db_port = argv[++i]; }
        else if (strcmp(argv[i], "--db-user") == 0) { db_user = argv[++i]; }
        else if (strcmp(argv[i], "--db-pass") == 0) { db_pass = argv[++i]; }
        else if (strcmp(argv[i], "--group") == 0) { group = argv[++i]; }
        else if (strcmp(argv[i], "--headers-only") == 0) { headers_only = 1; }
        else if (strcmp(argv[i], "--limit") == 0) { limit = atoi(argv[++i]); }
        else if (strcmp(argv[i], "--progress-width") == 0) { progress_width = atoi(argv[++i]); }
        else if (strcmp(argv[i], "--init-db") == 0) { init_db = 1; }
        else if (strcmp(argv[i], "--create-db") == 0) { init_db = 1; create_db_exit = 1; }
        else if (strcmp(argv[i], "--conf") == 0) { conf_path = argv[++i]; }
        else if (strcmp(argv[i], "--write-conf") == 0) { write_conf_path = argv[++i]; }
        else if (strcmp(argv[i], "--log") == 0) { log_path = argv[++i]; }
        else if (strcmp(argv[i], "--verbose") == 0) { g_verbose = 1; }
        else if (strcmp(argv[i], "--threads") == 0) { threads = atoi(argv[++i]); if (threads < 1) threads = 1; if (threads > 64) threads = 64; }
        else if (strcmp(argv[i], "--retries") == 0) { retries = atoi(argv[++i]); if (retries < 0) retries = 0; if (retries > 10) retries = 10; }
        else if (strcmp(argv[i], "--upsert") == 0) { g_upsert = 1; }
        else usage_and_exit(argv[0], ERR_ARGS, "unknown option");
        i++;
    }

    if (log_path) log_open(log_path);
    infof("Starting nntp2sql");

    /* load configuration file early (values from CLI override) */
    if (conf_path) {
        load_conf(conf_path, &host, &port, &use_ssl, &do_starttls,
                  &user, &pass, &db_type_s, &db_name, &db_host,
                  &db_port, &db_user, &db_pass, &group,
                  &headers_only, &limit, &progress_width);
    }
    /* apply defaults if not provided */
    if (!host) host = "localhost"; /* default NNTP host */
    if (!host || !db_type_s || !db_name || !group) usage_and_exit(argv[0], ERR_ARGS, "missing required parameters");
    if (!port) port = use_ssl ? "563" : "119";
    infof("Config: host=%s port=%s ssl=%d starttls=%d db=%s type=%s group=%s", host, port, use_ssl, do_starttls, db_name, db_type_s, group);

    DB db = {0};
    if (strcmp(db_type_s, "sqlite") == 0) db.type = DB_SQLITE;
    else if (strcmp(db_type_s, "mariadb") == 0 || strcmp(db_type_s, "mysql") == 0) db.type = DB_MYSQL;
    else fatal(ERR_ARGS, "Unknown db-type (expected sqlite|mariadb|mysql): %s", db_type_s);

    /* Open DB (optionally create database) */
    if (db.type == DB_SQLITE) {
        if (sqlite3_open(db_name, &db.sqlite) != SQLITE_OK) fatal(ERR_DB_CONNECT, "sqlite open failed: %s", sqlite3_errmsg(db.sqlite));
    } else if (db.type == DB_MYSQL) {
        const char *mh = db_host ? db_host : "localhost";
        unsigned int mp = db_port ? atoi(db_port) : 3306;
        const char *mu = db_user ? db_user : "root";
        const char *mpass = db_pass ? db_pass : "";
        if (init_db) {
            MYSQL *tmp = mysql_init(NULL);
            if (!tmp) fatal(ERR_DB_CONNECT, "mysql_init failed (init phase)");
            if (!mysql_real_connect(tmp, mh, mu, mpass, NULL, mp, NULL, 0)) {
                fatal(ERR_DB_CONNECT, "mysql server connect failed: %s", mysql_error(tmp));
            }
            char q[512];
            snprintf(q, sizeof(q), "CREATE DATABASE IF NOT EXISTS `%s` CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;", db_name);
            if (mysql_query(tmp, q)) warnf("mysql create database error: %s", mysql_error(tmp));
            mysql_close(tmp);
        }
        db.mysql = mysql_init(NULL);
        if (!db.mysql) fatal(ERR_DB_CONNECT, "mysql_init failed");
        if (!mysql_real_connect(db.mysql, mh, mu, mpass, db_name, mp, NULL, 0)) {
            fatal(ERR_DB_CONNECT, "mysql connect failed: %s", mysql_error(db.mysql));
        }
    }

    if (write_conf_path) {
        write_conf(write_conf_path, host, port, use_ssl, do_starttls,
                   user, pass, db_type_s, db_name, db_host, db_port, db_user, db_pass,
                   group, headers_only, limit, progress_width);
        fprintf(stdout, "Configuration written to %s\n", write_conf_path);
        db_close(&db);
        log_close();
        return 0; /* exit after saving */
    }

    db_init_schema(&db);

    if (create_db_exit) {
        fprintf(stdout, "Database and schema created for '%s' (%s)\n", db_name, db_type_s);
        db_close(&db);
        log_close();
        return 0; /* exit without NNTP ingest */
    }

    /* NNTP connection */
    Conn c;
    conn_init(&c);
    c.sock = tcp_connect(host, port);
    if (c.sock < 0) fatal(ERR_NET_CONNECT, "Unable to connect to %s:%s", host, port);

    /* SSL on connect? */
    if (use_ssl) {
        ssl_init();
        c.ctx = SSL_CTX_new(TLS_client_method());
        if (!c.ctx) fatal(ERR_TLS, "SSL_CTX_new failed");
        c.ssl = SSL_new(c.ctx);
        if (!c.ssl) fatal(ERR_TLS, "SSL_new failed");
        if (!SSL_set_fd(c.ssl, c.sock)) fatal(ERR_TLS, "SSL_set_fd failed");
        if (SSL_connect(c.ssl) <= 0) { ERR_print_errors_fp(stderr); fatal(ERR_TLS, "SSL_connect failed"); }
        c.use_ssl = 1;
    }

    char line[BUFSZ];
    if (conn_readline(&c, line, sizeof(line)) <= 0) fatal(ERR_NNTP_GREETING, "No greeting from server");
    int greet = atoi(line);
    if (greet >= 400) fatal(ERR_NNTP_GREETING, "Server error: %s", line);

    /* optionally STARTTLS */
    if (do_starttls) {
        int rc = nntp_starttls(&c);
        if (rc < 200 || rc >= 300) fatal(ERR_TLS, "STARTTLS failed: %d", rc);
        if (!conn_starttls(&c)) fatal(ERR_TLS, "TLS handshake failed");
    }

    /* AUTH if provided */
    if (user && pass) {
        int rc = nntp_auth(&c, user, pass);
        if (rc >= 400) fatal(ERR_AUTH, "AUTH failed: %d", rc);
    }

    /* select group */
    int count=0, first=0, last=0;
    int rc = nntp_group(&c, group, &count, &first, &last);
    if (rc < 200 || rc >= 300) fatal(ERR_NNTP_CMD, "GROUP failed: %d", rc);
    db_insert_group(&db, group, count, first, last);

    if (count == 0) {
        warnf("Group has no articles.");
        db_close(&db);
        conn_cleanup(&c);
        return 0;
    }

    int fetch_first = first, fetch_last = last;
    if (limit > 0) {
        if (limit < (last - first + 1)) {
            fetch_first = last - limit + 1;
            if (fetch_first < first) fetch_first = first;
        }
    }

    /* sanitize progress width */
    if (progress_width < 5) progress_width = 5;
    if (progress_width > 200) progress_width = 200;

    if (headers_only) {
        char *xdata = nntp_xover(&c, fetch_first, fetch_last);
        if (!xdata) warnf("XOVER returned no data");
        else {
            int total = fetch_last - fetch_first + 1;
            int processed = 0;
            char *saveptr = NULL;
            char *line2 = strtok_r(xdata, "\n", &saveptr);
            while (line2) {
                int artnum = 0; char *subject=NULL,*author=NULL,*date=NULL,*msgid=NULL,*refs=NULL; int bytes=0, linesz=0;
                parse_xover_line(line2, &artnum, &subject, &author, &date, &msgid, &refs, &bytes, &linesz);
                db_insert_article(&db, group, artnum, subject, author, date, msgid, refs, bytes, linesz);
                free(subject); free(author); free(date); free(msgid); free(refs);
                processed++;
                int pct = (int)((processed * 100.0) / (total ? total : 1));
                int filled = (int)(progress_width * (processed / (double)(total ? total : 1)));
                if (filled > progress_width) filled = progress_width;
                char *bar = malloc(progress_width + 1);
                if (!bar) fatal(ERR_RUNTIME, "Out of memory allocating progress bar");
                for (int bi = 0; bi < progress_width; ++bi) bar[bi] = (bi < filled) ? '#' : '.';
                bar[progress_width] = '\0';
                fprintf(stdout, "\rHeaders (XOVER): [%s] %3d%% (%d/%d)", bar, pct, processed, total);
                free(bar);
                fflush(stdout);
                line2 = strtok_r(NULL, "\n", &saveptr);
            }
            fprintf(stdout, "\n");
            free(xdata);
        }
    } else {
        /* Multithread HEAD fetching (C99) */
        int total = fetch_last - fetch_first + 1;
        volatile int processed = 0;
        pthread_mutex_t progress_mutex; pthread_mutex_init(&progress_mutex, NULL);
        pthread_mutex_t db_mutex; pthread_mutex_init(&db_mutex, NULL);
        WorkQueue wq; queue_init(&wq, total);
        for (int a = fetch_first; a <= fetch_last; ++a) queue_push(&wq, a);
        if (threads > total) threads = total;
        pthread_t *tids = malloc(sizeof(pthread_t) * threads);
        WorkerArgs wa;
        wa.db = &db; wa.group = group; wa.retries = retries; wa.progress_width = progress_width; wa.total = total; wa.processed = &processed;
        wa.progress_mutex = &progress_mutex; wa.db_mutex = &db_mutex; wa.queue = &wq;
        wa.host = host; wa.port = port; wa.use_ssl = use_ssl; wa.do_starttls = do_starttls; wa.user = user; wa.pass = pass;
        for (int ti = 0; ti < threads; ++ti) {
            if (pthread_create(&tids[ti], NULL, head_worker, &wa) != 0) warnf("pthread_create failed for thread %d", ti);
        }
        for (int ti = 0; ti < threads; ++ti) pthread_join(tids[ti], NULL);
        fprintf(stdout, "\n");
        free(tids);
        queue_destroy(&wq);
        pthread_mutex_destroy(&db_mutex);
        pthread_mutex_destroy(&progress_mutex);
    }

    db_close(&db);
    conn_cleanup(&c);
    log_close();
    return 0;
}
