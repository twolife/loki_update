/*
    Loki_Update - A tool for updating Loki products over the Internet
    GTK 4 user interface
*/

#include <sys/types.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <gtk/gtk.h>

#include "update_ui.h"
#include "prefpath.h"
#include "patchset.h"
#include "load_products.h"
#include "load_patchset.h"
#include "url_paths.h"
#include "get_url.h"
#include "md5.h"
#include "gpg_verify.h"
#include "update.h"
#include "log_output.h"
#include "safe_malloc.h"

static patchset *update_patchset;
static version_node *update_root;
static patch_path *update_path;
static patch *update_patch;
static char readme_file[PATH_MAX];
static char update_url[PATH_MAX];

/* The different notebook pages for the loki_update UI */
enum {
    PRODUCT_PAGE,
    GETLIST_PAGE,
    SELECT_PAGE,
    UPDATE_PAGE
};

/* Static variables used for this UI */
static int update_status = 0;
static int one_product = 0;
static enum {
    FULLY_AUTOMATIC,
    SEMI_AUTOMATIC,
    FULLY_INTERACTIVE
} interactive = FULLY_INTERACTIVE;
static patchset *product_patchset = NULL;
static int update_proceeding = 0;
static int download_pending = 0;
static int switch_mirror = 0;
static int download_cancelled = 0;
static GMainLoop *main_loop = NULL;

struct download_update_info {
    GtkWidget *status;
    GtkWidget *progress;
    GtkWidget *rate_label;
    GtkWidget *eta_label;
    float last_rate;
    time_t last_update;
};
#define MAX_RATE_CHANGE 50.0f

/* Named widgets replacing glade_xml_get_widget(). */
typedef struct UiWidgets {
    GtkWidget *window;
    GtkWidget *update_notebook;

    GtkWidget *product_vbox;
    GtkWidget *product_continue_button;

    GtkWidget *product_label;
    GtkWidget *list_status_label;
    GtkWidget *list_status_arrow;
    GtkWidget *list_download_pixmap;
    GtkWidget *update_list_progress;
    GtkWidget *list_rate_label;
    GtkWidget *list_eta_label;
    GtkWidget *list_cancel_button;
    GtkWidget *list_done_button;
    GtkWidget *list_details_button;

    GtkWidget *update_vbox;
    GtkWidget *choose_continue_button;
    GtkWidget *auto_update_toggle;
    GtkWidget *estimated_size_label;

    GtkWidget *update_name_label;
    GtkWidget *update_size_label;
    GtkWidget *update_status_label;
    GtkWidget *verify_status_label;
    GtkWidget *gpg_status_label;
    GtkWidget *update_status_arrow;
    GtkWidget *update_download_pixmap;
    GtkWidget *update_verify_pixmap;
    GtkWidget *update_patch_pixmap;
    GtkWidget *update_download_label;
    GtkWidget *update_download_progress;
    GtkWidget *update_patch_progress;
    GtkWidget *update_rate_label;
    GtkWidget *update_eta_label;
    GtkWidget *update_readme_button;
    GtkWidget *gpg_details_button;
    GtkWidget *choose_mirror_button;
    GtkWidget *switch_mirror_button;
    GtkWidget *save_mirror_button;
    GtkWidget *update_details_button;
    GtkWidget *update_action_button;
    GtkWidget *update_cancel_button;

    GtkWidget *readme_dialog;
    GtkWidget *readme_area;
    GtkWidget *details_dialog;
    GtkWidget *update_details_text;
    GtkWidget *save_details_button;
    GtkWidget *gpg_dialog;
    GtkWidget *gpg_details_text;
    GtkWidget *mirrors_dialog;
    GtkWidget *mirrors_vbox;
} UiWidgets;

static UiWidgets ui;
static GPtrArray *product_buttons = NULL;
static GPtrArray *update_buttons = NULL;
static guint flash_id = 0;

struct image {
    const char *file;
};
static struct image balls[] = {
    { DATADIR "/pixmaps/gray_ball.png" },
    { DATADIR "/pixmaps/green_ball.png" },
    { DATADIR "/pixmaps/check_ball.png" },
    { DATADIR "/pixmaps/yellow_ball.png" },
    { DATADIR "/pixmaps/red_ball.png" }
};
static struct image arrows[] = {
    { DATADIR "/pixmaps/gray_arrow.png" },
    { DATADIR "/pixmaps/green_arrow.png" },
    { DATADIR "/pixmaps/yellow_arrow.png" },
    { DATADIR "/pixmaps/red_arrow.png" }
};

void download_update_slot(GtkWidget *w, gpointer data);
void perform_update_slot(GtkWidget *w, gpointer data);
void action_button_slot(GtkWidget *w, gpointer data);

static void iterate_events(void)
{
    GMainContext *ctx = g_main_context_default();
    while (g_main_context_pending(ctx)) {
        g_main_context_iteration(ctx, FALSE);
    }
}

static void gtk_button_set_text(GtkButton *button, const char *text)
{
    gtk_button_set_label(button, text);
}

static void gtk_button_set_sensitive(GtkWidget *button, gboolean sensitive)
{
    if (button) gtk_widget_set_sensitive(button, sensitive);
}

static void clear_box(GtkWidget *box)
{
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(box)) != NULL) {
        gtk_box_remove(GTK_BOX(box), child);
    }
}

static void set_image_file(GtkWidget *image, const char *file)
{
    if (image) {
        gtk_image_set_from_file(GTK_IMAGE(image), file);
        gtk_widget_set_visible(image, TRUE);
    }
}

static GtkWidget *new_image_from_status(const struct image *img)
{
    GtkWidget *w = gtk_image_new_from_file(img->file);
    gtk_widget_set_size_request(w, 18, 18);
    return w;
}

static GtkWidget *new_text_view(gboolean editable)
{
    GtkWidget *view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(view), editable);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(view), editable);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), GTK_WRAP_WORD_CHAR);
    gtk_widget_set_hexpand(view, TRUE);
    gtk_widget_set_vexpand(view, TRUE);
    return view;
}

static void text_view_clear(GtkWidget *view)
{
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
    gtk_text_buffer_set_text(buf, "", -1);
}

static void text_view_append(GtkWidget *view, const char *text)
{
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(buf, &end);
    gtk_text_buffer_insert(buf, &end, text, -1);
}

static char *text_view_get_all(GtkWidget *view)
{
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buf, &start, &end);
    return gtk_text_buffer_get_text(buf, &start, &end, FALSE);
}

static GtkWidget *scrolled(GtkWidget *child)
{
    GtkWidget *sw = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), child);
    gtk_widget_set_hexpand(sw, TRUE);
    gtk_widget_set_vexpand(sw, TRUE);
    return sw;
}

static GtkWidget *row_with_icon(GtkWidget *icon, GtkWidget *label)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append(GTK_BOX(box), icon);
    gtk_box_append(GTK_BOX(box), label);
    gtk_widget_set_hexpand(label, FALSE);
    return box;
}

static void remove_readme(void)
{
    if (readme_file[0]) {
        unlink(readme_file);
        readme_file[0] = '\0';
    }
    gtk_button_set_sensitive(ui.update_readme_button, FALSE);
}

static void remove_update(void)
{
    if (update_url[0]) {
        unlink(update_url);
        update_url[0] = '\0';
    }
}

static char *quickupdate_file(char *path, int maxlen)
{
    preferences_path("quick-update.txt", path, maxlen);
    return path;
}

static void save_interactive(int value)
{
    char path[PATH_MAX];
    FILE *fp = fopen(quickupdate_file(path, sizeof(path)), "w");
    if (fp) {
        fprintf(fp, "%d\n", value);
        fclose(fp);
    }
}

static int load_interactive(void)
{
    int value = SEMI_AUTOMATIC;
    char path[PATH_MAX];
    FILE *fp = fopen(quickupdate_file(path, sizeof(path)), "r");
    if (fp) {
        if (fgets(path, sizeof(path), fp)) {
            path[strlen(path) - 1] = '\0';
            value = atoi(path);
        }
        fclose(fp);
    }
    return value;
}

