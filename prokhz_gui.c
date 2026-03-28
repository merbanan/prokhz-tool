/*
 * prokhz_gui.c — GTK4 graphical frontend for libprokhz
 *
 * Device operations run on a worker thread to keep the UI responsive.
 * Drivers are registered once at startup.  Probe selects the matched
 * driver in the dropdown automatically.  The serial device entry is
 * a dropdown populated by scanning /dev/ttyUSB* and /dev/ttyACM*.
 */

#include "prokhz.h"
#include "prokhz_device.h"

#include <gtk/gtk.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <glob.h>
#include <time.h>

/* ── Aptus constants (mirror of prokhz_tool.c) ────────────────────────── */

#define APTUS_MIN 100000000UL
#define APTUS_MAX 999999999UL

/* ── format table ─────────────────────────────────────────────────────── */

typedef struct {
    const char      *label;
    prokhz_format_t  fmt;
} FormatEntry;

static const FormatEntry formats[] = {
    { "hex",        PROKHZ_FMT_HEX        },
    { "hex-spaced", PROKHZ_FMT_HEX_SPACED },
    { "dec8h",      PROKHZ_FMT_DEC_8H     },
    { "dec6h",      PROKHZ_FMT_DEC_6H     },
    { "wiegand",    PROKHZ_FMT_WIEGAND     },
    { "dec9",       PROKHZ_FMT_DEC_9      },
    { "dec8",       PROKHZ_FMT_DEC_8      },
};
#define N_FORMATS ((int)(sizeof formats / sizeof formats[0]))

/* ── application state ────────────────────────────────────────────────── */

typedef struct {
    prokhz_dev_t   *dev;
    char            driver_name[64];
    int             n_drivers;

    /* main window */
    GtkWidget      *window;
    GtkWidget      *status_bar;
    GtkWidget      *driver_label;

    /* device frame */
    GtkWidget      *probe_button;
    GtkWidget      *driver_drop;
    GtkWidget      *serial_drop;
    GtkWidget      *serial_refresh_btn;
    GtkWidget      *connect_button;
    GtkWidget      *disconnect_button;
    GtkWidget      *beep_button;
    GtkWidget      *verbose_check;

    /* read frame — one entry per format */
    GtkWidget      *read_button;
    GtkWidget      *read_entries[N_FORMATS];  /* indexed by formats[]    */
    GtkWidget      *copy_buttons[N_FORMATS];

    /* write frame */
    GtkWidget      *tag_type_drop;
    GtkWidget      *write_entry;
    GtkWidget      *write_button;

    /* aptus frame */
    GtkWidget      *aptus_tag_type_drop;
    GtkWidget      *aptus_start_button;
    GtkWidget      *aptus_stop_button;
    GtkWidget      *aptus_count_label;
    GtkWidget      *aptus_last_label;
    volatile int    aptus_running;
    guint           aptus_count;

    /* log */
    GtkWidget      *log_view;
    GtkTextBuffer  *log_buf;
} AppState;

static AppState app;

/* ── logging ──────────────────────────────────────────────────────────── */

static void log_append(const char *line)
{
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(app.log_buf, &end);
    gtk_text_buffer_insert(app.log_buf, &end, line, -1);
    gtk_text_buffer_insert(app.log_buf, &end, "\n", -1);
    GtkTextMark *mark = gtk_text_buffer_get_insert(app.log_buf);
    gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(app.log_view), mark);
}

static void set_status(const char *msg)
{
    gtk_label_set_text(GTK_LABEL(app.status_bar), msg);
}

/* ── GtkDropDown helpers ──────────────────────────────────────────────── */

static GtkWidget *make_dropdown(const char * const *items)
{
    GtkStringList *sl = gtk_string_list_new(NULL);
    for (int i = 0; items[i]; i++)
        gtk_string_list_append(sl, items[i]);
    GtkWidget *dd = gtk_drop_down_new(G_LIST_MODEL(sl), NULL);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(dd), 0);
    return dd;
}

static const char *dropdown_get_text(GtkWidget *dd)
{
    guint idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(dd));
    GListModel *model = gtk_drop_down_get_model(GTK_DROP_DOWN(dd));
    GtkStringObject *obj = g_list_model_get_item(model, idx);
    return obj ? gtk_string_object_get_string(obj) : NULL;
}

static int dropdown_get_index(GtkWidget *dd)
{
    return (int)gtk_drop_down_get_selected(GTK_DROP_DOWN(dd));
}

