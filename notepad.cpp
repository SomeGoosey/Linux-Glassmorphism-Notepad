#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <gdk/gdk.h>
#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#include <X11/Xatom.h>
#endif
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#define ICON_PATH "/home/yugmaj2139/Documents/Default Project/notepad.png"

/* ------------------------------- globals ------------------------------ */

struct Doc {
    GtkTextBuffer* buffer;
    std::string    path;
    std::string    name;
    gboolean       dirty;
};

static GtkWidget               *window;
static GtkWidget               *notebook;
static std::vector<GtkWidget*>  glass_windows;
static double                   anim_t = 0.0;

static Doc* new_tab(const std::string& content, const std::string& path);

/* ----------------------------- animations ---------------------------- */

struct Anim {
    GtkWidget* w;
    double     from, to, t0, t1;
    void     (*done)(GtkWidget*);
};

static gboolean anim_step(gpointer p) {
    Anim* a = (Anim*)p;
    double t = g_get_monotonic_time() / 1000.0;
    double f = CLAMP((t - a->t0) / (a->t1 - a->t0), 0.0, 1.0);
    gtk_widget_set_opacity(a->w, a->from + (a->to - a->from) * f);
    if (t >= a->t1) {
        if (a->done) a->done(a->w);
        g_object_unref(a->w);
        g_free(a);
        return FALSE;
    }
    return TRUE;
}

static void animate_opacity(GtkWidget* w, double from, double to, int ms,
                            void (*done)(GtkWidget*) = NULL) {
    g_object_ref(w);
    Anim* a = g_new0(Anim, 1);
    a->w = w; a->from = from; a->to = to;
    a->t0 = g_get_monotonic_time() / 1000.0;
    a->t1 = a->t0 + ms;
    a->done = done;
    g_timeout_add(16, anim_step, a);
    gtk_widget_set_opacity(w, from);
}

static void untrack_window(GtkWidget* w, gpointer) {
    auto it = std::find(glass_windows.begin(), glass_windows.end(), w);
    if (it != glass_windows.end()) glass_windows.erase(it);
}

static gboolean anim_tick(gpointer p) {
    anim_t += 0.033;
    for (GtkWidget* w : glass_windows)
        if (gtk_widget_get_realized(w))
            gtk_widget_queue_draw(w);
    return TRUE;
}

/* -------------------------- glass / blur ----------------------------- */

static void apply_kde_blur(GtkWidget* w) {
#ifdef GDK_WINDOWING_X11
    GdkWindow* gw = gtk_widget_get_window(w);
    if (gw && gdk_window_is_viewable(gw)) {
        GdkDisplay* d = gdk_display_get_default();
        if (d && GDK_IS_X11_DISPLAY(d)) {
            Display* dpy = GDK_DISPLAY_XDISPLAY(d);
            Window xid = GDK_WINDOW_XID(gw);
            Atom a = gdk_x11_get_xatom_by_name_for_display(d,
                "_KDE_NET_WM_BLUR_BEHIND_REGION");
            long val = 0;
            XChangeProperty(dpy, xid, a, XA_CARDINAL, 32, PropModeReplace,
                (const unsigned char*)&val, 1);
        }
    }
#else
    (void)w;
#endif
}

static gboolean apply_kde_blur_map(GtkWidget* w, GdkEvent*, gpointer) {
    apply_kde_blur(w);
    return FALSE;
}

static Window menu_toplevel_xid(Window xid, Display* dpy, Window root) {
    Window parent = 0, top = xid;
    Window* kids = NULL; unsigned int nk = 0;
    while (top != root && top != 0) {
        if (!XQueryTree(dpy, top, &root, &parent, &kids, &nk)) break;
        if (kids) XFree(kids);
        if (parent == 0 || parent == root) break;
        top = parent;
    }
    return top;
}

static void apply_menu_blur(GtkWidget* menu) {
#ifdef GDK_WINDOWING_X11
    GdkWindow* gw = gtk_widget_get_window(menu);
    if (gw && GDK_IS_X11_WINDOW(gw)) {
        GdkDisplay* d = gdk_display_get_default();
        Display* dpy = GDK_DISPLAY_XDISPLAY(d);
        Window xid = GDK_WINDOW_XID(gw);
        Window root = XDefaultRootWindow(dpy);
        Window target = menu_toplevel_xid(xid, dpy, root);
        Atom a = gdk_x11_get_xatom_by_name_for_display(d,
            "_KDE_NET_WM_BLUR_BEHIND_REGION");

        gint ox = 0, oy = 0;
        if (target != xid) gdk_window_get_position(gw, &ox, &oy);
        gint W = gdk_window_get_width(gw);
        gint H = gdk_window_get_height(gw);

        const gint R = 14;
        std::vector<long> vals;
        for (gint y = 0; y < H; y++) {
            gint dy = MIN(y, H - 1 - y);
            if (dy >= R) {
                vals.push_back(ox); vals.push_back(oy + y);
                vals.push_back(W); vals.push_back(1);
            } else {
                gint dx = R - (gint)ceil(sqrt((double)(R * R - (R - dy) * (R - dy))));
                if (dx < 0) dx = 0;
                gint x0 = ox + dx, w2 = W - 2 * dx;
                if (w2 > 0) { vals.push_back(x0); vals.push_back(oy + y); vals.push_back(w2); vals.push_back(1); }
            }
        }
        if (!vals.empty())
            XChangeProperty(dpy, target, a, XA_CARDINAL, 32, PropModeReplace,
                (const unsigned char*)vals.data(), (int)vals.size());
    }
#else
    (void)menu;
#endif
}

static gboolean menu_clear_draw(GtkWidget* w, cairo_t* cr, gpointer p) {
    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_restore(cr);
    return FALSE;
}

static gboolean glass_draw(GtkWidget* w, cairo_t* cr, gpointer p) {
    GtkAllocation a;
    gtk_widget_get_allocation(w, &a);
    int W = MAX(a.width, 1);
    int H = MAX(a.height, 1);
    double t = anim_t;

    cairo_set_source_rgba(cr, 0.965, 0.978, 1.0, 0.10);
    cairo_paint(cr);

    double pulse = 0.5 + 0.5 * sin(t * 0.6);
    cairo_pattern_t* rg = cairo_pattern_create_radial(
        W * 0.5, H * 0.34, 0, W * 0.5, H * 0.34, MAX(W, H) * 0.62);
    cairo_pattern_add_color_stop_rgba(rg, 0, 0.92, 1.0, 0.99, 0.03 + 0.02 * pulse);
    cairo_pattern_add_color_stop_rgba(rg, 1, 0.92, 1.0, 0.99, 0.0);
    cairo_set_source(cr, rg);
    cairo_paint(cr);
    cairo_pattern_destroy(rg);

    double sx = fmod(t * 70.0, (double)(W + 700)) - 350.0;
    cairo_pattern_t* lin = cairo_pattern_create_linear(sx, 0, sx + 460, H);
    cairo_pattern_add_color_stop_rgba(lin, 0.0, 1, 1, 1, 0.0);
    cairo_pattern_add_color_stop_rgba(lin, 0.5, 1, 1, 1, 0.02);
    cairo_pattern_add_color_stop_rgba(lin, 1.0, 1, 1, 1, 0.0);
    cairo_set_source(cr, lin);
    cairo_paint(cr);
    cairo_pattern_destroy(lin);

    cairo_set_line_width(cr, 30);
    for (int i = 0; i < 3; i++) {
        double yy = fmod(H * 0.15 + i * H * 0.33 + t * 12.0 * (i % 2 ? 1 : -1), H * 1.25) - H * 0.12;
        cairo_set_source_rgba(cr, 1, 1, 1, 0.014);
        cairo_move_to(cr, 0, yy);
        cairo_curve_to(cr, W * 0.33, yy + 26, W * 0.66, yy - 26, W, yy + 10);
        cairo_stroke(cr);
    }

    cairo_pattern_t* vg = cairo_pattern_create_radial(
        W * 0.5, H * 0.5, MIN(W, H) * 0.35, W * 0.5, H * 0.5, MAX(W, H) * 0.78);
    cairo_pattern_add_color_stop_rgba(vg, 0, 0, 0, 0, 0.0);
    cairo_pattern_add_color_stop_rgba(vg, 1, 0.02, 0.04, 0.08, 0.03);
    cairo_set_source(cr, vg);
    cairo_paint(cr);
    cairo_pattern_destroy(vg);

    return FALSE;
}

