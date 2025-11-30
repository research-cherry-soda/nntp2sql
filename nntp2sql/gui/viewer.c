#include <gtk/gtk.h>
#include <sqlite3.h>
#include <mysql/mysql.h>
#ifdef HAVE_PQ
#include <libpq-fe.h>
#endif

static GtkWidget *entry;
static GListStore *store;
typedef enum { V_SQLITE, V_MYSQL, V_PG } VDbType;
static VDbType vtype;
static sqlite3 *sdb;
static MYSQL *mydb;
#ifdef HAVE_PQ
static PGconn *pgdb;
#endif

typedef struct {
    int artnum;
    char *subject;
    char *author;
    char *date;
} Row;

static void row_free(Row *r){
    if (!r) return;
    g_free(r->subject);
    g_free(r->author);
    g_free(r->date);
    g_free(r);
}

static void store_clear(){
    guint n = g_list_model_get_n_items(G_LIST_MODEL(store));
    for (guint i = 0; i < n; i++){
        Row *r = g_list_model_get_item(G_LIST_MODEL(store), 0);
        row_free(r);
        g_list_store_remove(store, 0);
    }
}

static void load_rows_sqlite(sqlite3 *db, const char *filter){
    store_clear();
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
        Row *r = g_new0(Row, 1);
        r->artnum = sqlite3_column_int(st, 0);
        r->subject = g_strdup((const char*)sqlite3_column_text(st, 1));
        r->author = g_strdup((const char*)sqlite3_column_text(st, 2));
        r->date = g_strdup((const char*)sqlite3_column_text(st, 3));
        g_list_store_append(store, r);
    }
    sqlite3_finalize(st);
}

static void load_rows_mysql(MYSQL *db, const char *filter){
    store_clear();
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
        Row *r = g_new0(Row, 1);
        r->artnum = row[0] ? atoi(row[0]) : 0;
        r->subject = g_strdup(row[1] ? row[1] : "");
        r->author = g_strdup(row[2] ? row[2] : "");
        r->date = g_strdup(row[3] ? row[3] : "");
        g_list_store_append(store, r);
    }
    mysql_free_result(res);
}

#ifdef HAVE_PQ
static void load_rows_pg(PGconn *db, const char *filter){
    store_clear();
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
        Row *r = g_new0(Row, 1);
        r->artnum = atoi(PQgetvalue(res, i, 0));
        r->subject = g_strdup(PQgetvalue(res, i, 1));
        r->author = g_strdup(PQgetvalue(res, i, 2));
        r->date = g_strdup(PQgetvalue(res, i, 3));
        g_list_store_append(store, r);
    }
    PQclear(res);
}
#endif

static void on_search(GtkEditable *e, gpointer data){
    const char *txt = gtk_editable_get_text(GTK_EDITABLE(entry));
    if (vtype == V_SQLITE) load_rows_sqlite(sdb, txt);
    else if (vtype == V_MYSQL) load_rows_mysql(mydb, txt);
    #ifdef HAVE_PQ
    else if (vtype == V_PG) load_rows_pg(pgdb, txt);
    #endif
}

static GMainLoop *app_loop = NULL;
static gboolean on_close(GtkWindow *win, gpointer data){
    if (app_loop){ g_main_loop_quit(app_loop); }
    return TRUE; /* prevent default destroy, we'll quit loop */
}

/* GtkListItemFactory callbacks */
static void setup_cb(GtkSignalListItemFactory *f, GtkListItem *item, gpointer u){
    GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *l_art = gtk_label_new("");
    GtkWidget *l_subject = gtk_label_new("");
    GtkWidget *l_author = gtk_label_new("");
    GtkWidget *l_date = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(l_subject), 0.0);
    gtk_box_append(GTK_BOX(h), l_art);
    gtk_box_append(GTK_BOX(h), l_subject);
    gtk_box_append(GTK_BOX(h), l_author);
    gtk_box_append(GTK_BOX(h), l_date);
    gtk_list_item_set_child(item, h);
    g_object_set_data(G_OBJECT(item), "l_art", l_art);
    g_object_set_data(G_OBJECT(item), "l_subject", l_subject);
    g_object_set_data(G_OBJECT(item), "l_author", l_author);
    g_object_set_data(G_OBJECT(item), "l_date", l_date);
}