void main_cancel_slot(GtkWidget *w, gpointer data)
{
    download_cancelled = 1;
    if (main_loop) g_main_loop_quit(main_loop);
}

static gboolean on_window_close(GtkWindow *window, gpointer data)
{
    main_cancel_slot(NULL, NULL);
    return TRUE;
}

void select_all_products_slot(GtkWidget *w, gpointer data)
{
    for (guint i = 0; product_buttons && i < product_buttons->len; i++) {
        GtkWidget *button = g_ptr_array_index(product_buttons, i);
        const char *product_name = g_object_get_data(G_OBJECT(button), "data");
        if (product_name && strcasecmp(PRODUCT, product_name) != 0) {
            gtk_check_button_set_active(GTK_CHECK_BUTTON(button), TRUE);
        }
    }
}

static void select_product(const char *product)
{
    for (guint i = 0; product_buttons && i < product_buttons->len; i++) {
        GtkWidget *button = g_ptr_array_index(product_buttons, i);
        if (product) {
            const char *product_name = g_object_get_data(G_OBJECT(button), "data");
            if (product_name && strcasecmp(product, product_name) == 0) {
                gtk_check_button_set_active(GTK_CHECK_BUTTON(button), TRUE);
                break;
            }
        } else {
            gtk_check_button_set_active(GTK_CHECK_BUTTON(button), FALSE);
        }
    }
}

static void deselect_product(void)
{
    for (guint i = 0; product_buttons && i < product_buttons->len; i++) {
        GtkWidget *button = g_ptr_array_index(product_buttons, i);
        if (gtk_check_button_get_active(GTK_CHECK_BUTTON(button))) {
            gtk_check_button_set_active(GTK_CHECK_BUTTON(button), FALSE);
            break;
        }
    }
}

static const char *selected_product(void)
{
    for (guint i = 0; product_buttons && i < product_buttons->len; i++) {
        GtkWidget *button = g_ptr_array_index(product_buttons, i);
        if (gtk_check_button_get_active(GTK_CHECK_BUTTON(button))) {
            return g_object_get_data(G_OBJECT(button), "data");
        }
    }
    return NULL;
}

void choose_product_slot(GtkWidget *w, gpointer data)
{
    if (product_patchset) {
        free_patchset(product_patchset);
        product_patchset = NULL;
    }
    gtk_notebook_set_current_page(GTK_NOTEBOOK(ui.update_notebook), PRODUCT_PAGE);
    gtk_window_present(GTK_WINDOW(ui.window));
    select_product(NULL);
}

void main_menu_slot(GtkWidget *w, gpointer data)
{
    if (one_product) main_cancel_slot(NULL, NULL);
    else choose_product_slot(NULL, NULL);
}

static void update_arrows(int which, int status)
{
    GtkWidget *widgets[] = { ui.list_status_arrow, ui.update_status_arrow };
    if (which >= 0 && which < 2 && status >= 0 && status < 4) {
        set_image_file(widgets[which], arrows[status].file);
    }
}

static struct flash {
    int which;
    int color;
    int colored;
} flash_data;

static gboolean flash_arrow(gpointer data)
{
    struct flash *flash = data;
    if (flash->colored) {
        update_arrows(flash->which, flash->color);
        flash->colored = 0;
    } else {
        update_arrows(flash->which, 0);
        flash->colored = 1;
    }
    return G_SOURCE_CONTINUE;
}

static void start_flash(int which, int status)
{
    update_arrows(which, 0);
    flash_data.which = which;
    flash_data.color = status;
    flash_data.colored = 0;
    if (flash_id) g_source_remove(flash_id);
    flash_id = g_timeout_add(500, flash_arrow, &flash_data);
}

static void stop_flash(void)
{
    if (flash_id) {
        g_source_remove(flash_id);
        flash_id = 0;
    }
}

static void update_balls(int which, int status)
{
    GtkWidget *widgets[] = {
        ui.list_download_pixmap,
        ui.update_download_pixmap,
        ui.update_verify_pixmap,
        ui.update_patch_pixmap
    };
    if (which < 0) {
        for (int i = 0; i < 4; i++) update_balls(i, status);
        return;
    }
    if (which >= 0 && which < 4 && status >= 0 && status < 5) {
        set_image_file(widgets[which], balls[status].file);
    }
}

static gboolean load_file(GtkWidget *view, const char *file)
{
    FILE *fp;
    text_view_clear(view);
    fp = fopen(file, "r");
    if (fp) {
        char line[BUFSIZ];
        while (fgets(line, BUFSIZ - 1, fp)) text_view_append(view, line);
        fclose(fp);
    }
    return fp != NULL;
}

void view_readme_slot(GtkWidget *w, gpointer data)
{
    if (readme_file[0] && ui.readme_dialog && ui.readme_area) {
        load_file(ui.readme_area, readme_file);
        gtk_window_present(GTK_WINDOW(ui.readme_dialog));
        gtk_button_set_sensitive(ui.update_readme_button, FALSE);
    }
}

void close_readme_slot(GtkWidget *w, gpointer data)
{
    if (ui.readme_dialog) gtk_widget_set_visible(ui.readme_dialog, FALSE);
    gtk_button_set_sensitive(ui.update_readme_button, TRUE);
}

static void clear_details_text(void)
{
    if (ui.update_details_text) text_view_clear(ui.update_details_text);
}

static void add_details_text(int level, const char *text)
{
    if (ui.update_details_text) {
        if (level == LOG_ERROR) text_view_append(ui.update_details_text, "ERROR: ");
        else if (level == LOG_WARNING) text_view_append(ui.update_details_text, "WARNING: ");
        if (level > LOG_DEBUG) text_view_append(ui.update_details_text, text);
    }
    lokilog(level, "%s", text);
}

static void save_dialog_response(GtkNativeDialog *dialog, int response, gpointer data)
{
    if (response == GTK_RESPONSE_ACCEPT) {
        GtkFileChooser *chooser = GTK_FILE_CHOOSER(dialog);
        GFile *file = gtk_file_chooser_get_file(chooser);
        if (file) {
            char *path = g_file_get_path(file);
            if (path && *path) {
                FILE *fp = fopen(path, "w");
                if (fp) {
                    char *text = text_view_get_all(ui.update_details_text);
                    fputs(text, fp);
                    g_free(text);
                    fclose(fp);
                } else {
                    char message[PATH_MAX];
                    snprintf(message, sizeof(message), "Unable to write to %s\n", path);
                    add_details_text(LOG_WARNING, message);
                }
            }
            g_free(path);
            g_object_unref(file);
        }
    }
    gtk_button_set_sensitive(ui.save_details_button, TRUE);
    g_object_unref(dialog);
}

static void open_save_details(void)
{
    GtkFileChooserNative *dialog;
    GFile *folder;
    dialog = gtk_file_chooser_native_new(_("Save Details"),
        GTK_WINDOW(ui.details_dialog), GTK_FILE_CHOOSER_ACTION_SAVE,
        _("Save"), _("Cancel"));
    folder = g_file_new_for_path(get_working_path());
    gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog), folder, NULL);
    g_object_unref(folder);
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog), "loki-update-details.txt");
    g_signal_connect(dialog, "response", G_CALLBACK(save_dialog_response), NULL);
    gtk_button_set_sensitive(ui.save_details_button, FALSE);
    gtk_native_dialog_show(GTK_NATIVE_DIALOG(dialog));
}

static void close_save_details(void) { gtk_button_set_sensitive(ui.save_details_button, TRUE); }
void save_details_slot(GtkWidget *w, gpointer data) { open_save_details(); }
void perform_save_slot(GtkWidget *w, gpointer data) { (void)w; (void)data; }
void cancel_save_slot(GtkWidget *w, gpointer data) { close_save_details(); }

void view_details_slot(GtkWidget *w, gpointer data)
{
    gtk_window_present(GTK_WINDOW(ui.details_dialog));
    gtk_button_set_sensitive(ui.list_details_button, FALSE);
    gtk_button_set_sensitive(ui.update_details_button, FALSE);
}