static void dropdown_select_text(GtkWidget *dd, const char *needle)
{
    GListModel *model = gtk_drop_down_get_model(GTK_DROP_DOWN(dd));
    guint n = g_list_model_get_n_items(model);
    for (guint i = 0; i < n; i++) {
        GtkStringObject *obj = g_list_model_get_item(model, i);
        if (obj && strcmp(gtk_string_object_get_string(obj), needle) == 0) {
            gtk_drop_down_set_selected(GTK_DROP_DOWN(dd), i);
            return;
        }
    }
}

static void dropdown_repopulate(GtkWidget *dd, const char * const *items)
{
    GListModel    *model = gtk_drop_down_get_model(GTK_DROP_DOWN(dd));
    GtkStringList *sl    = GTK_STRING_LIST(model);
    guint n = g_list_model_get_n_items(G_LIST_MODEL(sl));
    gtk_string_list_splice(sl, 0, n, NULL);
    for (int i = 0; items[i]; i++)
        gtk_string_list_append(sl, items[i]);
    if (g_list_model_get_n_items(G_LIST_MODEL(sl)) > 0)
        gtk_drop_down_set_selected(GTK_DROP_DOWN(dd), 0);
}

/* ── serial device scan ───────────────────────────────────────────────── */

static void refresh_serial_devices(void)
{
    glob_t g;
    memset(&g, 0, sizeof g);
    glob("/dev/ttyUSB*", GLOB_NOSORT, NULL, &g);
    glob("/dev/ttyACM*", GLOB_NOSORT | GLOB_APPEND, NULL, &g);

    const char **items = g_new0(const char *, g.gl_pathc + 2);
    items[0] = "(none)";
    for (size_t i = 0; i < g.gl_pathc; i++)
        items[i + 1] = g.gl_pathv[i];
    items[g.gl_pathc + 1] = NULL;

    dropdown_repopulate(app.serial_drop, items);

    /* default to first real device if any found */
    if (g.gl_pathc > 0)
        gtk_drop_down_set_selected(GTK_DROP_DOWN(app.serial_drop), 1);

    g_free(items);

    char msg[64];
    snprintf(msg, sizeof msg, "Found %zu serial device(s).", g.gl_pathc);
    log_append(msg);
    globfree(&g);
}

/* ── UI sensitivity helpers ───────────────────────────────────────────── */

static void update_sensitivity(void)
{
    gboolean connected = (app.dev != NULL);
    gboolean aptus_on  = (app.aptus_running != 0);

    gtk_widget_set_sensitive(app.connect_button,     !connected);
    gtk_widget_set_sensitive(app.probe_button,       !connected);
    gtk_widget_set_sensitive(app.driver_drop,        !connected);
    gtk_widget_set_sensitive(app.serial_drop,        !connected);
    gtk_widget_set_sensitive(app.serial_refresh_btn, !connected);
    gtk_widget_set_sensitive(app.disconnect_button,   connected && !aptus_on);
    gtk_widget_set_sensitive(app.beep_button,         connected && !aptus_on);
    gtk_widget_set_sensitive(app.read_button,         connected && !aptus_on);
    gtk_widget_set_sensitive(app.write_button,        connected && !aptus_on);
    gtk_widget_set_sensitive(app.aptus_start_button,  connected && !aptus_on);
    gtk_widget_set_sensitive(app.aptus_stop_button,   connected &&  aptus_on);

    if (connected) {
        char label[128];
        snprintf(label, sizeof label, "Connected: %s", app.driver_name);
        gtk_label_set_text(GTK_LABEL(app.driver_label), label);
    } else {
        gtk_label_set_text(GTK_LABEL(app.driver_label), "Not connected");
    }
}

/* ── generic result ───────────────────────────────────────────────────── */

typedef struct {
    gboolean success;
    char     message[256];
    uint8_t  tag_bytes[PROKHZ_TAG_ID_LEN];
    gboolean has_tag;
} OpResult;

/* ── probe ────────────────────────────────────────────────────────────── */

typedef struct {
    int  verbose;
    char device[256];
} ProbeTask;

typedef struct {
    char report[4096];
    char found_driver[64];
} ProbeResult;

static gboolean on_probe_idle(gpointer data)
{
    ProbeResult *res = data;
    log_append(res->report);
    if (res->found_driver[0]) {
        dropdown_select_text(app.driver_drop, res->found_driver);
        char msg[128];
        snprintf(msg, sizeof msg, "Probe complete — found %s.",
                 res->found_driver);
        set_status(msg);
    } else {
        set_status("Probe complete — no device found.");
    }
    gtk_widget_set_sensitive(app.probe_button,   TRUE);
    gtk_widget_set_sensitive(app.connect_button, TRUE);
    g_free(res);
    return G_SOURCE_REMOVE;
}