static void setup_rgba(GtkWidget* w) {
    GdkScreen* screen = gdk_screen_get_default();
    GdkVisual* visual = gdk_screen_get_rgba_visual(screen);
    if (visual)
        gtk_widget_set_visual(w, visual);
}

static void wire_existing(GtkWidget* w) {
    setup_rgba(w);
    gtk_widget_set_app_paintable(w, TRUE);
    g_signal_connect(w, "draw", G_CALLBACK(glass_draw), NULL);
    g_signal_connect(w, "realize", G_CALLBACK(apply_kde_blur), NULL);
    g_signal_connect(w, "map-event", G_CALLBACK(apply_kde_blur_map), NULL);
    gtk_style_context_add_class(gtk_widget_get_style_context(w), "glass-window");
    glass_windows.push_back(w);
    g_signal_connect(w, "destroy", G_CALLBACK(untrack_window), NULL);
}

static GtkWidget* wire_window(void) {
    GtkWidget* w = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    wire_existing(w);
    return w;
}

static void enter_fade(GtkWidget* w) {
    animate_opacity(w, 0.0, 1.0, 260);
}

/* ------------------------------- model ------------------------------ */

static Doc* doc_of_page(GtkWidget* page) {
    return (Doc*)g_object_get_data(G_OBJECT(page), "doc");
}

static Doc* current_doc(void) {
    if (!notebook) return NULL;
    gint idx = gtk_notebook_get_current_page(GTK_NOTEBOOK(notebook));
    if (idx < 0) return NULL;
    GtkWidget* page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(notebook), idx);
    return page ? doc_of_page(page) : NULL;
}

static gchar* doc_name(Doc* d) {
    if (d->path.empty() && d->name.empty()) return g_strdup("Untitled");
    if (!d->name.empty()) return g_strdup(d->name.c_str());
    return g_path_get_basename(d->path.c_str());
}

static void set_title(void) {
    Doc* d = current_doc();
    if (!d) return;
    gchar* name = doc_name(d);
    std::string title = std::string(name) + (d->dirty ? " *" : "") + " - notepad";
    gtk_window_set_title(GTK_WINDOW(window), title.c_str());
    g_free(name);
}

/* -------------------------------- io --------------------------------- */

static std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static gboolean write_file(const std::string& path, Doc* d) {
    GtkTextIter start, end;
    gtk_text_buffer_get_start_iter(d->buffer, &start);
    gtk_text_buffer_get_end_iter(d->buffer, &end);
    gchar* text = gtk_text_buffer_get_text(d->buffer, &start, &end, FALSE);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    gboolean ok = f.good();
    if (ok) f << text;
    g_free(text);
    return ok;
}

static char* file_dialog(GtkFileChooserAction action, Doc* d);

/* ---------------------------- glass prompt --------------------------- */

enum { R_CANCEL = 0, R_DISCARD = 1, R_YES = 1, R_SAVE = 2 };

struct PBtn {
    const char* label;
    int         resp;
    gboolean    primary;
};

static int       prompt_resp = R_CANCEL;
static GtkWidget* prompt_win = NULL;

static void prompt_end(GtkWidget* win, int resp) {
    prompt_resp = resp;
    gtk_widget_destroy(win);
    gtk_main_quit();
}

static void prompt_btn(GtkWidget* btn, gpointer p) {
    prompt_end(prompt_win, GPOINTER_TO_INT(p));
}

static gboolean prompt_esc(GtkWidget* w, GdkEventKey* e, gpointer p) {
    if (e->keyval == GDK_KEY_Escape) { prompt_end(prompt_win, R_CANCEL); return TRUE; }
    return FALSE;
}

static gboolean prompt_del(GtkWidget* w, GdkEvent*, gpointer p) {
    prompt_end(prompt_win, R_CANCEL);
    return TRUE;
}

static int prompt_run(GtkWindow* parent, const char* title, const char* message,
                      const PBtn* btns, int nbtns) {
    GtkWidget* win = wire_window();
    prompt_win = win;
    gtk_window_set_title(GTK_WINDOW(win), title);
    if (parent) {
        gtk_window_set_transient_for(GTK_WINDOW(win), parent);
        gtk_window_set_modal(GTK_WINDOW(win), TRUE);
    }
    gtk_window_set_resizable(GTK_WINDOW(win), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(win), 440, -1);
    gtk_container_set_border_width(GTK_CONTAINER(win), 24);
    g_signal_connect(win, "key-press-event", G_CALLBACK(prompt_esc), NULL);
    g_signal_connect(win, "delete-event", G_CALLBACK(prompt_del), NULL);

    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_container_add(GTK_CONTAINER(win), vbox);

    GtkWidget* lbl = gtk_label_new(message);
    gtk_label_set_line_wrap(GTK_LABEL(lbl), TRUE);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
    gtk_widget_set_margin_end(lbl, 4);
    gtk_box_pack_start(GTK_BOX(vbox), lbl, FALSE, FALSE, 0);

    GtkWidget* hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_set_homogeneous(GTK_BOX(hbox), TRUE);
    gtk_box_pack_end(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);

    GtkWidget* primary = NULL;
    for (int i = 0; i < nbtns; i++) {
        GtkWidget* b = gtk_button_new_with_mnemonic(btns[i].label);
        gtk_widget_set_name(b, btns[i].primary ? "glass-primary" : "glass-btn");
        g_signal_connect(b, "clicked", G_CALLBACK(prompt_btn), GINT_TO_POINTER(btns[i].resp));
        gtk_box_pack_start(GTK_BOX(hbox), b, TRUE, TRUE, 0);
        if (btns[i].primary) primary = b;
    }
    gtk_widget_show_all(win);
    enter_fade(win);
    if (primary) gtk_widget_grab_focus(primary);
    prompt_resp = R_CANCEL;
    gtk_main();
    return prompt_resp;
}

static void error_msg(const char* message) {
    PBtn btns[] = { { "_OK", R_YES, TRUE } };
    prompt_run(GTK_WINDOW(window), "notepad", message, btns, 1);
}

/* --------------------------- name prompt ----------------------------- */

static gboolean name_ok = FALSE;
static GtkWidget* name_win = NULL;
static GtkWidget* name_entry = NULL;
static gchar* name_result = NULL;

static void name_end(gboolean ok) {
    name_ok = ok;
    gtk_widget_destroy(name_win);
    gtk_main_quit();
}

static void name_btn_clicked(GtkWidget* btn, gpointer p) {
    gboolean ok = GPOINTER_TO_INT(p);
    if (ok) {
        g_free(name_result);
        name_result = gtk_editable_get_chars(GTK_EDITABLE(name_entry), 0, -1);
    }
    name_end(ok);
}

static gboolean name_esc(GtkWidget* w, GdkEventKey* e, gpointer p) {
    if (e->keyval == GDK_KEY_Escape) { name_end(FALSE); return TRUE; }
    return FALSE;
}

static gboolean name_del(GtkWidget* w, GdkEvent*, gpointer p) {
    name_end(FALSE);
    return TRUE;
}