void close_details_slot(GtkWidget *w, gpointer data)
{
    gtk_widget_set_visible(ui.details_dialog, FALSE);
    gtk_button_set_sensitive(ui.list_details_button, TRUE);
    gtk_button_set_sensitive(ui.update_details_button, TRUE);
    close_save_details();
}

void view_gpg_details_slot(GtkWidget *w, gpointer data)
{
    gtk_window_present(GTK_WINDOW(ui.gpg_dialog));
    gtk_button_set_sensitive(ui.gpg_details_button, FALSE);
}

void close_gpg_details_slot(GtkWidget *w, gpointer data)
{
    gtk_widget_set_visible(ui.gpg_dialog, FALSE);
    gtk_button_set_sensitive(ui.gpg_details_button, TRUE);
}

static void enable_gpg_details(const char *url, char *sig)
{
    char *file;
    char *signature;
    char *fingerprint;
    char text[1024];

    if (!ui.gpg_details_text) return;
    text_view_clear(ui.gpg_details_text);
    file = strrchr(url, '/') + 1;
    fingerprint = sig;
    signature = strchr(sig, ' ');
    if (!signature) signature = sig;
    else *signature++ = '\0';
    snprintf(text, sizeof(text), "%s\nSigned by %s\nGPG Fingerprint: ", file, signature);
    while (*fingerprint && (strlen(text) + 8 < sizeof(text))) {
        strncat(text, fingerprint, 4);
        strcat(text, " ");
        fingerprint += 4;
    }
    strcat(text, "\n");
    text_view_append(ui.gpg_details_text, text);
    gtk_button_set_sensitive(ui.gpg_details_button, TRUE);
}

static void fill_mirrors_list(urlset *mirrors)
{
    struct mirror_url *entry;
    GtkWidget *first_radio = NULL;
    char host[PATH_MAX];

    if (!ui.mirrors_vbox) return;
    clear_box(ui.mirrors_vbox);

    for (entry = mirrors->list; entry; entry = entry->next) {
        if (get_url_host(entry->url, host, sizeof(host))) {
            GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
            GtkWidget *icon = new_image_from_status(entry->status == URL_OK ? &balls[1] : &balls[4]);
            GtkWidget *radio = gtk_check_button_new_with_label(host);
            if (first_radio)
                gtk_check_button_set_group(GTK_CHECK_BUTTON(radio), GTK_CHECK_BUTTON(first_radio));
            else
                first_radio = radio;
            gtk_box_append(GTK_BOX(hbox), icon);
            gtk_box_append(GTK_BOX(hbox), radio);
            if (entry == mirrors->current)
                gtk_check_button_set_active(GTK_CHECK_BUTTON(radio), TRUE);
            gtk_widget_set_sensitive(radio, entry->status == URL_OK);
            if (mirrors->preferred_site && strcasecmp(host, mirrors->preferred_site) == 0) {
                gtk_box_append(GTK_BOX(hbox), gtk_label_new(_(" (preferred)")));
            }
            gtk_box_append(GTK_BOX(ui.mirrors_vbox), hbox);
            entry->data = hbox;
        } else {
            entry->data = NULL;
        }
    }
}

static GtkWidget *mirror_radio_from_row(GtkWidget *hbox)
{
    GtkWidget *child = gtk_widget_get_first_child(hbox); /* icon */
    return child ? gtk_widget_get_next_sibling(child) : NULL;
}

static GtkWidget *mirror_icon_from_row(GtkWidget *hbox)
{
    return gtk_widget_get_first_child(hbox);
}

static void select_current_mirror(urlset *mirrors)
{
    GtkWidget *hbox = mirrors && mirrors->current ? mirrors->current->data : NULL;
    GtkWidget *radio = hbox ? mirror_radio_from_row(hbox) : NULL;
    if (radio) gtk_check_button_set_active(GTK_CHECK_BUTTON(radio), TRUE);
}

static void failed_current_mirror(urlset *mirrors)
{
    GtkWidget *hbox = mirrors && mirrors->current ? mirrors->current->data : NULL;
    if (hbox) {
        GtkWidget *icon = mirror_icon_from_row(hbox);
        GtkWidget *radio = mirror_radio_from_row(hbox);
        set_image_file(icon, balls[4].file);
        if (radio) gtk_widget_set_sensitive(radio, FALSE);
    }
    set_url_status(mirrors, URL_FAILED);
}

void show_mirrors_slot(GtkWidget *w, gpointer data) { gtk_window_present(GTK_WINDOW(ui.mirrors_dialog)); }
void close_mirrors_slot(GtkWidget *w, gpointer data) { gtk_widget_set_visible(ui.mirrors_dialog, FALSE); }

void choose_mirror_slot(GtkWidget *w, gpointer data)
{
    urlset *mirrors = update_patchset->mirrors;
    struct mirror_url *entry, *stop, *prev;
    GtkWidget *hbox, *radio;

    entry = mirrors->current;
    hbox = entry->data;
    radio = hbox ? mirror_radio_from_row(hbox) : NULL;
    if (radio && gtk_check_button_get_active(GTK_CHECK_BUTTON(radio))) return;

    stop = entry;
    do {
        prev = entry;
        entry = entry->next ? entry->next : mirrors->list;
        hbox = entry->data;
        radio = hbox ? mirror_radio_from_row(hbox) : NULL;
        if (radio && gtk_check_button_get_active(GTK_CHECK_BUTTON(radio))) {
            switch_mirror = 1;
            mirrors->current = prev;
            break;
        }
    } while (entry != stop);
}

void switch_mirror_slot(GtkWidget *w, gpointer data) { switch_mirror = 1; }

void save_mirror_slot(GtkWidget *w, gpointer data)
{
    patchset *patchset;
    if (update_patchset) {
        for (patchset = product_patchset; patchset; patchset = patchset->next) {
            set_preferred_url(patchset->mirrors, update_patchset->mirrors->current->url);
        }
        save_preferred_url(update_patchset->mirrors);
        fill_mirrors_list(update_patchset->mirrors);
    }
}

static void mirror_buttons_sensitive(gboolean sensitive)
{
    gtk_widget_set_sensitive(ui.choose_mirror_button, sensitive);
    gtk_widget_set_sensitive(ui.switch_mirror_button, sensitive);
    gtk_widget_set_sensitive(ui.save_mirror_button, sensitive);
    if (!sensitive) close_mirrors_slot(NULL, NULL);
}

void cancel_download_slot(GtkWidget *w, gpointer data) { download_cancelled = 1; }

void update_proceed_slot(GtkWidget *w, gpointer data)
{
    stop_flash();
    if (update_proceeding) main_menu_slot(NULL, NULL);
    else update_proceeding = 1;
}

static void set_download_info(struct download_update_info *info,
                              GtkWidget *status,
                              GtkWidget *progress,
                              GtkWidget *rate_label,
                              GtkWidget *eta_label)
{
    info->status = status;
    info->progress = progress;
    info->rate_label = rate_label;
    info->eta_label = eta_label;
    info->last_rate = 0.0f;
    info->last_update = 0;
    switch_mirror = 0;
    download_cancelled = 0;
}

static void set_status_message(GtkWidget *status_label, const char *text)
{
    char *text_buf;
    if (status_label) gtk_label_set_text(GTK_LABEL(status_label), text);
    text_buf = safe_malloc(strlen(text) + 2);
    sprintf(text_buf, "%s\n", text);
    add_details_text(LOG_STATUS, text_buf);
    free(text_buf);
}