static gpointer probe_thread(gpointer data)
{
    ProbeTask   *pt  = data;
    ProbeResult *res = g_new0(ProbeResult, 1);
    prokhz_opts_t opts = {
        .device     = pt->device[0] ? pt->device : NULL,
        .beep       = 0,
        .verbose    = pt->verbose,
        .timeout_ms = 2000,
    };
    int pos = 0, n = app.n_drivers;
    pos += snprintf(res->report + pos, sizeof res->report - pos,
                    "Probing %d driver(s):\n", n);
    for (int i = 0; i < n; i++) {
        const prokhz_device_t *drv = prokhz_device_get(i);
        prokhz_dev_t *dev = NULL;
        prokhz_err_t  rc  = prokhz_open(drv->name, &opts, &dev);
        if (rc == PROKHZ_OK) {
            pos += snprintf(res->report + pos, sizeof res->report - pos,
                            "  %-14s  FOUND\n", drv->name);
            if (!res->found_driver[0])
                g_strlcpy(res->found_driver, drv->name,
                          sizeof res->found_driver);
            prokhz_close(dev);
        } else {
            pos += snprintf(res->report + pos, sizeof res->report - pos,
                            "  %-14s  not found (%s)\n",
                            drv->name, prokhz_strerror(rc));
        }
    }
    g_idle_add(on_probe_idle, res);
    g_free(pt);
    return NULL;
}

static void on_probe_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn; (void)user_data;
    gtk_widget_set_sensitive(app.probe_button,   FALSE);
    gtk_widget_set_sensitive(app.connect_button, FALSE);
    set_status("Probing...");
    ProbeTask *pt = g_new0(ProbeTask, 1);
    pt->verbose = gtk_check_button_get_active(
                      GTK_CHECK_BUTTON(app.verbose_check));
    const char *dev = dropdown_get_text(app.serial_drop);
    if (dev && strcmp(dev, "(none)") != 0)
        g_strlcpy(pt->device, dev, sizeof pt->device);
    g_thread_unref(g_thread_new("probe", probe_thread, pt));
}

/* ── connect ──────────────────────────────────────────────────────────── */

typedef struct {
    char driver[64];
    char device[256];
    int  verbose;
} ConnectTask;

typedef struct {
    OpResult      result;
    prokhz_dev_t *dev;
    char          driver_name[64];
} ConnectResult;

static gboolean on_connect_idle(gpointer data)
{
    ConnectResult *cr = data;
    log_append(cr->result.message);
    set_status(cr->result.message);
    if (cr->result.success) {
        app.dev = cr->dev;
        g_strlcpy(app.driver_name, cr->driver_name, sizeof app.driver_name);
    }
    update_sensitivity();
    g_free(cr);
    return G_SOURCE_REMOVE;
}

static gpointer connect_thread(gpointer data)
{
    ConnectTask   *ct  = data;
    ConnectResult *cr  = g_new0(ConnectResult, 1);
    prokhz_opts_t  opts = {
        .device     = ct->device[0] ? ct->device : NULL,
        .beep       = 0,
        .verbose    = ct->verbose,
        .timeout_ms = 2000,
    };
    prokhz_dev_t *dev = NULL;
    prokhz_err_t  rc  = ct->driver[0]
                        ? prokhz_open(ct->driver, &opts, &dev)
                        : prokhz_probe(&opts, &dev);
    if (rc == PROKHZ_OK) {
        cr->result.success = TRUE;
        cr->dev = dev;
        g_strlcpy(cr->driver_name, prokhz_driver_name(dev),
                  sizeof cr->driver_name);
        snprintf(cr->result.message, sizeof cr->result.message,
                 "Connected: %s", prokhz_driver_name(dev));
    } else {
        cr->result.success = FALSE;
        snprintf(cr->result.message, sizeof cr->result.message,
                 "Connect failed: %s", prokhz_strerror(rc));
    }
    g_idle_add(on_connect_idle, cr);
    g_free(ct);
    return NULL;
}

static void on_connect_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn; (void)user_data;
    set_status("Connecting...");
    ConnectTask *ct = g_new0(ConnectTask, 1);
    const char *drv = dropdown_get_text(app.driver_drop);
    if (drv && strcmp(drv, "(auto)") != 0)
        g_strlcpy(ct->driver, drv, sizeof ct->driver);
    const char *dev = dropdown_get_text(app.serial_drop);
    if (dev && strcmp(dev, "(none)") != 0)
        g_strlcpy(ct->device, dev, sizeof ct->device);
    ct->verbose = gtk_check_button_get_active(
                      GTK_CHECK_BUTTON(app.verbose_check));
    g_thread_unref(g_thread_new("connect", connect_thread, ct));
}