static gchar* name_prompt(const char* title, const char* initial) {
    GtkWidget* win = wire_window();
    name_win = win;
    gtk_window_set_title(GTK_WINDOW(win), title);
    gtk_window_set_transient_for(GTK_WINDOW(win), GTK_WINDOW(window));
    gtk_window_set_modal(GTK_WINDOW(win), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(win), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(win), 360, -1);
    gtk_container_set_border_width(GTK_CONTAINER(win), 24);
    g_signal_connect(win, "key-press-event", G_CALLBACK(name_esc), NULL);
    g_signal_connect(win, "delete-event", G_CALLBACK(name_del), NULL);

    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_container_add(GTK_CONTAINER(win), vbox);

    GtkWidget* lbl = gtk_label_new("Tab name:");
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
    gtk_box_pack_start(GTK_BOX(vbox), lbl, FALSE, FALSE, 0);

    GtkWidget* entry = gtk_entry_new();
    name_entry = entry;
    if (initial) gtk_entry_set_text(GTK_ENTRY(entry), initial);
    gtk_box_pack_start(GTK_BOX(vbox), entry, FALSE, FALSE, 0);

    GtkWidget* hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_set_homogeneous(GTK_BOX(hbox), TRUE);
    gtk_box_pack_end(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);

    GtkWidget* cancel = gtk_button_new_with_mnemonic("_Cancel");
    gtk_widget_set_name(cancel, "glass-btn");
    g_signal_connect(cancel, "clicked", G_CALLBACK(name_btn_clicked), GINT_TO_POINTER(FALSE));
    gtk_box_pack_start(GTK_BOX(hbox), cancel, TRUE, TRUE, 0);

    GtkWidget* ok = gtk_button_new_with_mnemonic("_Confirm");
    gtk_widget_set_name(ok, "glass-primary");
    g_signal_connect(ok, "clicked", G_CALLBACK(name_btn_clicked), GINT_TO_POINTER(TRUE));
    gtk_box_pack_start(GTK_BOX(hbox), ok, TRUE, TRUE, 0);

    gtk_widget_show_all(win);
    enter_fade(win);
    name_ok = FALSE;
    g_free(name_result);
    name_result = NULL;
    gtk_widget_grab_focus(entry);
    gtk_main();
    if (!name_ok) { g_free(name_result); return NULL; }
    gchar* out = name_result;
    name_result = NULL;
    return out;
}

static gboolean save_doc(Doc* d, gboolean force_picker);

static int ask_save_prompt(Doc* d) {
    gchar* name = doc_name(d);
    gchar* msg = g_strdup_printf("Save changes to \xe2\x80\x9c%s\xe2\x80\x9d?", name);
    PBtn btns[] = { { "_Cancel", R_CANCEL, FALSE }, { "_Discard", R_DISCARD, FALSE }, { "_Save", R_SAVE, TRUE } };
    int r = prompt_run(GTK_WINDOW(window), "Save changes?", msg, btns, 3);
    g_free(msg);
    g_free(name);
    return r;
}

static gboolean confirm_doc(Doc* d) {
    if (!d->dirty) return TRUE;
    int r = ask_save_prompt(d);
    if (r == R_SAVE)    return save_doc(d, FALSE);
    if (r == R_DISCARD) return TRUE;
    return FALSE;
}

/* ------------------------------ picker ------------------------------- */

struct PickerEntry {
    std::string name;
    bool        is_dir;
    gint64      size;
};

struct Picker {
    GtkWidget*         win;
    GtkWidget*         list;
    GtkWidget*         path_entry;
    GtkWidget*         name_entry;
    std::string        dir;
    std::vector<PickerEntry> entries;
    std::string        selected;
    unsigned           filter;
    bool               show_hidden;
    bool               save_mode;
    int                resp;
    std::string        result;
    GtkWidget*         chips[6];
    Picker()
        : win(NULL), list(NULL), path_entry(NULL), name_entry(NULL),
          filter(1u), show_hidden(false), save_mode(false), resp(0)
    {
        for (int i = 0; i < 6; i++) chips[i] = NULL;
    }
};

static std::string norm_dir(std::string d) {
    if (d.empty()) {
        const char* h = g_get_home_dir();
        d = h ? h : "/";
    }
    if (d.back() != '/') d += '/';
    return d;
}

static char* human_size(gint64 s) {
    if (s < 1024)       return g_strdup_printf("%lld B", (long long)s);
    if (s < 1048576)    return g_strdup_printf("%.1f KB", s / 1024.0);
    if (s < 1073741824LL) return g_strdup_printf("%.1f MB", s / 1048576.0);
    return g_strdup_printf("%.1f GB", s / 1073741824.0);
}

static bool match_filter(const std::string& nm, unsigned mask) {
    if (mask == 0 || (mask & 1u)) return true;
    std::string lower = nm;
    std::transform(lower.begin(), lower.end(), lower.begin(),
        [](unsigned char c) { return (char)tolower(c); });
    auto has = [&](std::initializer_list<const char*> exts) -> bool {
        for (const char* e : exts) {
            size_t n = strlen(e);
            if (lower.size() > n && lower.compare(lower.size() - n, n, e) == 0) return true;
        }
        return false;
    };
    if ((mask & 2u) && has({".txt", ".log"}))                       return true;
    if ((mask & 4u) && has({".json"}))                              return true;
    if ((mask & 8u) && has({".csv"}))                               return true;
    if ((mask & 16u) && has({".md", ".markdown"}))                  return true;
    if ((mask & 32u) && has({".c",".cpp",".h",".hpp",".py",".js",".ts",".sh",
                             ".xml",".yaml",".yml",".ini",".conf",".toml",".rs",".go",".java",".css",".html"}))
        return true;
    return false;
}

static GtkWidget* icon_theme_pix(GtkWidget* img, const char* name, int sz) {
    GtkIconTheme* theme = gtk_icon_theme_get_default();
    GdkPixbuf* pb = gtk_icon_theme_load_icon(theme, name, sz, GTK_ICON_LOOKUP_FORCE_SIZE, NULL);
    if (pb) {
        gtk_image_set_from_pixbuf(GTK_IMAGE(img), pb);
        g_object_unref(pb);
    } else {
        gtk_image_set_from_icon_name(GTK_IMAGE(img), name, GTK_ICON_SIZE_BUTTON);
    }
    return img;
}

static void picker_row(Picker* pk, size_t i) {
    const PickerEntry& e = pk->entries[i];
    GtkWidget* row = gtk_list_box_row_new();
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
    g_object_set_data(G_OBJECT(row), "entry", GINT_TO_POINTER((gint)i));

    GtkWidget* hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_margin_top(row, 1);
    gtk_widget_set_margin_bottom(row, 1);
    gtk_widget_set_margin_start(row, 4);
    gtk_widget_set_margin_end(row, 4);
    gtk_container_add(GTK_CONTAINER(row), hb);

    GtkWidget* icon = gtk_image_new();
    icon_theme_pix(icon, e.is_dir ? "folder" : "text-x-generic", 20);
    gtk_box_pack_start(GTK_BOX(hb), icon, FALSE, FALSE, 0);

    GtkWidget* vb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
    gtk_box_pack_start(GTK_BOX(hb), vb, TRUE, TRUE, 0);

    GtkWidget* name = gtk_label_new(e.name.c_str());
    gtk_label_set_xalign(GTK_LABEL(name), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_MIDDLE);
    gtk_box_pack_start(GTK_BOX(vb), name, FALSE, FALSE, 0);

    gchar* sub_txt = e.is_dir ? g_strdup("Folder") : human_size(e.size);
    GtkWidget* sub = gtk_label_new(sub_txt);
    gtk_label_set_xalign(GTK_LABEL(sub), 0.0);
    gtk_widget_set_name(sub, "list-sub");
    gtk_box_pack_start(GTK_BOX(vb), sub, FALSE, FALSE, 0);
    g_free(sub_txt);

    gtk_widget_show_all(row);
    gtk_list_box_insert(GTK_LIST_BOX(pk->list), row, -1);
    gtk_widget_set_visible(row, e.is_dir || match_filter(e.name, pk->filter));
}

