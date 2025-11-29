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
