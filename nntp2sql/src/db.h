#ifndef DB_H
#define DB_H
#include <sqlite3.h>
#include <mysql/mysql.h>

typedef enum { DB_SQLITE, DB_MYSQL } DBType;

typedef struct {
    DBType type;
    sqlite3 *sqlite;
    MYSQL *mysql;
    sqlite3_stmt *sqlite_insert_article;
    sqlite3_stmt *sqlite_insert_group;
    MYSQL_STMT *mysql_insert_article;
    sqlite3_stmt *sqlite_article_ins;
    sqlite3_stmt *sqlite_group_ins;
    MYSQL_STMT *mysql_article_ins;
} DB;

void db_close(DB *db);
void db_init_schema(DB *db);
void db_insert_group(DB *db, const char *name, int count, int first, int last);
void db_insert_article(DB *db, const char *group, int artnum, const char *subject,
                       const char *author, const char *date, const char *message_id,
                       const char *references, int bytes, int lines);
char *db_escape(DB *db, const char *s);

#endif