static void picker_refresh(Picker* pk) {
    /* remove rows */
    GtkListBoxRow* r;
    while ((r = gtk_list_box_get_row_at_index(GTK_LIST_BOX(pk->list), 0)))
        gtk_container_remove(GTK_CONTAINER(pk->list), GTK_WIDGET(r));

    pk->entries.clear();
    DIR* dp = opendir(pk->dir.c_str());
    if (dp) {
        struct dirent* de;
        while ((de = readdir(dp)) != NULL) {
            std::string nm = de->d_name;
            if (nm == "." || nm == "..") continue;
            if (!pk->show_hidden && !nm.empty() && nm[0] == '.') continue;
            PickerEntry e; e.name = nm; e.is_dir = false; e.size = 0;
            struct stat st;
            if (stat((pk->dir + nm).c_str(), &st) == 0) {
                e.is_dir = S_ISDIR(st.st_mode);
                e.size = (gint64)st.st_size;
            }
            pk->entries.push_back(e);
        }
        closedir(dp);
    }
    std::sort(pk->entries.begin(), pk->entries.end(),
        [](const PickerEntry& a, const PickerEntry& b) {
            if (a.is_dir != b.is_dir) return a.is_dir;
            return g_utf8_collate(a.name.c_str(), b.name.c_str()) < 0;
        });
    for (size_t i = 0; i < pk->entries.size(); i++)
        picker_row(pk, i);
    pk->selected.clear();
}

static void picker_cd(Picker* pk, std::string path) {
    path = norm_dir(path);
    DIR* dp = opendir(path.c_str());
    if (!dp) { error_msg(path.c_str()); return; }
    closedir(dp);
    pk->dir = path;
    gtk_entry_set_text(GTK_ENTRY(pk->path_entry), path.c_str());
    picker_refresh(pk);
}

static void picker_up(Picker* pk) {
    std::string p = pk->dir;
    if (p == "/") return;
    if (p.size() > 1 && p.back() == '/') p.erase(p.size() - 1);
    size_t pos = p.rfind('/');
    picker_cd(pk, pos == std::string::npos ? "/" : p.substr(0, pos + 1));
}

static void picker_accept(Picker* pk) {
    std::string target;
    if (pk->save_mode) {
        const gchar* t = gtk_entry_get_text(GTK_ENTRY(pk->name_entry));
        if (!t || !*t) { error_msg("Type a file name first."); return; }
        std::string nm = t;
        if (nm.empty()) { error_msg("Type a file name first."); return; }
        target = nm[0] == '/' ? nm : pk->dir + nm;
        if (access(target.c_str(), F_OK) == 0) {
            gchar* base = g_path_get_basename(target.c_str());
            gchar* msg = g_strdup_printf(
                "A file named \xe2\x80\x9c%s\xe2\x80\x9d already exists.\nReplace it?", base);
            PBtn btns[] = { { "_Cancel", R_CANCEL, FALSE }, { "_Replace", R_YES, TRUE } };
            int r = prompt_run(GTK_WINDOW(pk->win), "Replace file?", msg, btns, 2);
            g_free(msg); g_free(base);
            if (r != R_YES) return;
        }
    } else {
        if (pk->selected.empty()) { error_msg("Select a file first."); return; }
        target = pk->dir + pk->selected;
    }
    pk->result = target;
    pk->resp = GTK_RESPONSE_ACCEPT;
    gtk_widget_destroy(pk->win);
    gtk_main_quit();
}

static void picker_open(Picker* pk) {
    GtkListBoxRow* sel = gtk_list_box_get_selected_row(GTK_LIST_BOX(pk->list));
    gint idx = sel ? GPOINTER_TO_INT(g_object_get_data(G_OBJECT(sel), "entry")) : -1;
    if (idx >= 0 && pk->entries[idx].is_dir) { picker_cd(pk, pk->dir + pk->entries[idx].name); return; }
    picker_accept(pk);
}

static void picker_row_activated(GtkListBox* box, GtkListBoxRow* row, gpointer p) {
    Picker* pk = (Picker*)p;
    gint idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "entry"));
    if (idx < 0) return;
    const PickerEntry& e = pk->entries[idx];
    if (e.is_dir) { picker_cd(pk, pk->dir + e.name); }
    else if (pk->save_mode) {
        gtk_entry_set_text(GTK_ENTRY(pk->name_entry), e.name.c_str());
        gtk_widget_grab_focus(pk->name_entry);
    } else {
        pk->selected = e.name;
        picker_accept(pk);
    }
}

static void picker_selected(GtkListBox* box, GtkListBoxRow* row, gpointer p) {
    Picker* pk = (Picker*)p;
    if (!row) return;
    gint idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "entry"));
    if (idx < 0) return;
    const PickerEntry& e = pk->entries[idx];
    if (e.is_dir) { pk->selected.clear(); }
    else {
        pk->selected = e.name;
        if (pk->save_mode) gtk_entry_set_text(GTK_ENTRY(pk->name_entry), e.name.c_str());
    }
}

static gboolean picker_esc(GtkWidget* w, GdkEventKey* e, gpointer p) {
    if (e->keyval == GDK_KEY_Escape) {
        Picker* pk = (Picker*)p;
        pk->resp = GTK_RESPONSE_CANCEL;
        gtk_widget_destroy(pk->win);
        gtk_main_quit();
        return TRUE;
    }
    return FALSE;
}

static gboolean picker_del(GtkWidget* w, GdkEvent*, gpointer p) {
    Picker* pk = (Picker*)p;
    pk->resp = GTK_RESPONSE_CANCEL;
    gtk_widget_destroy(pk->win);
    gtk_main_quit();
    return TRUE;
}

static void picker_cancel(Picker* pk) {
    pk->resp = GTK_RESPONSE_CANCEL;
    gtk_widget_destroy(pk->win);
    gtk_main_quit();
}

static void picker_place(GtkWidget* btn, gpointer p) {
    Picker* pk = (Picker*)p;
    const char* path = (const char*)g_object_get_data(G_OBJECT(btn), "path");
    picker_cd(pk, path);
}

static void picker_filter(GtkToggleButton* tb, gpointer p) {
    Picker* pk = (Picker*)p;
    int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(tb), "filter"));
    bool active = gtk_toggle_button_get_active(tb);

    if (idx == 0) {
        if (active) pk->filter = 1u;
        else if (pk->filter != 1u) pk->filter = 1u;
    } else {
        unsigned bit = 1u << idx;
        if (active) {
            pk->filter |= bit;
            pk->filter &= ~1u;
        } else {
            pk->filter &= ~bit;
            if (pk->filter == 0 || pk->filter == (1u << 0))
                pk->filter = 1u;
        }
    }

    for (int i = 0; i < 6; i++) {
        if (!pk->chips[i]) continue;
        gboolean want = (pk->filter >> i) & 1u;
        if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(pk->chips[i])) != want) {
            g_signal_handlers_block_by_func(pk->chips[i], (gpointer)picker_filter, pk);
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(pk->chips[i]), want);
            g_signal_handlers_unblock_by_func(pk->chips[i], (gpointer)picker_filter, pk);
        }
    }
    picker_refresh(pk);
}

static void picker_hidden(GtkToggleButton* tb, gpointer p) {
    Picker* pk = (Picker*)p;
    pk->show_hidden = gtk_toggle_button_get_active(tb);
    picker_refresh(pk);
}