static int download_update(int status_level, const char *status,
                           float percentage,
                           int size, int total, float rate, void *udata)
{
    struct download_update_info *info = udata;
    if (status) {
        if (status_level == LOG_STATUS) set_status_message(info->status, status);
        else add_details_text(status_level, status);
    }
    if (info->progress && percentage) {
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(info->progress), percentage / 100.0);
    }
    if (rate > 0.01f) {
        float last_rate = info->last_rate;
        info->last_rate = rate;
        if ((rate < last_rate && (rate - last_rate) > MAX_RATE_CHANGE) ||
            (rate >= last_rate && (last_rate - rate) > MAX_RATE_CHANGE)) {
            rate = 0.0f;
        }
    }
    if ((rate > 0.01f) && (time(NULL) != info->last_update)) {
        char text[128];
        if (info->rate_label) {
            sprintf(text, "%7.2f K/s ", rate);
            gtk_label_set_text(GTK_LABEL(info->rate_label), text);
        }
        if (total && info->eta_label) {
            long eta = (long)((float)(total - size) / rate);
            if (eta < (60 * 60 * 24)) {
                if (eta > (60 * 60)) {
                    sprintf(text, _("ETA: %ld:"), eta / (60 * 60));
                    eta %= (60 * 60);
                } else {
                    sprintf(text, _("ETA: "));
                }
                sprintf(&text[strlen(text)], "%2.2ld:%2.2ld", eta / 60, eta % 60);
            } else strcpy(text, _("unknown"));
            gtk_label_set_text(GTK_LABEL(info->eta_label), text);
        }
        info->last_update = time(NULL);
    }
    iterate_events();
    return download_cancelled || switch_mirror;
}

static gpg_result do_gpg_verify(const char *file, char *sig, int maxsig)
{
    gpg_result gpg_code;
    struct download_update_info info;
    GtkWidget *status = ui.update_status_label;
    set_status_message(status, _("Running GPG..."));
    iterate_events();
    set_download_info(&info, status, NULL, NULL, NULL);
    gpg_code = gpg_verify(file, sig, maxsig, download_update, &info);
    if (gpg_code == GPG_NOPUBKEY) {
        set_status_message(ui.verify_status_label, _("Downloading public key"));
        get_publickey(sig, download_update, &info);
        gpg_code = gpg_verify(file, sig, maxsig, download_update, &info);
    }
    return gpg_code;
}

static void set_progress_url(GtkWidget *progress, const char *url)
{
    char text[1024], *bufp;
    int len;
    if (!progress) return;
    bufp = strstr(url, "://");
    if (bufp) {
        bufp += 3;
        while (*bufp && (*bufp != '/')) ++bufp;
        len = (bufp - url) + 1;
        if (len >= ((int)sizeof(text) - 3)) len = sizeof(text) - 4;
        strncpy(text, url, len);
        text[len] = '\0';
        strcat(text, "...");
        if (*bufp) {
            bufp = strrchr(url, '/');
            strncat(text, bufp, sizeof(text) - strlen(text) - 1);
        }
    } else {
        strncpy(text, url, sizeof(text) - 1);
        text[sizeof(text) - 1] = '\0';
    }
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(progress), TRUE);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(progress), text);
}

static void calculate_update_size(void)
{
    int size = 0;
    patchset *patchset;
    char text[1024];
    for (patchset = product_patchset; patchset; patchset = patchset->next)
        size += (selected_size(patchset) + 1023) / 1024;
    sprintf(text, _("(%d MB)"), size);
    gtk_label_set_text(GTK_LABEL(ui.estimated_size_label), text);
}

static void reset_selected_update(void)
{
    update_patchset = product_patchset;
    update_path = NULL;
    update_patch = NULL;
    while (!update_path && update_patchset) {
        update_root = update_patchset ? update_patchset->root : NULL;
        while (update_root && !update_root->selected) update_root = update_root->sibling;
        if (update_root) {
            update_path = update_root->selected->shortest_path;
            update_patch = update_path->patch;
        } else update_patchset = update_patchset->next;
    }
    calculate_update_size();
}

static patch *skip_to_selected_update(void)
{
    if (update_patch) {
        while (update_patch->installed) {
            update_path = update_path->next;
            if (!update_path) {
                do {
                    update_root = update_root->sibling;
                    if (!update_root) {
                        update_patchset = update_patchset->next;
                        if (!update_patchset) { update_patch = NULL; return NULL; }
                        update_root = update_patchset->root;
                    }
                } while (!update_root->selected);
                update_path = update_root->selected->shortest_path;
            }
            update_patch = update_path->patch;
        }
    }
    return update_patch;
}

static void cleanup_update(const char *status_msg, int update_obsolete)
{
    close_readme_slot(NULL, NULL);
    if (update_obsolete) { remove_update(); remove_readme(); }
    update_proceeding = 0;
    select_node(update_patch->node, 0);

    if ((update_status >= 0) && skip_to_selected_update()) {
        gtk_button_set_sensitive(ui.update_cancel_button, TRUE);
        gtk_button_set_text(GTK_BUTTON(ui.update_action_button), _("Continue"));
    } else {
        gtk_button_set_sensitive(ui.update_cancel_button, FALSE);
        gtk_button_set_text(GTK_BUTTON(ui.update_action_button), _("Finished"));
    }
    gtk_button_set_sensitive(ui.update_action_button, TRUE);

    if (status_msg) set_status_message(ui.update_status_label, status_msg);
    else { main_menu_slot(NULL, NULL); return; }

    if (interactive != FULLY_INTERACTIVE && update_status >= 0) {
        stop_flash();
        download_update_slot(NULL, NULL);
    }
}

void action_button_slot(GtkWidget *w, gpointer data)
{
    stop_flash();
    if (update_proceeding) perform_update_slot(NULL, NULL);
    else {
        if (update_status < 0) main_menu_slot(NULL, NULL);
        else if (skip_to_selected_update()) download_update_slot(NULL, NULL);
        else main_cancel_slot(NULL, NULL);
    }
}

void cancel_button_slot(GtkWidget *w, gpointer data)
{
    stop_flash();
    if (download_pending) cancel_download_slot(NULL, NULL);
    else cleanup_update(NULL, 0);
}