/* ── beep ─────────────────────────────────────────────────────────────── */

static void on_beep_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn; (void)user_data;
    if (!app.dev) return;
    prokhz_err_t rc = prokhz_beep(app.dev, 3);
    if (rc == PROKHZ_ERR_NOTSUP)
        log_append("Beep: not supported by this driver.");
    else if (rc != PROKHZ_OK) {
        char msg[64];
        snprintf(msg, sizeof msg, "Beep failed: %s", prokhz_strerror(rc));
        log_append(msg);
    } else {
        log_append("Beep sent.");
    }
}

/* ── disconnect ───────────────────────────────────────────────────────── */

static void on_disconnect_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn; (void)user_data;
    if (app.dev) { prokhz_close(app.dev); app.dev = NULL; }
    app.driver_name[0] = '\0';
    log_append("Disconnected.");
    set_status("Disconnected.");
    update_sensitivity();
}

/* ── read ─────────────────────────────────────────────────────────────── */

/* Populate all format entry boxes from raw tag bytes. */
static void fill_read_entries(const uint8_t bytes[PROKHZ_TAG_ID_LEN])
{
    char buf[32];
    for (int i = 0; i < N_FORMATS; i++) {
        prokhz_format_id(bytes, formats[i].fmt, buf, sizeof buf);
        gtk_editable_set_text(GTK_EDITABLE(app.read_entries[i]), buf);
    }
}

static gboolean on_read_idle(gpointer data)
{
    OpResult *res = data;
    log_append(res->message);
    set_status(res->message);
    if (res->success && res->has_tag)
        fill_read_entries(res->tag_bytes);
    gtk_widget_set_sensitive(app.read_button, TRUE);
    g_free(res);
    return G_SOURCE_REMOVE;
}

static gpointer read_thread(gpointer data)
{
    prokhz_dev_t    *dev = data;
    OpResult        *res = g_new0(OpResult, 1);
    prokhz_tag_id_t  tag = {0};

    prokhz_err_t rc = prokhz_read(dev, &tag);
    if (rc == PROKHZ_OK) {
        res->success = TRUE;
        res->has_tag = TRUE;
        memcpy(res->tag_bytes, tag.bytes, PROKHZ_TAG_ID_LEN);
        char hex[16];
        prokhz_format_id(tag.bytes, PROKHZ_FMT_HEX, hex, sizeof hex);
        snprintf(res->message, sizeof res->message, "Read: %s", hex);
    } else {
        res->success = FALSE;
        snprintf(res->message, sizeof res->message,
                 "Read failed: %s", prokhz_strerror(rc));
    }
    g_idle_add(on_read_idle, res);
    return NULL;
}

static void on_read_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn; (void)user_data;
    if (!app.dev) return;
    gtk_widget_set_sensitive(app.read_button, FALSE);
    set_status("Reading...");
    g_thread_unref(g_thread_new("read", read_thread, app.dev));
}

/* ── copy individual format to clipboard ──────────────────────────────── */

static void on_copy_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    int idx = GPOINTER_TO_INT(user_data);
    const char *text = gtk_editable_get_text(
                           GTK_EDITABLE(app.read_entries[idx]));
    if (text && *text) {
        GdkClipboard *cb = gtk_widget_get_clipboard(app.window);
        gdk_clipboard_set_text(cb, text);
        char msg[64];
        snprintf(msg, sizeof msg, "Copied %s to clipboard.",
                 formats[idx].label);
        log_append(msg);
    }
}

/* ── write ────────────────────────────────────────────────────────────── */

typedef struct {
    prokhz_dev_t     *dev;
    uint8_t           id[PROKHZ_TAG_ID_LEN];
    prokhz_tag_type_t tag_type;
} WriteTask;

static gboolean on_write_idle(gpointer data)
{
    OpResult *res = data;
    log_append(res->message);
    set_status(res->message);
    gtk_widget_set_sensitive(app.write_button, TRUE);
    g_free(res);
    return G_SOURCE_REMOVE;
}

static gpointer write_thread(gpointer data)
{
    WriteTask *wt  = data;
    OpResult  *res = g_new0(OpResult, 1);
    prokhz_err_t rc = prokhz_write(wt->dev, wt->id, wt->tag_type);
    if (rc == PROKHZ_OK) {
        res->success = TRUE;
        char hex[16];
        prokhz_format_id(wt->id, PROKHZ_FMT_HEX, hex, sizeof hex);
        snprintf(res->message, sizeof res->message, "Write OK: %s", hex);
    } else {
        res->success = FALSE;
        snprintf(res->message, sizeof res->message,
                 "Write failed: %s", prokhz_strerror(rc));
    }
    g_idle_add(on_write_idle, res);
    g_free(wt);
    return NULL;
}