static char* picker_run(int action, Doc* d) {
    bool save = action == GTK_FILE_CHOOSER_ACTION_SAVE;
    Picker* pk = new Picker();
    pk->save_mode = save;

    GtkWidget* win2 = wire_window();
    pk->win = win2;
    gtk_window_set_title(GTK_WINDOW(win2), save ? "Save As" : "Open File");
    gtk_window_set_transient_for(GTK_WINDOW(win2), GTK_WINDOW(window));
    gtk_window_set_modal(GTK_WINDOW(win2), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(win2), 940, 560);
    gtk_container_set_border_width(GTK_CONTAINER(win2), 20);
    g_signal_connect(win2, "key-press-event", G_CALLBACK(picker_esc), pk);
    g_signal_connect(win2, "delete-event", G_CALLBACK(picker_del), pk);

    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_add(GTK_CONTAINER(win2), vbox);

    /* path row */
    GtkWidget* prow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(vbox), prow, FALSE, FALSE, 0);

    GtkWidget* up = gtk_button_new_with_label("\xe2\x86\x91");
    gtk_widget_set_name(up, "chip-btn");
    gtk_widget_set_tooltip_text(up, "Go up");
    g_signal_connect(up, "clicked", G_CALLBACK(+[](GtkWidget*, gpointer p) {
        picker_up((Picker*)p);
    }), pk);
    gtk_box_pack_start(GTK_BOX(prow), up, FALSE, FALSE, 0);

    pk->path_entry = gtk_entry_new();
    gtk_box_pack_start(GTK_BOX(prow), pk->path_entry, TRUE, TRUE, 0);
    GtkWidget* gosl = gtk_button_new_with_label("Go");
    gtk_widget_set_name(gosl, "glass-primary");
    g_signal_connect(gosl, "clicked", G_CALLBACK(+[](GtkWidget*, gpointer p) {
        Picker* q = (Picker*)p;
        const gchar* t = gtk_entry_get_text(GTK_ENTRY(q->path_entry));
        picker_cd(q, t ? t : "");
    }), pk);
    gtk_box_pack_start(GTK_BOX(prow), gosl, FALSE, FALSE, 0);
    g_signal_connect(pk->path_entry, "activate", G_CALLBACK(+[](GtkWidget* e, gpointer p) {
        Picker* q = (Picker*)p;
        picker_cd(q, gtk_entry_get_text(GTK_ENTRY(e)));
    }), pk);

    /* places */
    GtkWidget* places = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(vbox), places, FALSE, FALSE, 0);
    GtkWidget* homeb = gtk_button_new_with_label("Home");
    gtk_widget_set_name(homeb, "chip-btn");
    g_object_set_data(G_OBJECT(homeb), "path", g_strdup(g_get_home_dir()));
    g_signal_connect(homeb, "clicked", G_CALLBACK(picker_place), pk);
    gtk_box_pack_start(GTK_BOX(places), homeb, FALSE, FALSE, 0);
    struct Place { GUserDirectory dir; const char* label; };
    static const Place places_list[] = {
        { G_USER_DIRECTORY_DOCUMENTS, "Documents" },
        { G_USER_DIRECTORY_DOWNLOAD,  "Downloads" },
        { G_USER_DIRECTORY_DESKTOP,   "Desktop" },
        { G_USER_DIRECTORY_PICTURES,  "Pictures" },
        { G_USER_DIRECTORY_MUSIC,     "Music" },
        { G_USER_DIRECTORY_VIDEOS,    "Videos" },
    };
    for (size_t i = 0; i < G_N_ELEMENTS(places_list); i++) {
        const gchar* d = g_get_user_special_dir(places_list[i].dir);
        if (!d) continue;
        GtkWidget* b = gtk_button_new_with_label(places_list[i].label);
        gtk_widget_set_name(b, "chip-btn");
        g_object_set_data(G_OBJECT(b), "path", g_strdup(d));
        g_signal_connect(b, "clicked", G_CALLBACK(picker_place), pk);
        gtk_box_pack_start(GTK_BOX(places), b, FALSE, FALSE, 0);
    }
    GtkWidget* roo = gtk_button_new_with_label("/");
    gtk_widget_set_name(roo, "chip-btn");
    g_object_set_data(G_OBJECT(roo), "path", g_strdup("/"));
    g_signal_connect(roo, "clicked", G_CALLBACK(picker_place), pk);
    gtk_box_pack_start(GTK_BOX(places), roo, FALSE, FALSE, 0);

    /* list */
    GtkWidget* sw2 = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw2), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start(GTK_BOX(vbox), sw2, TRUE, TRUE, 0);
    pk->list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(pk->list), GTK_SELECTION_SINGLE);
    gtk_container_add(GTK_CONTAINER(sw2), pk->list);
    g_signal_connect(pk->list, "row-activated", G_CALLBACK(picker_row_activated), pk);
    g_signal_connect(pk->list, "row-selected", G_CALLBACK(picker_selected), pk);

    /* filter / hidden */
    if (!save) {
        GtkWidget* fro = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        GtkWidget* labels[6] = { NULL };
        gtk_box_pack_start(GTK_BOX(vbox), fro, FALSE, FALSE, 0);
        const char* fnames[] = {"All","Text","JSON","CSV","Markdown","Code"};
        for (int i = 0; i < 6; i++) {
            GtkWidget* t = gtk_toggle_button_new_with_label(fnames[i]);
            gtk_widget_set_name(t, "filter-chip");
            g_object_set_data(G_OBJECT(t), "filter", GINT_TO_POINTER(i));
            g_signal_connect(t, "toggled", G_CALLBACK(picker_filter), pk);
            pk->chips[i] = t;
            if (i == 0) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(t), TRUE);
            gtk_box_pack_start(GTK_BOX(fro), t, FALSE, FALSE, 0);
        }
        GtkWidget* hid = gtk_check_button_new_with_label("Show hidden");
        gtk_widget_set_name(hid, "hidden-check");
        g_signal_connect(hid, "toggled", G_CALLBACK(picker_hidden), pk);
        gtk_box_pack_end(GTK_BOX(fro), hid, FALSE, FALSE, 0);
    }

    /* bottom row */
    GtkWidget* bottom = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_end(GTK_BOX(vbox), bottom, FALSE, FALSE, 0);

    GtkWidget* cancel = gtk_button_new_with_mnemonic("_Cancel");
    gtk_widget_set_name(cancel, "glass-btn");
    g_signal_connect(cancel, "clicked", G_CALLBACK(+[](GtkWidget*, gpointer p){ picker_cancel((Picker*)p); }), pk);
    gtk_box_pack_start(GTK_BOX(bottom), cancel, FALSE, FALSE, 0);

    GtkWidget* sep = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
    gtk_box_pack_start(GTK_BOX(bottom), sep, TRUE, TRUE, 0);

    if (save) {
        pk->name_entry = gtk_entry_new();
        gtk_box_pack_start(GTK_BOX(bottom), pk->name_entry, TRUE, TRUE, 0);
        const gchar* hint = "file name";
        gtk_entry_set_placeholder_text(GTK_ENTRY(pk->name_entry), hint);
    }

    GtkWidget* openb = gtk_button_new_with_mnemonic(save ? "_Save" : "_Open");
    gtk_widget_set_name(openb, "glass-primary");
    g_signal_connect(openb, "clicked", G_CALLBACK(+[](GtkWidget*, gpointer p){ picker_open((Picker*)p); }), pk);
    gtk_box_pack_start(GTK_BOX(bottom), openb, FALSE, FALSE, 0);

    /* initial dir + name */
    if (d && !d->path.empty()) {
        gchar* dirname = g_path_get_dirname(d->path.c_str());
        pk->dir = norm_dir(dirname);
        g_free(dirname);
        if (save) {
            gchar* bn = g_path_get_basename(d->path.c_str());
            gtk_entry_set_text(GTK_ENTRY(pk->name_entry), bn);
            g_free(bn);
        }
    }
    if (pk->dir.empty()) pk->dir = norm_dir(g_get_home_dir());

    gtk_widget_show_all(win2);
    gtk_entry_set_text(GTK_ENTRY(pk->path_entry), pk->dir.c_str());
    enter_fade(win2);
    picker_refresh(pk);
    if (save) gtk_widget_grab_focus(pk->name_entry);
    else gtk_widget_grab_focus(pk->list);

    gtk_main();

    char* result = NULL;
    if (pk->resp == GTK_RESPONSE_ACCEPT && !pk->result.empty())
        result = g_strdup(pk->result.c_str());
    delete pk;
    return result;
}