void download_update_slot(GtkWidget *w, gpointer data)
{
    struct download_update_info info;
    GtkWidget *status, *verify, *action, *progress, *gpg_status;
    patch *patch;
    char text[1024];
    const char *url;
    char sig[1024];
    char sum_file[PATH_MAX];
    char md5_real[CHECKSUM_SIZE + 1];
    char md5_calc[CHECKSUM_SIZE + 1];
    FILE *fp;
    gboolean have_readme;
    verify_result verified;

    patch = skip_to_selected_update();
    if (!patch) { start_flash(1, 1); return; }
    patch->installed = 1;
    update_proceeding = 1;

    gtk_notebook_set_current_page(GTK_NOTEBOOK(ui.update_notebook), UPDATE_PAGE);
    status = ui.update_status_label;
    verify = ui.verify_status_label;
    action = ui.update_action_button;
    gtk_button_set_text(GTK_BUTTON(action), _("Update"));
    gtk_button_set_sensitive(action, FALSE);

    add_details_text(LOG_VERBOSE, "\n");
    snprintf(text, sizeof(text), "%s: %s",
             get_product_description(patch->patchset->product_name), patch->description);
    set_status_message(ui.update_name_label, text);
    sprintf(text, _("(%d MB)"), (patch->size + 1023) / 1024);
    gtk_label_set_text(GTK_LABEL(ui.update_size_label), text);

    update_arrows(1, 1);
    have_readme = FALSE;
    verified = DOWNLOAD_FAILED;
    download_pending = 1;
    randomize_urls(patch->patchset->mirrors);
    fill_mirrors_list(patch->patchset->mirrors);
    do {
        url = get_next_url(patch->patchset->mirrors, patch->file);
        if (!url) break;
        select_current_mirror(patch->patchset->mirrors);

        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(ui.update_download_progress), 0.0);
        gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(ui.update_download_progress), FALSE);
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(ui.update_patch_progress), 0.0);
        gtk_label_set_text(GTK_LABEL(ui.update_rate_label), "");
        gtk_label_set_text(GTK_LABEL(ui.update_eta_label), "");
        gtk_label_set_text(GTK_LABEL(status), "");
        gtk_label_set_text(GTK_LABEL(verify), "");
        gpg_status = ui.gpg_status_label;
        gtk_label_set_text(GTK_LABEL(gpg_status), "");
        gtk_button_set_sensitive(ui.update_cancel_button, TRUE);
        gtk_button_set_sensitive(ui.update_readme_button, have_readme);
        gtk_button_set_sensitive(ui.gpg_details_button, FALSE);
        update_balls(-1, 0);
        mirror_buttons_sensitive(patch->patchset->mirrors->num_okay > 1);

        if (!have_readme && (interactive != FULLY_AUTOMATIC)) {
            set_status_message(status, _("Downloading README"));
            gtk_label_set_text(GTK_LABEL(ui.update_download_label), _("Downloading README"));
            sprintf(readme_file, "%s.txt", url);
            set_download_info(&info, status, NULL, NULL, NULL);
            if (get_url(readme_file, readme_file, sizeof(readme_file), download_update, &info) == 0) {
                gtk_button_set_sensitive(ui.update_readme_button, TRUE);
                have_readme = TRUE;
            } else if (switch_mirror) continue;
        }

        update_balls(1, 1);
        set_status_message(status, _("Downloading update"));
        gtk_label_set_text(GTK_LABEL(ui.update_download_label), _("Downloading update"));
        strcpy(update_url, url);
        progress = ui.update_download_progress;
        set_progress_url(progress, update_url);
        set_download_info(&info, status, progress, ui.update_rate_label, ui.update_eta_label);
        if (get_url(update_url, update_url, sizeof(update_url), download_update, &info) != 0) {
            if (switch_mirror) continue;
            failed_current_mirror(patch->patchset->mirrors);
            update_balls(1, 4);
            verified = DOWNLOAD_FAILED;
        } else {
            mirror_buttons_sensitive(FALSE);
            update_balls(1, 2);
            verified = VERIFY_UNKNOWN;
        }

        if (verified == VERIFY_UNKNOWN) update_balls(2, 1);
        if (verified == VERIFY_UNKNOWN) {
            set_status_message(verify, _("Verifying GPG signature"));
            sprintf(sum_file, "%s.sig", url);
            set_download_info(&info, status, NULL, NULL, NULL);
            if (get_url(sum_file, sum_file, sizeof(sum_file), download_update, &info) == 0) {
                switch (do_gpg_verify(sum_file, sig, sizeof(sig))) {
                    case GPG_NOTINSTALLED: set_status_message(gpg_status, _("GPG not installed")); verified = VERIFY_UNKNOWN; break;
                    case GPG_CANCELLED: set_status_message(gpg_status, _("GPG was cancelled")); verified = VERIFY_UNKNOWN; break;
                    case GPG_NOPUBKEY: set_status_message(gpg_status, _("GPG key not available")); verified = VERIFY_UNKNOWN; break;
                    case GPG_IMPORTED: break;
                    case GPG_VERIFYFAIL: failed_current_mirror(patch->patchset->mirrors); set_status_message(gpg_status, _("GPG verify failed")); verified = VERIFY_FAILED; break;
                    case GPG_VERIFYOK: set_status_message(gpg_status, _("GPG verify succeeded")); enable_gpg_details(update_url, sig); verified = VERIFY_OK; break;
                }
            } else set_status_message(gpg_status, _("GPG signature not available"));
            unlink(sum_file);
        }
        if (verified == VERIFY_UNKNOWN) {
            set_status_message(verify, _("Verifying MD5 checksum"));
            sprintf(sum_file, "%s.md5", url);
            set_download_info(&info, status, NULL, NULL, NULL);
            if (get_url(sum_file, sum_file, sizeof(sum_file), download_update, &info) == 0) {
                fp = fopen(sum_file, "r");
                if (fp) {
                    if (fgets(md5_calc, sizeof(md5_calc), fp)) {
                        set_status_message(status, _("Calculating MD5 checksum"));
                        iterate_events();
                        md5_compute(update_url, md5_real, 0);
                        if (strcmp(md5_calc, md5_real) != 0) {
                            failed_current_mirror(patch->patchset->mirrors);
                            verified = VERIFY_FAILED;
                        }
                    }
                    fclose(fp);
                }
            } else set_status_message(verify, _("MD5 checksum not available"));
            unlink(sum_file);
        }
    } while (((verified == DOWNLOAD_FAILED) || (verified == VERIFY_FAILED)) && !download_cancelled);
    download_pending = 0;
    mirror_buttons_sensitive(FALSE);

    switch (verified) {
        case VERIFY_UNKNOWN: set_status_message(verify, _("Verification succeeded")); update_balls(2, 3); break;
        case VERIFY_OK: set_status_message(verify, _("Verification succeeded")); update_balls(2, 2); break;
        case VERIFY_FAILED: update_status = -1; start_flash(1, 3); cleanup_update(_("Update corrupted"), 1); return;
        case DOWNLOAD_FAILED: update_status = -1; start_flash(1, 3); cleanup_update(_("Unable to retrieve update"), 0); return;
    }
    start_flash(1, 1);
    set_status_message(status, _("Ready for update"));
    gtk_button_set_sensitive(action, TRUE);
    if (interactive != FULLY_INTERACTIVE) { stop_flash(); perform_update_slot(NULL, NULL); }
}

void perform_update_slot(GtkWidget *w, gpointer data)
{
    struct download_update_info info;
    update_arrows(1, 1);
    update_balls(3, 1);
    gtk_button_set_sensitive(ui.update_action_button, FALSE);
    set_status_message(ui.update_status_label, _("Performing update"));
    gtk_button_set_sensitive(ui.update_cancel_button, FALSE);
    set_download_info(&info, ui.update_status_label, ui.update_patch_progress, NULL, NULL);
    if (perform_update(update_url, get_product_root(update_patchset->product_name), download_update, &info) != 0) {
        update_balls(3, 4);
        update_status = -1;
        start_flash(1, 3);
        cleanup_update(_("Update failed"), 0);
        return;
    }
    update_balls(3, 2);
    start_flash(1, 1);
    ++update_status;
    cleanup_update(_("Update complete"), 1);
}

void select_all_updates_slot(GtkWidget *w, gpointer data)
{
    for (guint i = 0; update_buttons && i < update_buttons->len; i++) {
        GtkWidget *button = g_ptr_array_index(update_buttons, i);
        gtk_check_button_set_active(GTK_CHECK_BUTTON(button), TRUE);
    }
}

static void update_toggle_option(GtkWidget *widget, gpointer func_data)
{
    static int in_update_toggle_option = 0;
    version_node *node = func_data;
    if (in_update_toggle_option) return;
    in_update_toggle_option = 1;

    if (gtk_check_button_get_active(GTK_CHECK_BUTTON(widget))) {
        select_node(node, 1);
        gtk_button_set_sensitive(ui.choose_continue_button, TRUE);
    } else {
        patchset *patchset;
        version_node *root;
        gboolean selected = FALSE;
        select_node(node, 0);
        for (patchset = product_patchset; patchset && !selected; patchset = patchset->next)
            for (root = patchset->root; root && !selected; root = root->sibling)
                if (root->selected) selected = TRUE;
        gtk_button_set_sensitive(ui.choose_continue_button, selected);
    }
    for (node = node->root; node; node = node->next) {
        if (node->udata) gtk_check_button_set_active(GTK_CHECK_BUTTON(node->udata), node->toggled);
    }
    reset_selected_update();
    in_update_toggle_option = 0;
}

void toggle_auto_update_slot(GtkWidget *w, gpointer data)
{
    interactive = gtk_check_button_get_active(GTK_CHECK_BUTTON(w)) ? SEMI_AUTOMATIC : FULLY_INTERACTIVE;
    save_interactive(interactive);
}