static void on_write_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn; (void)user_data;
    if (!app.dev) return;

    /* Use hex entry (index 0) as the write source */
    const char *hex = gtk_editable_get_text(
                          GTK_EDITABLE(app.read_entries[0]));
    /* Also accept a manually typed ID in the write entry */
    const char *manual = gtk_editable_get_text(
                             GTK_EDITABLE(app.write_entry));
    const char *src = (manual && strlen(manual) == 10) ? manual : hex;

    WriteTask *wt = g_new0(WriteTask, 1);
    if (prokhz_parse_hex_id(src, wt->id) != PROKHZ_OK) {
        log_append("Write error: ID must be exactly 10 hex characters.");
        set_status("Invalid tag ID.");
        g_free(wt);
        return;
    }

    static const prokhz_tag_type_t type_map[] = {
        PROKHZ_TAG_T5577, PROKHZ_TAG_EM4305, PROKHZ_TAG_EM4100,
    };
    int idx = dropdown_get_index(app.tag_type_drop);
    wt->tag_type = (idx >= 0 && idx < 3) ? type_map[idx] : PROKHZ_TAG_T5577;
    wt->dev = app.dev;

    gtk_widget_set_sensitive(app.write_button, FALSE);
    set_status("Writing...");
    g_thread_unref(g_thread_new("write", write_thread, wt));
}

/* ── Aptus generation ─────────────────────────────────────────────────── */

typedef struct {
    prokhz_dev_t     *dev;
    prokhz_tag_type_t tag_type;
} AptusTask;

typedef struct {
    unsigned long val;
    char          hex[16];
    gboolean      done;   /* TRUE = thread finished (stop was requested) */
} AptusProgress;

static gboolean on_aptus_progress(gpointer data)
{
    AptusProgress *ap = data;
    if (ap->done) {
        log_append("Aptus generation stopped.");
        set_status("Aptus generation stopped.");
        app.aptus_running = 0;
        update_sensitivity();
    } else {
        char msg[64];
        snprintf(msg, sizeof msg, "Aptus: %lu  %s", ap->val, ap->hex);
        gtk_label_set_text(GTK_LABEL(app.aptus_last_label), ap->hex);
        app.aptus_count++;
        char count_str[32];
        snprintf(count_str, sizeof count_str, "Written: %u", app.aptus_count);
        gtk_label_set_text(GTK_LABEL(app.aptus_count_label), count_str);
        log_append(msg);
        set_status(msg);
    }
    g_free(ap);
    return G_SOURCE_REMOVE;
}

static gpointer aptus_thread(gpointer data)
{
    AptusTask *at = data;
    srand((unsigned)time(NULL));

    while (app.aptus_running) {
        unsigned long val = (unsigned long)(
            (double)rand() / ((double)RAND_MAX + 1.0) *
            (double)(APTUS_MAX - APTUS_MIN + 1)
        ) + APTUS_MIN;

        uint8_t id[PROKHZ_TAG_ID_LEN];
        id[0] = 0x01;
        id[1] = (uint8_t)(val >> 24);
        id[2] = (uint8_t)(val >> 16);
        id[3] = (uint8_t)(val >>  8);
        id[4] = (uint8_t)(val);

        char written[16], readback[16];
        prokhz_format_id(id, PROKHZ_FMT_HEX, written, sizeof written);

        /* Write and verify, retry until readback matches or stopped */
        prokhz_tag_id_t verify;
        prokhz_err_t rc;
        do {
            if (!app.aptus_running) goto done;
            rc = prokhz_write(at->dev, id, at->tag_type);
            if (rc != PROKHZ_OK) continue;
            memset(&verify, 0, sizeof verify);
            rc = prokhz_read(at->dev, &verify);
            if (rc != PROKHZ_OK) continue;
            prokhz_format_id(verify.bytes, PROKHZ_FMT_HEX,
                             readback, sizeof readback);
        } while (strcmp(written, readback) != 0);

        prokhz_beep(at->dev, 1);

        AptusProgress *ap = g_new0(AptusProgress, 1);
        ap->val  = val;
        ap->done = FALSE;
        g_strlcpy(ap->hex, written, sizeof ap->hex);
        g_idle_add(on_aptus_progress, ap);
    }

done: {
        AptusProgress *ap = g_new0(AptusProgress, 1);
        ap->done = TRUE;
        g_idle_add(on_aptus_progress, ap);
        g_free(at);
        return NULL;
    }
}