static char* file_dialog(GtkFileChooserAction action, Doc* d) {
    return picker_run(action, d);
}

/* ------------------------------- save -------------------------------- */

static gboolean save_doc(Doc* d, gboolean force_picker) {
    if (force_picker || d->path.empty()) {
        char* p2 = file_dialog(GTK_FILE_CHOOSER_ACTION_SAVE, d);
        if (!p2) return FALSE;
        d->path = p2;
        g_free(p2);
    }
    if (!write_file(d->path, d)) {
        error_msg(d->path.c_str());
        return FALSE;
    }
    d->dirty = FALSE;
    gtk_text_buffer_set_modified(d->buffer, FALSE);
    set_title();
    return TRUE;
}

static void do_open(void) {
    char* p3 = file_dialog(GTK_FILE_CHOOSER_ACTION_OPEN, current_doc());
    if (!p3) return;
    std::string data = read_file(p3);
    FILE* chk = fopen(p3, "rb");
    gboolean readable_ok = chk != NULL;
    if (chk) fclose(chk);
    if (!readable_ok) {
        error_msg(p3);
        g_free(p3);
        return;
    }
    if (data.empty() && readable_ok) {
        /* empty readable file: just load (no-op text) */
    }
    Doc* cur = current_doc();
    if (cur && cur->path.empty() && !cur->dirty && !gtk_text_buffer_get_modified(cur->buffer)) {
        GtkTextIter s, e;
        gtk_text_buffer_get_start_iter(cur->buffer, &s);
        gtk_text_buffer_get_end_iter(cur->buffer, &e);
        gchar* t = gtk_text_buffer_get_text(cur->buffer, &s, &e, FALSE);
        bool empty = t[0] == '\0';
        g_free(t);
        if (empty) {
            cur->path = p3;
            gtk_text_buffer_set_text(cur->buffer, data.c_str(), data.size());
            gtk_text_buffer_set_modified(cur->buffer, FALSE);
            set_title();
            g_free(p3);
            return;
        }
    }
    new_tab(data, p3);
    g_free(p3);
}

static gboolean quit_app(void) {
    gint n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(notebook));
    for (gint i = 0; i < n; i++) {
        GtkWidget* page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(notebook), i);
        Doc* d = doc_of_page(page);
        if (!confirm_doc(d)) return FALSE;
    }
    return TRUE;
}

/* ------------------------------ tabs -------------------------------- */

static void refresh_active_tab_to(GtkWidget* cur);
static void refresh_active_tab(void);

static void on_changed(GtkTextBuffer* b, Doc* d) {
    d->dirty = gtk_text_buffer_get_modified(b);
    if (d == current_doc()) set_title();
}

static void on_switch(GtkNotebook* nb, GtkWidget* page, guint n, gpointer p) {
    set_title();
    refresh_active_tab_to(page);
}

static void set_active_tab(GtkWidget* box, gboolean active) {
    GtkStyleContext* sc = gtk_widget_get_style_context(box);
    if (active) gtk_style_context_add_class(sc, "active-tab");
    else        gtk_style_context_remove_class(sc, "active-tab");
}

static void refresh_active_tab_to(GtkWidget* cur) {
    gint n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(notebook));
    for (gint i = 0; i < n; i++) {
        GtkWidget* p = gtk_notebook_get_nth_page(GTK_NOTEBOOK(notebook), i);
        GtkWidget* box = gtk_notebook_get_tab_label(GTK_NOTEBOOK(notebook), p);
        set_active_tab(box, p == cur);
    }
}

static void refresh_active_tab(void) {
    refresh_active_tab_to(gtk_notebook_get_nth_page(GTK_NOTEBOOK(notebook),
        gtk_notebook_get_current_page(GTK_NOTEBOOK(notebook))));
}

static void update_tab_label(GtkWidget* page) {
    GtkWidget* box = gtk_notebook_get_tab_label(GTK_NOTEBOOK(notebook), page);
    GtkWidget* lbl = (GtkWidget*)g_object_get_data(G_OBJECT(box), "tab-label");
    Doc* d = doc_of_page(page);
    gchar* name = doc_name(d);
    gtk_label_set_text(GTK_LABEL(lbl), name);
    g_free(name);
}

static void close_doc_after_fade(GtkWidget* page) {
    Doc* d = (Doc*)g_object_get_data(G_OBJECT(page), "doc");
    gtk_notebook_remove_page(GTK_NOTEBOOK(notebook),
        gtk_notebook_page_num(GTK_NOTEBOOK(notebook), page));
    delete d;
    if (gtk_notebook_get_n_pages(GTK_NOTEBOOK(notebook)) == 0)
        new_tab("", "");
    set_title();
}

static void close_doc(GtkWidget* btn, Doc* d) {
    if (!confirm_doc(d)) return;
    GtkWidget* page = NULL;
    gint n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(notebook));
    for (gint i = 0; i < n; i++) {
        GtkWidget* p2 = gtk_notebook_get_nth_page(GTK_NOTEBOOK(notebook), i);
        if (doc_of_page(p2) == d) { page = p2; break; }
    }
    if (!page) {
        if (gtk_notebook_get_n_pages(GTK_NOTEBOOK(notebook)) == 0) new_tab("", "");
        delete d;
        set_title();
        return;
    }
    animate_opacity(page, 1.0, 0.0, 150, close_doc_after_fade);
}

static GtkWidget* page_of_doc(Doc* d) {
    gint n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(notebook));
    for (gint i = 0; i < n; i++) {
        GtkWidget* p2 = gtk_notebook_get_nth_page(GTK_NOTEBOOK(notebook), i);
        if (doc_of_page(p2) == d) return p2;
    }
    return NULL;
}

static void rename_doc(Doc* d) {
    gchar* cur = doc_name(d);
    gchar* nm = name_prompt("Rename Tab", cur);
    g_free(cur);
    if (!nm) return;
    d->name = nm;
    g_free(nm);
    GtkWidget* page = page_of_doc(d);
    if (page) update_tab_label(page);
    set_title();
}

static gboolean tab_click(GtkWidget* box, GdkEventButton* ev, Doc* d) {
    if (ev->type == GDK_2BUTTON_PRESS) { rename_doc(d); return TRUE; }
    if (ev->type == GDK_BUTTON_PRESS && ev->button == 3) { rename_doc(d); return TRUE; }
    return FALSE;
}

static GtkWidget* make_tab_label(Doc* d) {
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_name(box, "tab-box");
    GtkWidget* lbl = gtk_label_new(doc_name(d));
    GtkWidget* btn = gtk_button_new_with_label("\xe2\x9c\x95");
    gtk_widget_set_name(btn, "tab-close");
    gtk_button_set_relief(GTK_BUTTON(btn), GTK_RELIEF_NONE);
    gtk_widget_set_focus_on_click(btn, FALSE);
    gtk_box_pack_start(GTK_BOX(box), lbl, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(box), btn, FALSE, FALSE, 0);
    g_object_set_data(G_OBJECT(box), "tab-label", lbl);
    (void)d;
    g_signal_connect(box, "button-press-event", G_CALLBACK(tab_click), d);
    g_signal_connect(btn, "clicked", G_CALLBACK(close_doc), d);
    gtk_widget_show_all(box);
    return box;
}