void choose_update_slot(GtkWidget *w, gpointer data)
{
    struct download_update_info info;
    GtkWidget *status, *frame, *vbox, *button, *progress;
    patchset *patchset;
    const char *product_name;
    char text[1024];
    version_node *node, *root, *trunk;
    int selected;

    gtk_notebook_set_current_page(GTK_NOTEBOOK(ui.update_notebook), GETLIST_PAGE);
    gtk_window_present(GTK_WINDOW(ui.window));
    clear_box(ui.update_vbox);
    if (update_buttons) g_ptr_array_set_size(update_buttons, 0);

    status = ui.list_status_label;
    clear_details_text();
    add_details_text(LOG_VERBOSE, "\n");
    set_status_message(status, _("Listing product updates"));

    selected = 0;
    while ((product_name = selected_product()) != NULL) {
        deselect_product();
        patchset = create_patchset(product_name);
        if (!patchset) lokilog(LOG_WARNING, "Unable to open product '%s'\n", product_name);

        add_details_text(LOG_VERBOSE, "\n");
        set_status_message(ui.product_label, get_product_description(product_name));
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(ui.update_list_progress), 0.0);
        gtk_label_set_text(GTK_LABEL(ui.list_rate_label), "");
        gtk_label_set_text(GTK_LABEL(ui.list_eta_label), "");
        gtk_label_set_text(GTK_LABEL(status), "");
        gtk_button_set_sensitive(ui.list_cancel_button, TRUE);
        gtk_button_set_sensitive(ui.list_done_button, FALSE);
        gtk_button_set_text(GTK_BUTTON(ui.list_done_button), _("Continue"));

        update_arrows(0, 1);
        update_balls(0, 1);
        strcpy(update_url, get_product_url(patchset->product_name));
        progress = ui.update_list_progress;
        set_progress_url(progress, update_url);
        set_download_info(&info, status, progress, ui.list_rate_label, ui.list_eta_label);
        if (get_url(update_url, update_url, sizeof(update_url), download_update, &info) != 0) {
            remove_update();
            update_balls(0, 4);
            set_status_message(status, download_cancelled ? _("Download cancelled") : _("Unable to retrieve update list"));
            if (interactive != FULLY_AUTOMATIC) {
                gtk_button_set_sensitive(ui.list_cancel_button, FALSE);
                gtk_button_set_sensitive(ui.list_done_button, TRUE);
                update_proceeding = 0;
                start_flash(0, 3);
                do { g_main_context_iteration(g_main_context_default(), TRUE); } while (!update_proceeding);
            }
            free_patchset(patchset);
            continue;
        }
        set_status_message(status, _("Retrieved update list"));
        update_arrows(0, 1);
        update_balls(0, 2);
        gtk_button_set_sensitive(ui.list_cancel_button, FALSE);
        iterate_events();

        load_patchset(patchset, update_url);
        remove_update();
        if (!patchset->patches) { free_patchset(patchset); continue; }

        snprintf(text, sizeof(text), "%s %s", get_product_description(product_name), get_product_version(product_name));
        frame = gtk_frame_new(text);
        gtk_widget_set_margin_top(frame, 4);
        gtk_widget_set_margin_bottom(frame, 4);
        gtk_widget_set_margin_start(frame, 4);
        gtk_widget_set_margin_end(frame, 4);
        gtk_box_append(GTK_BOX(ui.update_vbox), frame);
        vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_frame_set_child(GTK_FRAME(frame), vbox);

        for (root = patchset->root; root; root = root->sibling) {
            for (trunk = root; trunk; trunk = trunk->child) {
                for (node = trunk; node; node = (node == root) ? NULL : node->sibling) {
                    if (node->invisible) continue;
                    strncpy(text, node->description, sizeof(text) - 1);
                    text[sizeof(text) - 1] = '\0';
                    if (node->note) {
                        int textlen = strlen(text);
                        snprintf(&text[textlen], sizeof(text) - textlen, " (%s)", node->note);
                    }
                    button = gtk_check_button_new_with_label(text);
                    gtk_check_button_set_active(GTK_CHECK_BUTTON(button), node->toggled);
                    if (node->toggled) ++selected;
                    gtk_box_append(GTK_BOX(vbox), button);
                    g_signal_connect(button, "toggled", G_CALLBACK(update_toggle_option), node);
                    sprintf(text, "%d MB", (node->shortest_path->size + 1023) / 1024);
                    gtk_widget_set_tooltip_text(button, text);
                    node->udata = button;
                    g_ptr_array_add(update_buttons, button);
                }
            }
        }
        patchset->next = product_patchset;
        product_patchset = patchset;
    }

    reset_selected_update();
    if (!product_patchset || ((interactive == FULLY_AUTOMATIC) && !update_patch)) {
        set_status_message(status, _("No new updates available"));
        update_proceeding = 1;
        if (interactive != FULLY_AUTOMATIC) start_flash(0, 1);
        gtk_button_set_text(GTK_BUTTON(ui.list_done_button), _("Finished"));
        gtk_button_set_sensitive(ui.list_done_button, TRUE);
    } else {
        gtk_notebook_set_current_page(GTK_NOTEBOOK(ui.update_notebook), SELECT_PAGE);
        gtk_button_set_text(GTK_BUTTON(ui.choose_continue_button), _("Continue"));
        gtk_button_set_sensitive(ui.choose_continue_button, selected ? TRUE : FALSE);
        if (interactive != FULLY_AUTOMATIC) {
            gtk_check_button_set_active(GTK_CHECK_BUTTON(ui.auto_update_toggle), interactive == SEMI_AUTOMATIC);
        }
        if (interactive == FULLY_AUTOMATIC) download_update_slot(NULL, NULL);
    }
}

static void product_toggle_option(GtkWidget *widget, gpointer func_data)
{
    gboolean enabled = FALSE;
    for (guint i = 0; product_buttons && i < product_buttons->len; i++) {
        GtkWidget *button = g_ptr_array_index(product_buttons, i);
        if (gtk_check_button_get_active(GTK_CHECK_BUTTON(button))) enabled = TRUE;
    }
    gtk_button_set_sensitive(ui.product_continue_button, enabled);
}

static GtkWidget *make_button(const char *text, GCallback cb)
{
    GtkWidget *button = gtk_button_new_with_label(text);
    if (cb) g_signal_connect(button, "clicked", cb, NULL);
    return button;
}

static GtkWidget *make_page_box(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(box, 12);
    gtk_widget_set_margin_bottom(box, 12);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);
    return box;
}

static void stick_to_right(GtkWidget *w)
{
    gtk_widget_set_hexpand(w, TRUE);
    gtk_widget_set_halign(w, GTK_ALIGN_END);
}

