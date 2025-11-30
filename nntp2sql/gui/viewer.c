#include <gtk/gtk.h>
#include <sqlite3.h>
#include <mysql/mysql.h>
#include <libpq-fe.h>

static GtkWidget *entry;
static GtkListStore *store;
typedef enum { V_SQLITE, V_MYSQL, V_PG } VDbType;
static VDbType vtype;
static sqlite3 *sdb;
static MYSQL *mydb;
static PGconn *pgdb;

static void load_rows_sqlite(sqlite3 *db, const char *filter){
    gtk_list_store_clear(store);
    const char *sql = filter && *filter ?
        "SELECT artnum, subject, author, date FROM articles WHERE subject LIKE ? OR author LIKE ? ORDER BY artnum LIMIT 1000" :
        "SELECT artnum, subject, author, date FROM articles ORDER BY artnum LIMIT 1000";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return;
    if (filter && *filter){
        char like[256]; snprintf(like, sizeof(like), "%%%s%%", filter);
        sqlite3_bind_text(st, 1, like, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, like, -1, SQLITE_TRANSIENT);
    }
    while (sqlite3_step(st) == SQLITE_ROW){
        GtkTreeIter iter; gtk_list_store_append(store, &iter);
        gtk_list_store_set(store, &iter,
            0, sqlite3_column_int(st, 0),
            1, (const char*)sqlite3_column_text(st, 1),
            2, (const char*)sqlite3_column_text(st, 2),
            3, (const char*)sqlite3_column_text(st, 3),
            -1);
    }
    sqlite3_finalize(st);
}

static void load_rows_mysql(MYSQL *db, const char *filter){
    gtk_list_store_clear(store);
    char q[1024];
    if (filter && *filter){
        char esc[512]; mysql_real_escape_string(db, esc, filter, (unsigned long)strlen(filter));
        snprintf(q, sizeof(q), "SELECT artnum, subject, author, date FROM articles WHERE subject LIKE '%%%s%%' OR author LIKE '%%%s%%' ORDER BY artnum LIMIT 1000", esc, esc);
    } else {
        snprintf(q, sizeof(q), "SELECT artnum, subject, author, date FROM articles ORDER BY artnum LIMIT 1000");
    }
    if (mysql_query(db, q)) return;
    MYSQL_RES *res = mysql_store_result(db); if (!res) return;
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res))){
        GtkTreeIter iter; gtk_list_store_append(store, &iter);
        gtk_list_store_set(store, &iter,
            0, row[0] ? atoi(row[0]) : 0,
            1, row[1] ? row[1] : "",
            2, row[2] ? row[2] : "",
            3, row[3] ? row[3] : "",
            -1);
    }
    mysql_free_result(res);
}

static void load_rows_pg(PGconn *db, const char *filter){
    gtk_list_store_clear(store);
    PGresult *res;
    if (filter && *filter){
        const char *params[1] = { filter };
        res = PQexecParams(db,
            "SELECT artnum, subject, author, date FROM articles WHERE subject ILIKE '%'||$1||'%' OR author ILIKE '%'||$1||'%' ORDER BY artnum LIMIT 1000",
            1, NULL, params, NULL, NULL, 0);
    } else {
        res = PQexec(db, "SELECT artnum, subject, author, date FROM articles ORDER BY artnum LIMIT 1000");
    }
    if (PQresultStatus(res) != PGRES_TUPLES_OK){ PQclear(res); return; }
    int n = PQntuples(res);
    for (int i=0;i<n;i++){
        GtkTreeIter iter; gtk_list_store_append(store, &iter);
        gtk_list_store_set(store, &iter,
            0, atoi(PQgetvalue(res, i, 0)),
            1, PQgetvalue(res, i, 1),
            2, PQgetvalue(res, i, 2),
            3, PQgetvalue(res, i, 3),
            -1);
    }
    PQclear(res);
}

static void on_search(GtkEditable *e, gpointer data){
    const char *txt = gtk_entry_get_text(GTK_ENTRY(entry));
    if (vtype == V_SQLITE) load_rows_sqlite(sdb, txt);
    else if (vtype == V_MYSQL) load_rows_mysql(mydb, txt);
    else if (vtype == V_PG) load_rows_pg(pgdb, txt);
}