static GtkWidget* add_tab(Doc* d, const std::string& content) {
    GtkWidget* sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);

    GtkWidget* tv = gtk_text_view_new();
    d->buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv));
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(tv), GTK_WRAP_WORD_CHAR);
    g_signal_connect(d->buffer, "changed", G_CALLBACK(on_changed), d);
    gtk_container_add(GTK_CONTAINER(sw), tv);

    g_object_set_data(G_OBJECT(sw), "doc", d);
    d->dirty = FALSE;
    gtk_text_buffer_set_text(d->buffer, content.c_str(), content.size());
    gtk_text_buffer_set_modified(d->buffer, FALSE);

    gint idx = gtk_notebook_append_page(GTK_NOTEBOOK(notebook), sw, make_tab_label(d));
    gtk_notebook_set_tab_reorderable(GTK_NOTEBOOK(notebook), sw, TRUE);
    gtk_widget_show_all(sw);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook), idx);
    refresh_active_tab();
    gtk_widget_grab_focus(tv);
    gtk_widget_set_opacity(sw, 0.0);
    animate_opacity(sw, 0.0, 1.0, 180);
    set_title();
    return sw;
}

static Doc* new_tab(const std::string& content, const std::string& path) {
    Doc* d = new Doc();
    d->path = path;
    add_tab(d, content);
    return d;
}

static void cb_save(void)     { Doc* d = current_doc(); if (d) save_doc(d, FALSE); }
static void cb_save_as(void)  { Doc* d = current_doc(); if (d) save_doc(d, TRUE); }
static void cb_open(void)     { do_open(); }
static void cb_new_tab(void) {
    gchar* nm = name_prompt("New Tab", "");
    if (!nm) return;
    Doc* d = new_tab("", "");
    d->name = nm;
    g_free(nm);
    GtkWidget* page = page_of_doc(d);
    if (page) update_tab_label(page);
    set_title();
}
static void cb_rename_tab(void){ Doc* d = current_doc(); if (d) rename_doc(d); }
static void cb_close_tab(void){ Doc* d = current_doc(); if (d) close_doc(NULL, d); }
static void cb_quit(void)     { if (quit_app()) { gtk_widget_destroy(window); } }

static gboolean on_delete_event(GtkWidget* w, GdkEvent* ev, gpointer p) {
    if (!quit_app()) return TRUE;
    gtk_widget_destroy(window);
    return TRUE;
}

/* ------------------------------ css -------------------------------- */

static const char* CSS =
"@define-color glass_text #000000;\n"
"* { background-color: transparent; border: none; outline: none; }\n"
"window.glass-window, window.notepad-window { background-color: transparent; }\n"
"window.glass-window * { color: @glass_text; }\n"
"menubar { background: rgba(255,255,255,0.05); border-bottom: 1px solid rgba(255,255,255,0.10); padding: 2px 8px; }\n"
"menubar > menuitem { padding: 4px 10px; margin: 1px; border-radius: 8px; transition: background 150ms ease; }\n"
"menubar > menuitem:hover { background: rgba(150,160,175,0.45); }\n"
"menubar > menuitem:selected { background: rgba(255,255,255,0.45); color: #10162b; }\n"
"menu { background: rgba(250,252,255,0.40); border: 1px solid rgba(255,255,255,0.50); border-radius: 14px; padding: 6px; color: #1c2434; box-shadow: 0 14px 44px rgba(20,30,60,0.20); }\n"
"window.background, window.popup.background, window.menu { background-color: transparent; background-image: none; }\n"
"menu menuitem { border-radius: 9px; padding: 7px 14px; }\n"
"menu menuitem:hover { background: rgba(94,180,170,0.35); }\n"
"menu separator { background: rgba(30,40,60,0.12); }\n"
"notebook { background: transparent; padding: 4px 8px 10px; border: none; }\n"
"notebook > header { background: transparent; border: none; }\n"
"notebook > header.top { padding: 8px 12px 4px; }\n"
"notebook header tabs tab, notebook header tabs tab:hover, notebook header tabs tab:selected {\n"
"  background: transparent; border: none; border-radius: 12px; box-shadow: none;\n"
"  padding: 0; margin: 0; color: @glass_text;\n"
"}\n"
"#tab-box {\n"
"  background: rgba(255,255,255,0.14);\n"
"  border: 1px solid rgba(255,255,255,0.35);\n"
"  border-radius: 12px;\n"
"  padding: 5px 12px;\n"
"  margin: 2px 4px;\n"
"  color: @glass_text;\n"
"  box-shadow: none;\n"
"}\n"
"#tab-box:hover { background: rgba(255,255,255,0.28); }\n"
"#tab-box.active-tab, #tab-box.active-tab:hover {\n"
"  background: rgba(255,255,255,0.50);\n"
"  box-shadow: none;\n"
"}\n"
"notebook header .arrow { background: rgba(255,255,255,0.22); border-radius: 8px; padding: 2px; }\n"
"#plus-tab {\n"
"  background: rgba(255,255,255,0.14);\n"
"  border: 1px solid rgba(255,255,255,0.35);\n"
"  border-radius: 10px;\n"
"  padding: 2px 10px;\n"
"  font-size: 18px;\n"
"  color: @glass_text;\n"
"}\n"
"#plus-tab:hover { background: rgba(255,255,255,0.45); box-shadow: 0 4px 14px rgba(30,45,80,0.14); }\n"
"#tab-close { background: transparent; border: none; border-radius: 8px; padding: 1px 6px; color: rgba(28,36,52,0.55); font-size: 11px; }\n"
"#tab-close:hover { background: rgba(30,40,60,0.20); color: #10162b; }\n"
"scrolledwindow { background-color: transparent; border-radius: 14px; border: none; }\n"
"viewport { background-color: transparent; border: none; }\n"
"textview { background-color: transparent; border: none; outline: none; }\n"
"textview text { background-color: rgba(255,255,255,0.15); color: @glass_text; font-family: \"DejaVu Sans Mono\"; font-size: 14px; caret-color: #000000; }\n"
"textview:focus { outline: none; background-color: transparent; }\n"
"textview text selection { background-color: rgba(16,150,140,0.35); color: #0c121f; }\n"
"scrollbar { background: transparent; }\n"
"scrollbar slider { background: rgba(40,60,90,0.20); border-radius: 10px; }\n"
"scrollbar slider:hover { background: rgba(40,60,90,0.38); }\n"
"scrollbar trough { background: transparent; }\n"
"entry { background: rgba(255,255,255,0.12); border: 1px solid rgba(255,255,255,0.35); border-radius: 10px; color: @glass_text; padding: 6px 10px; }\n"
"entry selection { background: rgba(15,157,140,0.35); }\n"
"#glass-btn { background: rgba(255,255,255,0.14); border: 1px solid rgba(255,255,255,0.40); border-radius: 10px; color: @glass_text; padding: 7px 18px; }\n"
"#glass-btn:hover { background: rgba(255,255,255,0.50); box-shadow: 0 4px 14px rgba(30,45,80,0.12); }\n"
"#glass-primary { background: rgba(15,157,140,0.55); border: 1px solid rgba(25,190,170,0.75); border-radius: 10px; color: #ffffff; padding: 7px 18px; }\n"
"#glass-primary:hover { background: rgba(15,157,140,0.75); box-shadow: 0 4px 16px rgba(15,157,140,0.35); }\n"
"#chip-btn { background: rgba(255,255,255,0.12); border: 1px solid rgba(255,255,255,0.35); border-radius: 10px; padding: 4px 12px; color: @glass_text; }\n"
"#chip-btn:hover { background: rgba(255,255,255,0.45); }\n"
"#filter-chip { background: rgba(255,255,255,0.12); border: 1px solid rgba(255,255,255,0.35); border-radius: 10px; padding: 3px 12px; color: @glass_text; }\n"
"#filter-chip:hover { background: rgba(255,255,255,0.35); }\n"
"#filter-chip:checked { background: rgba(15,157,140,0.55); border-color: rgba(15,157,140,0.7); color: #fff; }\n"
"list { background: rgba(255,255,255,0.08); border-radius: 14px; }\n"
"list row { border-radius: 10px; padding: 4px 8px; }\n"
"list row:hover { background: rgba(255,255,255,0.28); }\n"
"list row:selected { background: rgba(15,157,140,0.30); }\n"
"list row:selected label { color: #0c121f; }\n"
"#list-sub { font-size: 11px; color: rgba(28,36,54,0.60); }\n"
"#hidden-check { background: rgba(15,157,140,0.55); border: 1px solid rgba(15,157,140,0.70); border-radius: 10px; padding: 4px 14px; color: #ffffff; }\n"
"#hidden-check:hover { background: rgba(15,157,140,0.75); box-shadow: 0 4px 14px rgba(30,45,80,0.12); }\n"
"#hidden-check check { -gtk-icon-source: none; }\n"
;