static void on_aptus_start_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn; (void)user_data;
    if (!app.dev || app.aptus_running) return;

    static const prokhz_tag_type_t type_map[] = {
        PROKHZ_TAG_T5577, PROKHZ_TAG_EM4305,
    };
    int idx = dropdown_get_index(app.aptus_tag_type_drop);
    if (idx < 0 || idx > 1) idx = 0;

    app.aptus_running = 1;
    app.aptus_count   = 0;
    gtk_label_set_text(GTK_LABEL(app.aptus_count_label), "Written: 0");
    gtk_label_set_text(GTK_LABEL(app.aptus_last_label),  "—");
    update_sensitivity();
    log_append("Aptus generation started.");
    set_status("Generating Aptus tags...");

    AptusTask *at = g_new0(AptusTask, 1);
    at->dev      = app.dev;
    at->tag_type = type_map[idx];
    g_thread_unref(g_thread_new("aptus", aptus_thread, at));
}

static void on_aptus_stop_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn; (void)user_data;
    app.aptus_running = 0;   /* thread polls this and exits cleanly */
    set_status("Stopping Aptus generation...");
}

/* ── clear log ────────────────────────────────────────────────────────── */

static void on_clear_log_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn; (void)user_data;
    gtk_text_buffer_set_text(app.log_buf, "", 0);
}

/* ── serial refresh ───────────────────────────────────────────────────── */

static void on_serial_refresh_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn; (void)user_data;
    refresh_serial_devices();
}

/* ── UI construction helpers ──────────────────────────────────────────── */

static void grid_attach_label(GtkWidget *grid, const char *text,
                               int col, int row)
{
    GtkWidget *lbl = gtk_label_new(text);
    gtk_widget_set_halign(lbl, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(grid), lbl, col, row, 1, 1);
}

static GtkWidget *make_frame_grid(const char *title)
{
    GtkWidget *frame = gtk_frame_new(title);
    GtkWidget *grid  = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_widget_set_margin_start(grid, 8);
    gtk_widget_set_margin_end(grid, 8);
    gtk_widget_set_margin_top(grid, 8);
    gtk_widget_set_margin_bottom(grid, 8);
    gtk_frame_set_child(GTK_FRAME(frame), grid);
    g_object_set_data(G_OBJECT(frame), "grid", grid);
    return frame;
}

/* ── device frame ─────────────────────────────────────────────────────── */

static GtkWidget *build_device_frame(void)
{
    GtkWidget *frame = make_frame_grid("Device");
    GtkWidget *grid  = g_object_get_data(G_OBJECT(frame), "grid");

    int n = app.n_drivers;
    const char **driver_items = g_new0(const char *, n + 2);
    driver_items[0] = "(auto)";
    for (int i = 0; i < n; i++)
        driver_items[i + 1] = prokhz_device_get(i)->name;
    driver_items[n + 1] = NULL;

    grid_attach_label(grid, "Driver:", 0, 0);
    app.driver_drop = make_dropdown(driver_items);
    gtk_widget_set_hexpand(app.driver_drop, TRUE);
    gtk_grid_attach(GTK_GRID(grid), app.driver_drop, 1, 0, 3, 1);
    g_free(driver_items);

    grid_attach_label(grid, "Serial port:", 0, 1);
    static const char * const none_items[] = { "(none)", NULL };
    app.serial_drop = make_dropdown(none_items);
    gtk_widget_set_hexpand(app.serial_drop, TRUE);
    gtk_grid_attach(GTK_GRID(grid), app.serial_drop, 1, 1, 2, 1);

    app.serial_refresh_btn = gtk_button_new_with_label("↺");
    gtk_widget_set_tooltip_text(app.serial_refresh_btn, "Rescan serial ports");
    g_signal_connect(app.serial_refresh_btn, "clicked",
                     G_CALLBACK(on_serial_refresh_clicked), NULL);
    gtk_grid_attach(GTK_GRID(grid), app.serial_refresh_btn, 3, 1, 1, 1);

    app.verbose_check = gtk_check_button_new_with_label("Verbose");
    gtk_grid_attach(GTK_GRID(grid), app.verbose_check, 0, 2, 1, 1);

    app.probe_button = gtk_button_new_with_label("Probe");
    g_signal_connect(app.probe_button, "clicked",
                     G_CALLBACK(on_probe_clicked), NULL);
    gtk_grid_attach(GTK_GRID(grid), app.probe_button, 1, 2, 1, 1);

    app.connect_button = gtk_button_new_with_label("Connect");
    g_signal_connect(app.connect_button, "clicked",
                     G_CALLBACK(on_connect_clicked), NULL);
    gtk_grid_attach(GTK_GRID(grid), app.connect_button, 2, 2, 1, 1);

    app.disconnect_button = gtk_button_new_with_label("Disconnect");
    g_signal_connect(app.disconnect_button, "clicked",
                     G_CALLBACK(on_disconnect_clicked), NULL);
    gtk_grid_attach(GTK_GRID(grid), app.disconnect_button, 3, 2, 1, 1);

    app.beep_button = gtk_button_new_with_label("Beep");
    g_signal_connect(app.beep_button, "clicked",
                     G_CALLBACK(on_beep_clicked), NULL);
    gtk_grid_attach(GTK_GRID(grid), app.beep_button, 4, 2, 1, 1);

    app.driver_label = gtk_label_new("Not connected");
    gtk_widget_set_halign(app.driver_label, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), app.driver_label, 0, 3, 4, 1);

    return frame;
}