static void build_dialogs(void)
{
    GtkWidget *box, *buttons, *tmp;

    ui.readme_dialog = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(ui.readme_dialog), _("README"));
    gtk_window_set_default_size(GTK_WINDOW(ui.readme_dialog), 640, 420);
    gtk_window_set_transient_for(GTK_WINDOW(ui.readme_dialog), GTK_WINDOW(ui.window));
    box = make_page_box();
    ui.readme_area = new_text_view(FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(ui.readme_area), true);
    gtk_box_append(GTK_BOX(box), scrolled(ui.readme_area));
    gtk_box_append(GTK_BOX(box), make_button(_("Close"), G_CALLBACK(close_readme_slot)));
    gtk_window_set_child(GTK_WINDOW(ui.readme_dialog), box);

    ui.details_dialog = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(ui.details_dialog), _("Update Details"));
    gtk_window_set_default_size(GTK_WINDOW(ui.details_dialog), 700, 460);
    gtk_window_set_transient_for(GTK_WINDOW(ui.details_dialog), GTK_WINDOW(ui.window));
    box = make_page_box();
    ui.update_details_text = new_text_view(FALSE);
    gtk_box_append(GTK_BOX(box), scrolled(ui.update_details_text));
    buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append(GTK_BOX(buttons), make_button(_("Close"), G_CALLBACK(close_details_slot)));
    ui.save_details_button = make_button(_("Save"), G_CALLBACK(save_details_slot));
    stick_to_right(ui.save_details_button);
    gtk_box_append(GTK_BOX(buttons), ui.save_details_button);
    gtk_box_append(GTK_BOX(box), buttons);
    gtk_window_set_child(GTK_WINDOW(ui.details_dialog), box);

    ui.gpg_dialog = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(ui.gpg_dialog), _("GPG Details"));
    gtk_window_set_default_size(GTK_WINDOW(ui.gpg_dialog), 560, 260);
    gtk_window_set_transient_for(GTK_WINDOW(ui.gpg_dialog), GTK_WINDOW(ui.window));
    box = make_page_box();
    ui.gpg_details_text = new_text_view(FALSE);
    gtk_box_append(GTK_BOX(box), scrolled(ui.gpg_details_text));
    gtk_box_append(GTK_BOX(box), make_button(_("Close"), G_CALLBACK(close_gpg_details_slot)));
    gtk_window_set_child(GTK_WINDOW(ui.gpg_dialog), box);

    ui.mirrors_dialog = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(ui.mirrors_dialog), _("Mirrors"));
    gtk_window_set_default_size(GTK_WINDOW(ui.mirrors_dialog), 520, 360);
    gtk_window_set_transient_for(GTK_WINDOW(ui.mirrors_dialog), GTK_WINDOW(ui.window));
    box = make_page_box();
    ui.mirrors_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_box_append(GTK_BOX(box), scrolled(ui.mirrors_vbox));
    buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    tmp = make_button(_("Close"), G_CALLBACK(close_mirrors_slot));
    stick_to_right(tmp);
    gtk_box_append(GTK_BOX(buttons), make_button(_("Choose"), G_CALLBACK(choose_mirror_slot)));
    gtk_box_append(GTK_BOX(buttons), tmp);
    gtk_box_append(GTK_BOX(box), buttons);
    gtk_window_set_child(GTK_WINDOW(ui.mirrors_dialog), box);
}