/* --------------------------- main content --------------------------- */

static GtkWidget* build_content(GtkAccelGroup* acc) {
    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    GtkWidget* bar = gtk_menu_bar_new();
    GtkWidget* fm  = gtk_menu_new();
    setup_rgba(fm);
    gtk_widget_set_app_paintable(fm, TRUE);
    g_signal_connect(fm, "draw", G_CALLBACK(menu_clear_draw), NULL);
    g_signal_connect(fm, "map-event", G_CALLBACK(apply_menu_blur), NULL);
    g_signal_connect(fm, "show", G_CALLBACK(+[](GtkWidget* m, gpointer) { apply_menu_blur(m); }), NULL);
    GtkWidget* fi  = gtk_menu_item_new_with_mnemonic("_File");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(fi), fm);
    gtk_menu_shell_append(GTK_MENU_SHELL(bar), fi);
    gtk_box_pack_start(GTK_BOX(vbox), bar, FALSE, FALSE, 0);

    struct Mi { const char* label; GCallback cb; guint key; guint mods; };
    static const Mi items[] = {
        { "_New Tab",   G_CALLBACK(cb_new_tab),    GDK_KEY_t, GDK_CONTROL_MASK },
        { "_Open...",   G_CALLBACK(cb_open),       GDK_KEY_o, GDK_CONTROL_MASK },
        { "_Save",      G_CALLBACK(cb_save),       GDK_KEY_s, GDK_CONTROL_MASK },
        { "Save _As...",G_CALLBACK(cb_save_as),    GDK_KEY_s, GDK_CONTROL_MASK|GDK_SHIFT_MASK },
        { "_Close Tab", G_CALLBACK(cb_close_tab),  GDK_KEY_w, GDK_CONTROL_MASK },
        { "_Rename Tab",G_CALLBACK(cb_rename_tab), GDK_KEY_r, GDK_CONTROL_MASK },
    };
    for (int i = 0; i < 6; i++) {
        GtkWidget* mi = gtk_menu_item_new_with_mnemonic(items[i].label);
        gtk_menu_shell_append(GTK_MENU_SHELL(fm), mi);
        g_signal_connect(mi, "activate", items[i].cb, NULL);
        gtk_widget_add_accelerator(mi, "activate", acc, items[i].key, (GdkModifierType)items[i].mods, GTK_ACCEL_VISIBLE);
    }
    gtk_menu_shell_append(GTK_MENU_SHELL(fm), gtk_separator_menu_item_new());
    GtkWidget* quit = gtk_menu_item_new_with_label("Quit");
    gtk_menu_shell_append(GTK_MENU_SHELL(fm), quit);
    g_signal_connect(quit, "activate", G_CALLBACK(cb_quit), NULL);
    gtk_widget_add_accelerator(quit, "activate", acc, GDK_KEY_q, (GdkModifierType)GDK_CONTROL_MASK, GTK_ACCEL_VISIBLE);

    notebook = gtk_notebook_new();
    gtk_box_pack_start(GTK_BOX(vbox), notebook, TRUE, TRUE, 0);
    g_signal_connect(notebook, "switch-page", G_CALLBACK(on_switch), NULL);

    GtkWidget* plus = gtk_button_new_with_label("+");
    gtk_widget_set_name(plus, "plus-tab");
    gtk_widget_set_tooltip_text(plus, "New tab");
    g_signal_connect(plus, "clicked", G_CALLBACK(cb_new_tab), NULL);
    gtk_notebook_set_action_widget(GTK_NOTEBOOK(notebook), plus, GTK_PACK_END);
    gtk_widget_show(plus);

    return vbox;
}

/* ------------------------------- test -------------------------------- */

static gboolean test_ui_capture(gpointer) {
    cairo_surface_write_to_png(gtk_offscreen_window_get_surface(GTK_OFFSCREEN_WINDOW(window)),
        getenv("NOTEPAD_SHOT"));
    gtk_main_quit();
    return FALSE;
}

static int run_test_ui(const char* out) {
    (void)out;
    window = gtk_offscreen_window_new();
    wire_existing(window);
    gtk_style_context_add_class(gtk_widget_get_style_context(window), "notepad-window");
    GtkAccelGroup* acc = gtk_accel_group_new();
    gtk_window_add_accel_group(GTK_WINDOW(window), acc);
    gtk_window_set_default_size(GTK_WINDOW(window), 900, 720);
    gtk_container_add(GTK_CONTAINER(window), build_content(acc));
    gtk_widget_show_all(window);
    const char* src = getenv("NOTEPAD_SRC");
    std::string content = src ? read_file(src) : "";
    new_tab(content, src ? src : "");
    Doc* d0 = current_doc();
    if (d0) {
        GtkTextIter s, e;
        gtk_text_buffer_get_start_iter(d0->buffer, &s);
        gtk_text_buffer_get_end_iter(d0->buffer, &e);
        gchar* t = gtk_text_buffer_get_text(d0->buffer, &s, &e, FALSE);
        g_printerr("[test] src=%s content_len=%zu buffer=%ld\n", src ? src : "(null)", content.size(),
                   t ? (long)g_utf8_strlen(t, -1) : -1L);
        g_free(t);
    }
    const char* src2 = getenv("NOTEPAD_SRC2");
    if (src2)
        new_tab(read_file(src2), src2);
    g_timeout_add(450, test_ui_capture, NULL);
    gtk_main();
    return 0;
}

int main(int argc, char** argv) {
    if (!getenv("NOTEPAD_SHOT"))
        g_setenv("GDK_BACKEND", "x11", TRUE);
    if (!gtk_init_check(&argc, &argv)) {
        g_unsetenv("GDK_BACKEND");
        gtk_init(&argc, &argv);
    }

    GtkCssProvider* css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css, CSS, -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_timeout_add(33, anim_tick, NULL);

    if (getenv("NOTEPAD_SHOT"))
        return run_test_ui(getenv("NOTEPAD_SHOT"));

    window = wire_window();
    gtk_style_context_add_class(gtk_widget_get_style_context(window), "notepad-window");
    gtk_widget_set_size_request(window, 400, 300);
    gtk_window_set_default_size(GTK_WINDOW(window), 960, 680);
    gtk_window_set_title(GTK_WINDOW(window), "notepad");
    g_signal_connect(window, "delete-event", G_CALLBACK(on_delete_event), NULL);
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GError* err = NULL;
    GdkPixbuf* icon = gdk_pixbuf_new_from_file(ICON_PATH, &err);
    if (icon) { gtk_window_set_icon(GTK_WINDOW(window), icon); g_object_unref(icon); }
    else if (err) g_error_free(err);

    GtkAccelGroup* acc = gtk_accel_group_new();
    gtk_window_add_accel_group(GTK_WINDOW(window), acc);
    gtk_container_add(GTK_CONTAINER(window), build_content(acc));

    gtk_widget_show_all(window);
    set_title();
    enter_fade(window);

    if (argc > 1) new_tab(read_file(argv[1]), argv[1]);
    else          new_tab("", "");

    gtk_main();
    return 0;
}