static void bind_cb(GtkSignalListItemFactory *f, GtkListItem *item, gpointer u){
    Row *r = (Row*)gtk_list_item_get_item(item);
    GtkWidget *l_art = GTK_WIDGET(g_object_get_data(G_OBJECT(item), "l_art"));
    GtkWidget *l_subject = GTK_WIDGET(g_object_get_data(G_OBJECT(item), "l_subject"));
    GtkWidget *l_author = GTK_WIDGET(g_object_get_data(G_OBJECT(item), "l_author"));
    GtkWidget *l_date = GTK_WIDGET(g_object_get_data(G_OBJECT(item), "l_date"));
    char buf[32]; snprintf(buf, sizeof(buf), "%d", r ? r->artnum : 0);
    gtk_label_set_text(GTK_LABEL(l_art), buf);
    gtk_label_set_text(GTK_LABEL(l_subject), (r && r->subject) ? r->subject : "");
    gtk_label_set_text(GTK_LABEL(l_author), (r && r->author) ? r->author : "");
    gtk_label_set_text(GTK_LABEL(l_date), (r && r->date) ? r->date : "");
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
    #ifdef HAVE_PQ
    else if (strcmp(type, "postgres") == 0){ vtype = V_PG; pgdb = PQconnectdb(conn); if (PQstatus(pgdb) != CONNECTION_OK){ g_printerr("postgres connect failed: %s\n", PQerrorMessage(pgdb)); return 1; } }
    #endif
    else { g_printerr("unknown db type\n"); return 2; }
    gtk_init();
    GtkWidget *win = gtk_window_new(); gtk_window_set_title(GTK_WINDOW(win), "NNTP Viewer"); gtk_window_set_default_size(GTK_WINDOW(win), 800, 480);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_window_set_child(GTK_WINDOW(win), box);
    entry = gtk_entry_new(); gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "Search subject/author...");
    gtk_box_append(GTK_BOX(box), entry);
    g_signal_connect(entry, "changed", G_CALLBACK(on_search), NULL);
    store = g_list_store_new(G_TYPE_POINTER);

    GtkListItemFactory *factory = gtk_signal_list_item_factory_new();

    g_signal_connect(factory, "setup", G_CALLBACK(setup_cb), NULL);
    g_signal_connect(factory, "bind", G_CALLBACK(bind_cb), NULL);

    GtkSelectionModel *sel = GTK_SELECTION_MODEL(gtk_no_selection_new(G_LIST_MODEL(store)));
    GtkWidget *view = gtk_list_view_new(sel, factory);

    GtkWidget *sw = gtk_scrolled_window_new(); gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), view);
    gtk_box_append(GTK_BOX(box), sw);
    if (vtype == V_SQLITE) load_rows_sqlite(sdb, NULL);
    else if (vtype == V_MYSQL) load_rows_mysql(mydb, NULL);
    #ifdef HAVE_PQ
    else if (vtype == V_PG) load_rows_pg(pgdb, NULL);
    #endif
    g_signal_connect(win, "close-request", G_CALLBACK(on_close), NULL);
    gtk_widget_set_visible(win, TRUE);
    app_loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(app_loop);
    g_main_loop_unref(app_loop);
    if (vtype == V_SQLITE) sqlite3_close(sdb);
    else if (vtype == V_MYSQL) mysql_close(mydb);
    #ifdef HAVE_PQ
    else if (vtype == V_PG) PQfinish(pgdb);
    #endif
    return 0;
}