/* ── read frame ───────────────────────────────────────────────────────── */

static GtkWidget *build_read_frame(void)
{
    GtkWidget *frame = make_frame_grid("Read");
    GtkWidget *grid  = g_object_get_data(G_OBJECT(frame), "grid");

    app.read_button = gtk_button_new_with_label("Read tag");
    g_signal_connect(app.read_button, "clicked",
                     G_CALLBACK(on_read_clicked), NULL);
    gtk_grid_attach(GTK_GRID(grid), app.read_button, 0, 0, 1, 1);

    /* One row per format: label | entry | copy button */
    for (int i = 0; i < N_FORMATS; i++) {
        grid_attach_label(grid, formats[i].label, 0, i + 1);

        app.read_entries[i] = gtk_entry_new();
        gtk_editable_set_editable(GTK_EDITABLE(app.read_entries[i]), FALSE);
        gtk_widget_set_hexpand(app.read_entries[i], TRUE);
        gtk_grid_attach(GTK_GRID(grid), app.read_entries[i], 1, i + 1, 1, 1);

        app.copy_buttons[i] = gtk_button_new_with_label("Copy");
        g_signal_connect(app.copy_buttons[i], "clicked",
                         G_CALLBACK(on_copy_clicked),
                         GINT_TO_POINTER(i));
        gtk_grid_attach(GTK_GRID(grid), app.copy_buttons[i], 2, i + 1, 1, 1);
    }

    return frame;
}

/* ── write frame ──────────────────────────────────────────────────────── */

static GtkWidget *build_write_frame(void)
{
    GtkWidget *frame = make_frame_grid("Write");
    GtkWidget *grid  = g_object_get_data(G_OBJECT(frame), "grid");

    static const char * const type_items[] = {
        "T5577", "EM4305", "EM4100", NULL
    };
    grid_attach_label(grid, "Tag type:", 0, 0);
    app.tag_type_drop = make_dropdown(type_items);
    gtk_grid_attach(GTK_GRID(grid), app.tag_type_drop, 1, 0, 1, 1);

    grid_attach_label(grid, "Tag ID (hex):", 0, 1);
    app.write_entry = gtk_entry_new();
    gtk_entry_set_max_length(GTK_ENTRY(app.write_entry), 10);
    gtk_entry_set_placeholder_text(GTK_ENTRY(app.write_entry),
                                   "10 hex chars, or leave blank to use read result");
    gtk_widget_set_hexpand(app.write_entry, TRUE);
    gtk_grid_attach(GTK_GRID(grid), app.write_entry, 1, 1, 1, 1);

    app.write_button = gtk_button_new_with_label("Write tag");
    g_signal_connect(app.write_button, "clicked",
                     G_CALLBACK(on_write_clicked), NULL);
    gtk_grid_attach(GTK_GRID(grid), app.write_button, 2, 1, 1, 1);

    return frame;
}

/* ── aptus frame ──────────────────────────────────────────────────────── */