static void build_main_window(void)
{
    GtkWidget *root, *page, *buttons, *row, *label, *sw;

    ui.window = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(ui.window), _("Loki Update Tool"));
    gtk_window_set_default_size(GTK_WINDOW(ui.window), 500, 400);
    g_signal_connect(ui.window, "close-request", G_CALLBACK(on_window_close), NULL);

    root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    ui.update_notebook = gtk_notebook_new();
    gtk_notebook_set_show_tabs(GTK_NOTEBOOK(ui.update_notebook), FALSE);
    gtk_notebook_set_show_border(GTK_NOTEBOOK(ui.update_notebook), FALSE);
    gtk_box_append(GTK_BOX(root), ui.update_notebook);
    gtk_window_set_child(GTK_WINDOW(ui.window), root);

    /* Product page */
    page = make_page_box();
    label = gtk_label_new(_("Please select the products you would like to update:"));
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(page), label);
    ui.product_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_box_append(GTK_BOX(page), scrolled(ui.product_vbox));

    buttons = gtk_center_box_new();
    gtk_center_box_set_start_widget(GTK_CENTER_BOX(buttons), make_button(_("Exit"), G_CALLBACK(main_cancel_slot)));
    gtk_center_box_set_center_widget(GTK_CENTER_BOX(buttons), make_button(_("Select All"), G_CALLBACK(select_all_products_slot)));
    ui.product_continue_button = make_button(_("Continue"), G_CALLBACK(choose_update_slot));
    gtk_center_box_set_end_widget(GTK_CENTER_BOX(buttons), ui.product_continue_button);
    gtk_box_append(GTK_BOX(page), buttons);
    gtk_notebook_append_page(GTK_NOTEBOOK(ui.update_notebook), page, NULL);

    /* Get list page */
    page = make_page_box();
    ui.product_label = gtk_label_new("");
    gtk_widget_set_halign(ui.product_label, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(page), ui.product_label);
    gtk_box_append(GTK_BOX(page), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    ui.list_download_pixmap = new_image_from_status(&balls[0]);
    label = gtk_label_new(_("Downloading update list"));
    gtk_box_append(GTK_BOX(page), row_with_icon(ui.list_download_pixmap, label));
    ui.update_list_progress = gtk_progress_bar_new();
    gtk_box_append(GTK_BOX(page), ui.update_list_progress);
    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    ui.list_rate_label = gtk_label_new("");
    ui.list_eta_label = gtk_label_new("");
    gtk_box_append(GTK_BOX(row), ui.list_rate_label);
    gtk_box_append(GTK_BOX(row), ui.list_eta_label);
    gtk_box_append(GTK_BOX(page), row);
    gtk_box_append(GTK_BOX(page), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    ui.list_status_arrow = new_image_from_status(&arrows[0]);
    ui.list_status_label = gtk_label_new("");
    ui.list_details_button = make_button(_("Details"), G_CALLBACK(view_details_slot));
    stick_to_right(ui.list_details_button);
    
    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append(GTK_BOX(row), row_with_icon(ui.list_status_arrow, ui.list_status_label));
    gtk_box_append(GTK_BOX(row), ui.list_details_button);
    gtk_box_append(GTK_BOX(page), row);
    gtk_box_append(GTK_BOX(page), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    ui.list_cancel_button = make_button(_("Cancel"), G_CALLBACK(cancel_download_slot));
    ui.list_done_button = make_button(_("Continue"), G_CALLBACK(update_proceed_slot));
    stick_to_right(ui.list_done_button);
    gtk_box_append(GTK_BOX(buttons), ui.list_cancel_button);
    gtk_box_append(GTK_BOX(buttons), ui.list_done_button);
    gtk_box_append(GTK_BOX(page), buttons);
    gtk_notebook_append_page(GTK_NOTEBOOK(ui.update_notebook), page, NULL);

    /* Select update page */
    page = make_page_box();
    label = gtk_label_new(_("Available updates:"));
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(page), label);
    ui.update_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    sw = scrolled(ui.update_vbox);
    gtk_box_append(GTK_BOX(page), sw);
    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    ui.estimated_size_label = gtk_label_new("");
    ui.auto_update_toggle = gtk_check_button_new_with_label(_("Quick Update"));
    g_signal_connect(ui.auto_update_toggle, "toggled", G_CALLBACK(toggle_auto_update_slot), NULL);
    gtk_box_append(GTK_BOX(row), ui.auto_update_toggle);
    gtk_box_append(GTK_BOX(row), ui.estimated_size_label);
    gtk_box_append(GTK_BOX(page), row);

    buttons = gtk_center_box_new();
    gtk_center_box_set_start_widget(GTK_CENTER_BOX(buttons), make_button(_("Main Menu"), G_CALLBACK(main_menu_slot)));
    gtk_center_box_set_center_widget(GTK_CENTER_BOX(buttons), make_button(_("Select All"), G_CALLBACK(select_all_updates_slot)));
    ui.choose_continue_button = make_button(_("Continue"), G_CALLBACK(download_update_slot));
    gtk_center_box_set_end_widget(GTK_CENTER_BOX(buttons), ui.choose_continue_button);
    gtk_box_append(GTK_BOX(page), buttons);
    gtk_notebook_append_page(GTK_NOTEBOOK(ui.update_notebook), page, NULL);

    /* Update page */
    page = make_page_box();
    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    ui.update_name_label = gtk_label_new("");
    ui.update_size_label = gtk_label_new("");
    stick_to_right(ui.update_size_label);
    gtk_box_append(GTK_BOX(row), ui.update_name_label);
    gtk_box_append(GTK_BOX(row), ui.update_size_label);
    gtk_box_append(GTK_BOX(page), row);
    gtk_box_append(GTK_BOX(page), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    ui.update_download_pixmap = new_image_from_status(&balls[0]);
    ui.update_download_label = gtk_label_new(_("Downloading update"));
    gtk_box_append(GTK_BOX(page), row_with_icon(ui.update_download_pixmap, ui.update_download_label));
    ui.update_download_progress = gtk_progress_bar_new();
    gtk_box_append(GTK_BOX(page), ui.update_download_progress);

    ui.update_rate_label = gtk_label_new("");
    ui.update_eta_label = gtk_label_new("");
    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append(GTK_BOX(row), ui.update_rate_label);
    gtk_box_append(GTK_BOX(row), ui.update_eta_label);
    //gtk_box_append(GTK_BOX(page), row);

    buttons = gtk_center_box_new();
    ui.choose_mirror_button = make_button(_("Choose Mirror"), G_CALLBACK(show_mirrors_slot));
    ui.switch_mirror_button = make_button(_("Switch Mirror"), G_CALLBACK(switch_mirror_slot));
    ui.save_mirror_button = make_button(_("Save Mirror"), G_CALLBACK(save_mirror_slot));
    gtk_center_box_set_start_widget(GTK_CENTER_BOX(buttons), ui.choose_mirror_button);
    gtk_center_box_set_center_widget(GTK_CENTER_BOX(buttons), ui.switch_mirror_button);
    gtk_center_box_set_end_widget(GTK_CENTER_BOX(buttons), ui.save_mirror_button);
    gtk_box_append(GTK_BOX(page), buttons);
    gtk_box_append(GTK_BOX(page), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    ui.update_verify_pixmap = new_image_from_status(&balls[0]);
    ui.verify_status_label = gtk_label_new("");
    ui.gpg_status_label = gtk_label_new("");
    ui.gpg_details_button = make_button(_("GPG Details"), G_CALLBACK(view_gpg_details_slot));
    stick_to_right(ui.gpg_details_button);
    gtk_box_append(GTK_BOX(page), row_with_icon(ui.update_verify_pixmap, gtk_label_new(_("Verifying update:"))));
    gtk_box_append(GTK_BOX(page), ui.verify_status_label);
    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append(GTK_BOX(row), ui.gpg_status_label);
    gtk_box_append(GTK_BOX(row), ui.gpg_details_button);
    gtk_box_append(GTK_BOX(page), row);
    gtk_box_append(GTK_BOX(page), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    ui.update_patch_pixmap = new_image_from_status(&balls[0]);
    gtk_box_append(GTK_BOX(page), row_with_icon(ui.update_patch_pixmap, gtk_label_new(_("Performing update:"))));
    ui.update_patch_progress = gtk_progress_bar_new();
    gtk_box_append(GTK_BOX(page), ui.update_patch_progress);
    gtk_box_append(GTK_BOX(page), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    ui.update_status_arrow = new_image_from_status(&arrows[0]);
    ui.update_status_label = gtk_label_new("");
    ui.update_details_button = make_button(_("Details"), G_CALLBACK(view_details_slot));
    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    stick_to_right(ui.update_details_button);
    gtk_box_append(GTK_BOX(row), row_with_icon(ui.update_status_arrow, ui.update_status_label));
    gtk_box_append(GTK_BOX(row), ui.update_details_button);
    gtk_box_append(GTK_BOX(page), row);
    gtk_box_append(GTK_BOX(page), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    buttons = gtk_center_box_new();
    ui.update_cancel_button = make_button(_("Cancel"), G_CALLBACK(cancel_button_slot));
    ui.update_readme_button = make_button(_("README"), G_CALLBACK(view_readme_slot));
    ui.update_action_button = make_button(_("Update"), G_CALLBACK(action_button_slot));
    gtk_center_box_set_start_widget(GTK_CENTER_BOX(buttons), ui.update_cancel_button);
    gtk_center_box_set_center_widget(GTK_CENTER_BOX(buttons), ui.update_readme_button);
    gtk_center_box_set_end_widget(GTK_CENTER_BOX(buttons), ui.update_action_button);
    gtk_box_append(GTK_BOX(page), buttons);
    gtk_notebook_append_page(GTK_NOTEBOOK(ui.update_notebook), page, NULL);

    build_dialogs();
}

static int gtkui_detect(void)
{
    const char *display = getenv("DISPLAY");
    const char *wayland = getenv("WAYLAND_DISPLAY");
    return (display && *display) || (wayland && *wayland);
}

static int gtkui_init(int argc, char *argv[])
{
    GtkWidget *button, *label;
    const char *product_name;
    const char *description;

    g_set_prgname("com.lokigames.loki_update");
    gtk_init();

    product_buttons = g_ptr_array_new();
    update_buttons = g_ptr_array_new();
    build_main_window();

    for (product_name = get_first_product(); product_name; product_name = get_next_product()) {
        description = get_product_description(product_name);
        button = gtk_check_button_new_with_label(description);
        g_object_set_data(G_OBJECT(button), "data", (gpointer)product_name);
        gtk_box_append(GTK_BOX(ui.product_vbox), button);
        g_ptr_array_add(product_buttons, button);
        if (strcasecmp(PRODUCT, product_name) != 0) {
            g_signal_connect(button, "toggled", G_CALLBACK(product_toggle_option), NULL);
        } else {
            gtk_widget_set_visible(button, FALSE);
        }
    }
    if (get_num_products() == 0) {
        label = gtk_label_new(_("No products found.\nAre you the one that installed the software?"));
        gtk_box_append(GTK_BOX(ui.product_vbox), label);
    }
    gtk_button_set_sensitive(ui.product_continue_button, FALSE);
    gtk_button_set_sensitive(ui.update_readme_button, FALSE);
    gtk_button_set_sensitive(ui.gpg_details_button, FALSE);
    mirror_buttons_sensitive(FALSE);

    putenv("PATCH_LOGGING=1");
    putenv("SETUP_NOCHECK=1");
    return 0;
}

static int gtkui_update_product(const char *product)
{
    update_status = 0;
    select_product(product);
    one_product = 1;
    interactive = FULLY_AUTOMATIC;
    choose_update_slot(NULL, NULL);
    return update_status;
}

static int gtkui_perform_updates(const char *product)
{
    if (get_logging() >= LOG_NORMAL) set_logging(LOG_NONE);

    update_status = 0;
    interactive = load_interactive();
    if (product) {
        if (is_valid_product(product)) {
            select_product(product);
            one_product = 1;
            choose_update_slot(NULL, NULL);
        } else {
            GtkWidget *label;
            char message[PATH_MAX];
            snprintf(message, sizeof(message),
                     _("\n\"%s\"\n\nThis is not recognized as either a product tag or a product\ninstallation directory.  If this is a product tag, are you running\nas the user who installed the product?"),
                     product);
            label = gtk_label_new(message);
            gtk_box_append(GTK_BOX(ui.product_vbox), label);
            choose_product_slot(NULL, NULL);
        }
    } else {
        one_product = 0;
        choose_product_slot(NULL, NULL);
    }
    gtk_window_present(GTK_WINDOW(ui.window));
    main_loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(main_loop);
    g_main_loop_unref(main_loop);
    main_loop = NULL;
    return update_status;
}

static void gtkui_cleanup(void)
{
    if (product_patchset) {
        free_patchset(product_patchset);
        product_patchset = NULL;
    }
    stop_flash();
    if (product_buttons) { g_ptr_array_free(product_buttons, TRUE); product_buttons = NULL; }
    if (update_buttons) { g_ptr_array_free(update_buttons, TRUE); update_buttons = NULL; }
    if (ui.window) gtk_window_destroy(GTK_WINDOW(ui.window));
    if (ui.readme_dialog) gtk_window_destroy(GTK_WINDOW(ui.readme_dialog));
    if (ui.details_dialog) gtk_window_destroy(GTK_WINDOW(ui.details_dialog));
    if (ui.gpg_dialog) gtk_window_destroy(GTK_WINDOW(ui.gpg_dialog));
    if (ui.mirrors_dialog) gtk_window_destroy(GTK_WINDOW(ui.mirrors_dialog));
    memset(&ui, 0, sizeof(ui));
}

update_UI gtk_ui = {
    gtkui_detect,
    gtkui_init,
    gtkui_update_product,
    gtkui_perform_updates,
    gtkui_cleanup
};

#ifdef DYNAMIC_UI
update_UI *create_ui(void)
{
    update_UI *ui_ptr = NULL;
    if (gtk_ui.detect()) {
        ui_ptr = &gtk_ui;
    }
    return ui_ptr;
}
#endif /* DYNAMIC_UI */