int main(int argc, char **argv){
    if (argc < 3){ g_printerr("usage: viewer --db-type {sqlite|mysql|postgres} <conn>\n  sqlite: path/to.db\n  mysql: host,db,user,pass[,port]\n  postgres: conninfo string\n"); return 2; }
    if (strcmp(argv[1], "--db-type") != 0){ g_printerr("first arg must be --db-type\n"); return 2; }
    const char *type = argv[2]; const char *conn = argv[3];
    if (strcmp(type, "sqlite") == 0){ vtype = V_SQLITE; if (sqlite3_open(conn, &sdb) != SQLITE_OK){ g_printerr("sqlite open failed\n"); return 1; } }
    else if (strcmp(type, "mysql") == 0){ vtype = V_MYSQL; mydb = mysql_init(NULL); if (!mydb){ g_printerr("mysql init failed\n"); return 1; }
        /* parse host,db,user,pass[,port] */
        char buf[512]; strncpy(buf, conn, sizeof(buf)-1); buf[sizeof(buf)-1]='\0';
        char *host = strtok(buf, ","); char *dbn = strtok(NULL, ","); char *user = strtok(NULL, ","); char *pass = strtok(NULL, ","); char *port = strtok(NULL, ",");
        unsigned int p = port ? (unsigned int)atoi(port) : 3306;
        if (!mysql_real_connect(mydb, host?host:"localhost", user?user:"root", pass?pass:"", dbn?dbn:"", p, NULL, 0)) { g_printerr("mysql connect failed: %s\n", mysql_error(mydb)); return 1; }
    }
    else if (strcmp(type, "postgres") == 0){ vtype = V_PG; pgdb = PQconnectdb(conn); if (PQstatus(pgdb) != CONNECTION_OK){ g_printerr("postgres connect failed: %s\n", PQerrorMessage(pgdb)); return 1; } }
    else { g_printerr("unknown db type\n"); return 2; }
    gtk_init(&argc, &argv);
    GtkWidget *win = gtk_window_new(); gtk_window_set_title(GTK_WINDOW(win), "NNTP Viewer"); gtk_window_set_default_size(GTK_WINDOW(win), 800, 480);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_window_set_child(GTK_WINDOW(win), box);
    entry = gtk_entry_new(); gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "Search subject/author...");
    gtk_box_append(GTK_BOX(box), entry);
    g_signal_connect(entry, "changed", G_CALLBACK(on_search), db);
    store = gtk_list_store_new(4, G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    GtkWidget *view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    GtkCellRenderer *r;
    r = gtk_cell_renderer_text_new(); gtk_tree_view_append_column(GTK_TREE_VIEW(view), gtk_tree_view_column_new_with_attributes("ArtNum", r, "text", 0, NULL));
    r = gtk_cell_renderer_text_new(); gtk_tree_view_append_column(GTK_TREE_VIEW(view), gtk_tree_view_column_new_with_attributes("Subject", r, "text", 1, NULL));
    r = gtk_cell_renderer_text_new(); gtk_tree_view_append_column(GTK_TREE_VIEW(view), gtk_tree_view_column_new_with_attributes("Author", r, "text", 2, NULL));
    r = gtk_cell_renderer_text_new(); gtk_tree_view_append_column(GTK_TREE_VIEW(view), gtk_tree_view_column_new_with_attributes("Date", r, "text", 3, NULL));
    GtkWidget *sw = gtk_scrolled_window_new(); gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), view);
    gtk_box_append(GTK_BOX(box), sw);
    if (vtype == V_SQLITE) load_rows_sqlite(sdb, NULL);
    else if (vtype == V_MYSQL) load_rows_mysql(mydb, NULL);
    else if (vtype == V_PG) load_rows_pg(pgdb, NULL);
    g_signal_connect(win, "close-request", G_CALLBACK(gtk_window_destroy), NULL);
    gtk_widget_show(win);
    gtk_main();
    if (vtype == V_SQLITE) sqlite3_close(sdb);
    else if (vtype == V_MYSQL) mysql_close(mydb);
    else if (vtype == V_PG) PQfinish(pgdb);
    return 0;
}
#include <gtk/gtk.h>
#include <sqlite3.h>
#include <mysql/mysql.h>
#include <stdio.h>
#include <string.h>

static GtkWidget *create_list_view(void) {
    GtkWidget *view = gtk_tree_view_new();
    GtkCellRenderer *renderer;
    GtkTreeViewColumn *column;

    renderer = gtk_cell_renderer_text_new();
    column = gtk_tree_view_column_new_with_attributes("Artnum", renderer, "text", 0, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(view), column);

    renderer = gtk_cell_renderer_text_new();
    column = gtk_tree_view_column_new_with_attributes("Subject", renderer, "text", 1, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(view), column);

    renderer = gtk_cell_renderer_text_new();
    column = gtk_tree_view_column_new_with_attributes("Author", renderer, "text", 2, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(view), column);

    renderer = gtk_cell_renderer_text_new();
    column = gtk_tree_view_column_new_with_attributes("Date", renderer, "text", 3, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(view), column);

    return view;
}

static GtkListStore *load_sqlite(const char *db_path, const char *group) {
    sqlite3 *db = NULL; GtkListStore *store = gtk_list_store_new(4, G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    if (sqlite3_open(db_path, &db) != SQLITE_OK) return store;
    const char *sql = "SELECT artnum, subject, author, date FROM articles WHERE group_name=? ORDER BY artnum LIMIT 1000";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, group, -1, SQLITE_TRANSIENT);
        while (sqlite3_step(st) == SQLITE_ROW) {
            GtkTreeIter iter; gtk_list_store_append(store, &iter);
            gtk_list_store_set(store, &iter,
                0, sqlite3_column_int(st, 0),
                1, sqlite3_column_text(st, 1) ? (const char*)sqlite3_column_text(st,1) : "",
                2, sqlite3_column_text(st, 2) ? (const char*)sqlite3_column_text(st,2) : "",
                3, sqlite3_column_text(st, 3) ? (const char*)sqlite3_column_text(st,3) : "",
                -1);
        }
    }
    if (st) sqlite3_finalize(st);
    if (db) sqlite3_close(db);
    return store;
}

int main(int argc, char **argv) {
    gtk_init(&argc, &argv);
    const char *db_path = NULL; const char *group = NULL;
    if (argc >= 3 && strcmp(argv[1], "--sqlite") == 0) {
        db_path = argv[2];
        group = argc >= 5 && strcmp(argv[3], "--group") == 0 ? argv[4] : NULL;
    }
    if (!db_path || !group) {
        g_printerr("Usage: viewer --sqlite DBPATH --group GROUP\n");
        return 2;
    }

    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), "nntp2sql viewer (beta .3)");
    gtk_window_set_default_size(GTK_WINDOW(win), 900, 600);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    GtkWidget *view = create_list_view();
    GtkListStore *store = load_sqlite(db_path, group);
    gtk_tree_view_set_model(GTK_TREE_VIEW(view), GTK_TREE_MODEL(store));
    g_object_unref(store);

    gtk_container_add(GTK_CONTAINER(scroll), view);
    gtk_container_add(GTK_CONTAINER(win), scroll);

    g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    gtk_widget_show_all(win);
    gtk_main();
    return 0;
}