static GtkWidget *build_aptus_frame(void)
{
    GtkWidget *frame = make_frame_grid("Generate Aptus tags");
    GtkWidget *grid  = g_object_get_data(G_OBJECT(frame), "grid");

    static const char * const type_items[] = { "T5577", "EM4305", NULL };
    grid_attach_label(grid, "Tag type:", 0, 0);
    app.aptus_tag_type_drop = make_dropdown(type_items);
    gtk_grid_attach(GTK_GRID(grid), app.aptus_tag_type_drop, 1, 0, 1, 1);

    app.aptus_start_button = gtk_button_new_with_label("Start");
    g_signal_connect(app.aptus_start_button, "clicked",
                     G_CALLBACK(on_aptus_start_clicked), NULL);
    gtk_grid_attach(GTK_GRID(grid), app.aptus_start_button, 2, 0, 1, 1);

    app.aptus_stop_button = gtk_button_new_with_label("Stop");
    g_signal_connect(app.aptus_stop_button, "clicked",
                     G_CALLBACK(on_aptus_stop_clicked), NULL);
    gtk_grid_attach(GTK_GRID(grid), app.aptus_stop_button, 3, 0, 1, 1);

    app.aptus_count_label = gtk_label_new("Written: 0");
    gtk_widget_set_halign(app.aptus_count_label, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), app.aptus_count_label, 0, 1, 2, 1);

    grid_attach_label(grid, "Last ID:", 2, 1);
    app.aptus_last_label = gtk_label_new("—");
    gtk_widget_set_halign(app.aptus_last_label, GTK_ALIGN_START);
    gtk_widget_set_hexpand(app.aptus_last_label, TRUE);
    gtk_grid_attach(GTK_GRID(grid), app.aptus_last_label, 3, 1, 1, 1);

    return frame;
}

/* ── log frame ────────────────────────────────────────────────────────── */

static GtkWidget *build_log_frame(void)
{
    GtkWidget *frame = gtk_frame_new("Log");
    GtkWidget *vbox  = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_start(vbox, 8);
    gtk_widget_set_margin_end(vbox, 8);
    gtk_widget_set_margin_top(vbox, 8);
    gtk_widget_set_margin_bottom(vbox, 8);
    gtk_frame_set_child(GTK_FRAME(frame), vbox);

    app.log_buf  = gtk_text_buffer_new(NULL);
    app.log_view = gtk_text_view_new_with_buffer(app.log_buf);
    gtk_text_view_set_editable(GTK_TEXT_VIEW(app.log_view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(app.log_view), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(app.log_view), GTK_WRAP_CHAR);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_widget_set_size_request(scroll, -1, 140);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), app.log_view);
    gtk_box_append(GTK_BOX(vbox), scroll);

    GtkWidget *clear_btn = gtk_button_new_with_label("Clear log");
    gtk_widget_set_halign(clear_btn, GTK_ALIGN_END);
    g_signal_connect(clear_btn, "clicked",
                     G_CALLBACK(on_clear_log_clicked), NULL);
    gtk_box_append(GTK_BOX(vbox), clear_btn);

    return frame;
}

/* ── application activate ─────────────────────────────────────────────── */

static void on_activate(GtkApplication *gtk_app, gpointer user_data)
{
    (void)user_data;
    memset(&app, 0, sizeof app);

    prokhz_device_register_all();
    app.n_drivers = prokhz_device_count();

    app.window = gtk_application_window_new(gtk_app);
    gtk_window_set_title(GTK_WINDOW(app.window), "prokhz — 125 kHz RFID");
    gtk_window_set_default_size(GTK_WINDOW(app.window), 580, -1);
    gtk_window_set_resizable(GTK_WINDOW(app.window), TRUE);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(vbox, 8);
    gtk_widget_set_margin_end(vbox, 8);
    gtk_widget_set_margin_top(vbox, 8);
    gtk_widget_set_margin_bottom(vbox, 8);
    gtk_window_set_child(GTK_WINDOW(app.window), vbox);

    gtk_box_append(GTK_BOX(vbox), build_device_frame());
    gtk_box_append(GTK_BOX(vbox), build_read_frame());
    gtk_box_append(GTK_BOX(vbox), build_write_frame());
    gtk_box_append(GTK_BOX(vbox), build_aptus_frame());
    gtk_box_append(GTK_BOX(vbox), build_log_frame());

    app.status_bar = gtk_label_new("Ready.");
    gtk_widget_set_halign(app.status_bar, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(vbox), app.status_bar);

    update_sensitivity();
    gtk_window_present(GTK_WINDOW(app.window));
    refresh_serial_devices();
    log_append("prokhz-gui started.");
}

/* ── main ─────────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    GtkApplication *gtk_app =
        gtk_application_new("se.southpole.prokhz",
                            G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(gtk_app, "activate", G_CALLBACK(on_activate), NULL);
    int status = g_application_run(G_APPLICATION(gtk_app), argc, argv);
    g_object_unref(gtk_app);
    if (app.dev) prokhz_close(app.dev);
    return status;
}
