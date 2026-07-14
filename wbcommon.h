// wbcommon.h — shared helpers for the asteroidzman waybar CFFI plugins.
//
// Header-only (static functions; each plugin compiles its own copy):
//   WbPop            content-sized layer-shell popup window under a bar pill
//   wb_themed_pixbuf tint a monochrome SVG to the widget's theme colour
//   wb_icon_*        GtkImage wrapper that re-tints itself on theme change
//   WbReader         line-reader subprocess: dies with the bar (PDEATHSIG),
//                    respawns with backoff if the child exits
//
// Popup behaviour (matches what the plugins converged on):
//   - namespace "waybar-popup" (compositor layerrule blurs it), CSS #cffi-popwin
//   - opens under the pill, centred, clamped on-screen on every edge; the
//     clamped position is computed BEFORE map because gtk-layer-shell margins
//     set after map only reach the compositor with the next buffer commit
//   - closes on Escape, or on focus-loss debounced 180 ms so focus-follows-mouse
//     transit blips (pill → popup crosses another surface) and combo/menu grabs
//     don't dismiss it
//   - singleton: opening any wbcommon popup hides the one previously open,
//     coordinated across plugin .so's via object data on the shared bar toplevel
//   - keyboard: set g_object_set_data(pop.win, "wb-focus", widget) during
//     rebuild to focus that widget on open (arrow/Tab nav from there)

#ifndef WBCOMMON_H
#define WBCOMMON_H

#include <gtk/gtk.h>
#include <gtk-layer-shell.h>
#include <gdk/gdkkeysyms.h>
#include <gio/gio.h>
#include <signal.h>
#include <string.h>
#include <sys/prctl.h>

// ─── WbPop: content-sized layer-shell popup ──────────────────────────────────

typedef struct WbPop WbPop;
typedef void (*WbPopRebuildFn)(gpointer user);
struct WbPop {
  GtkWidget *win;              // the layer-shell toplevel (a GtkBin — add content to it)
  GtkWidget *anchor;           // the bar pill the popup hangs under
  WbPopRebuildFn rebuild;      // fills win with fresh content before each show
  gpointer user;
  int pill_x, pill_w, bar_w, pop_top, armed;
  int last_mx, last_my;        // margins already committed (skip no-op recenters)
  guint close_src;             // debounced focus-loss close
};

#define WBPOP_OPEN_KEY "wbpop-open"   // on the bar toplevel: GtkWidget* of the open popup

static G_GNUC_UNUSED void wbpop_hide(WbPop *p) {
  if (p->close_src) { g_source_remove(p->close_src); p->close_src = 0; }
  gtk_widget_hide(p->win);
  GtkWidget *top = gtk_widget_get_toplevel(p->anchor);
  if (GTK_IS_WINDOW(top) && g_object_get_data(G_OBJECT(top), WBPOP_OPEN_KEY) == p->win)
    g_object_set_data(G_OBJECT(top), WBPOP_OPEN_KEY, NULL);
}

static gboolean wbpop_arm_cb(gpointer d) { ((WbPop *)d)->armed = 1; return G_SOURCE_REMOVE; }

static gboolean wbpop_key_cb(GtkWidget *w, GdkEventKey *e, gpointer d) {
  (void)w;
  if (e->keyval == GDK_KEY_Escape) { wbpop_hide(d); return TRUE; }
  return FALSE;
}

static gboolean wbpop_close_timeout(gpointer d) {
  WbPop *p = d;
  p->close_src = 0;
  if (gtk_widget_get_visible(p->win) && !gtk_grab_get_current() &&
      !gtk_window_has_toplevel_focus(GTK_WINDOW(p->win)))
    wbpop_hide(p);
  return G_SOURCE_REMOVE;
}

// Focus-follows-mouse: moving pointer from pill to popup briefly crosses another
// surface; debounce so the transit blip (or a combo/menu grab) doesn't dismiss.
static void wbpop_focus_cb(GObject *win, GParamSpec *ps, gpointer d) {
  (void)ps; WbPop *p = d;
  if (gtk_window_has_toplevel_focus(GTK_WINDOW(win))) {
    if (p->close_src) { g_source_remove(p->close_src); p->close_src = 0; }
    return;
  }
  if (p->armed && gtk_widget_get_visible(p->win) && !p->close_src)
    p->close_src = g_timeout_add(180, wbpop_close_timeout, p);
}

// Post-allocation correction pass; the pre-map clamp in wbpop_show is primary.
static gboolean wbpop_recenter(gpointer d) {
  WbPop *p = d;
  if (!gtk_widget_get_visible(p->win)) return G_SOURCE_REMOVE;
  int w = gtk_widget_get_allocated_width(p->win);
  int h = gtk_widget_get_allocated_height(p->win);
  if (w < 8) return G_SOURCE_CONTINUE;   // not allocated yet — retry next idle
  int sw = p->bar_w, sh = 0;
  GdkWindow *gw = gtk_widget_get_window(p->win);
  if (gw) {
    GdkMonitor *m = gdk_display_get_monitor_at_window(gtk_widget_get_display(p->win), gw);
    if (m) { GdkRectangle g; gdk_monitor_get_geometry(m, &g); sw = g.width; sh = g.height; }
  }
  int mx = p->pill_x + p->pill_w / 2 - w / 2;
  if (sw > 0 && mx + w > sw - 4) mx = sw - w - 4;
  if (mx < 4) mx = 4;
  int my = p->pop_top;
  if (sh > 0 && my + h > sh - 4) my = sh - h - 4;
  if (my < 4) my = 4;
  // Only touch the surface when the pre-map position was actually wrong: a
  // post-map margin change is a geometry change, which the compositor
  // animates (layer MOVE) — needlessly re-committing made every popup slide
  // and its blur backdrop settle visibly late.
  if (mx == p->last_mx && my == p->last_my) return G_SOURCE_REMOVE;
  p->last_mx = mx; p->last_my = my;
  gtk_layer_set_margin(GTK_WINDOW(p->win), GTK_LAYER_SHELL_EDGE_LEFT, mx);
  gtk_layer_set_margin(GTK_WINDOW(p->win), GTK_LAYER_SHELL_EDGE_TOP, my);
  gtk_widget_queue_resize(p->win);   // force a commit so the margin lands
  return G_SOURCE_REMOVE;
}

static G_GNUC_UNUSED void wbpop_show(WbPop *p) {
  g_object_set_data(G_OBJECT(p->win), "wb-focus", NULL);   // rebuild sets a fresh one
  p->rebuild(p->user);
  // A realized GtkWindow never shrinks below its largest allocation on its
  // own — after tall content (an open editor, a long list) later shows would
  // keep the stale height as an empty slab. Renegotiate from scratch; content
  // size-requests provide the floor.
  gtk_window_resize(GTK_WINDOW(p->win), 1, 1);
  GtkWidget *top = gtk_widget_get_toplevel(p->anchor);
  int x = 0, y = 0, yb = 0, dummy = 0;
  if (GTK_IS_WIDGET(top)) {
    gtk_widget_translate_coordinates(p->anchor, top, 0, 0, &x, &y);
    gtk_widget_translate_coordinates(p->anchor, top, 0,
                                     gtk_widget_get_allocated_height(p->anchor), &dummy, &yb);
  }
  p->pill_x = x;
  p->pill_w = gtk_widget_get_allocated_width(p->anchor);
  p->bar_w = GTK_IS_WIDGET(top) ? gtk_widget_get_allocated_width(top) : 0;
  // Pre-map clamp: margins set after map only reach the compositor with the
  // next buffer commit (which may never come for a static popup), so measure
  // the natural size NOW and put the final position in the initial configure.
  GtkRequisition nat = {0, 0};
  gtk_widget_get_preferred_size(p->win, NULL, &nat);
  int mx = x + p->pill_w / 2 - nat.width / 2;
  if (p->bar_w > 0 && nat.width > 0 && mx + nat.width > p->bar_w - 4)
    mx = p->bar_w - nat.width - 4;
  if (mx < 4) mx = 4;
  gtk_layer_set_margin(GTK_WINDOW(p->win), GTK_LAYER_SHELL_EDGE_LEFT, mx);
  p->pop_top = yb > 0 ? yb + 2 : 60;
  gtk_layer_set_margin(GTK_WINDOW(p->win), GTK_LAYER_SHELL_EDGE_TOP, p->pop_top);
  p->last_mx = mx; p->last_my = p->pop_top;
  // Singleton: hide whichever wbcommon popup (any plugin) is currently open.
  if (GTK_IS_WINDOW(top)) {
    GtkWidget *prev = g_object_get_data(G_OBJECT(top), WBPOP_OPEN_KEY);
    if (prev && prev != p->win && gtk_widget_get_visible(prev)) gtk_widget_hide(prev);
    g_object_set_data(G_OBJECT(top), WBPOP_OPEN_KEY, p->win);
  }
  p->armed = 0;
  if (p->close_src) { g_source_remove(p->close_src); p->close_src = 0; }
  gtk_widget_show_all(p->win);
  GtkWidget *focus = g_object_get_data(G_OBJECT(p->win), "wb-focus");
  gtk_widget_grab_focus(focus ? focus : p->win);
  g_timeout_add(250, wbpop_arm_cb, p);
  g_idle_add(wbpop_recenter, p);
}

static G_GNUC_UNUSED int wbpop_visible(WbPop *p) { return gtk_widget_get_visible(p->win); }

// For plugins that rebuild content while the popup is OPEN (e.g. an inline
// editor toggling): shrink/grow the window to the new content and re-clamp.
static G_GNUC_UNUSED void wbpop_refit(WbPop *p) {
  if (!gtk_widget_get_visible(p->win)) return;
  gtk_window_resize(GTK_WINDOW(p->win), 1, 1);
  g_idle_add(wbpop_recenter, p);
}

static G_GNUC_UNUSED void wbpop_toggle(WbPop *p) {
  if (wbpop_visible(p)) wbpop_hide(p); else wbpop_show(p);
}

static G_GNUC_UNUSED void wbpop_init(WbPop *p, GtkWidget *anchor,
                                     WbPopRebuildFn rebuild, gpointer user) {
  memset(p, 0, sizeof *p);
  p->anchor = anchor;
  p->rebuild = rebuild;
  p->user = user;
  p->win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_widget_set_name(p->win, "cffi-popwin");
  { GdkVisual *rgba = gdk_screen_get_rgba_visual(gtk_widget_get_screen(p->win));
    if (rgba) gtk_widget_set_visual(p->win, rgba); }   // transparency (no app_paintable!)
  gtk_layer_init_for_window(GTK_WINDOW(p->win));
  gtk_layer_set_namespace(GTK_WINDOW(p->win), "waybar-popup");
  gtk_layer_set_layer(GTK_WINDOW(p->win), GTK_LAYER_SHELL_LAYER_TOP);
  gtk_layer_set_anchor(GTK_WINDOW(p->win), GTK_LAYER_SHELL_EDGE_TOP, TRUE);
  gtk_layer_set_anchor(GTK_WINDOW(p->win), GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
  gtk_layer_set_exclusive_zone(GTK_WINDOW(p->win), -1);
  gtk_layer_set_keyboard_mode(GTK_WINDOW(p->win), GTK_LAYER_SHELL_KEYBOARD_MODE_ON_DEMAND);
  gtk_widget_add_events(p->win, GDK_KEY_PRESS_MASK);
  g_signal_connect(p->win, "key-press-event", G_CALLBACK(wbpop_key_cb), p);
  g_signal_connect(p->win, "notify::has-toplevel-focus", G_CALLBACK(wbpop_focus_cb), p);
}

static G_GNUC_UNUSED void wbpop_destroy(WbPop *p) {
  if (!p->win) return;
  wbpop_hide(p);
  gtk_widget_destroy(p->win);
  p->win = NULL;
}

// ─── themed icons: monochrome SVG tinted to the widget's theme colour ────────

static G_GNUC_UNUSED GdkPixbuf *wb_themed_pixbuf(GtkWidget *w, const char *dir,
                                                 int size, const char *name) {
  char *p = g_build_filename(dir, name, NULL);
  GdkPixbuf *src = gdk_pixbuf_new_from_file_at_size(p, size, size, NULL);
  g_free(p);
  if (!src) return NULL;
  GdkPixbuf *d = gdk_pixbuf_get_has_alpha(src) ? gdk_pixbuf_copy(src)
                                               : gdk_pixbuf_add_alpha(src, FALSE, 0, 0, 0);
  g_object_unref(src);
  GdkRGBA c; GtkStyleContext *sc = gtk_widget_get_style_context(w);
  gtk_style_context_get_color(sc, gtk_style_context_get_state(sc), &c);
  guchar R = (guchar)(c.red * 255), G = (guchar)(c.green * 255), B = (guchar)(c.blue * 255);
  int wd = gdk_pixbuf_get_width(d), h = gdk_pixbuf_get_height(d);
  int rs = gdk_pixbuf_get_rowstride(d), nc = gdk_pixbuf_get_n_channels(d);
  guchar *px = gdk_pixbuf_get_pixels(d);
  for (int y = 0; y < h; y++) for (int x = 0; x < wd; x++) {
    guchar *q = px + y * rs + x * nc; q[0] = R; q[1] = G; q[2] = B;
    if (nc == 4) q[3] = (guchar)(q[3] * c.alpha);
  }
  return d;
}

// GtkImage that re-tints itself on "style-updated" (theme / matugen change).
static void wb_icon_restyle(GtkWidget *img, gpointer d) {
  (void)d;
  const char *name = g_object_get_data(G_OBJECT(img), "wb-svg");
  const char *dir = g_object_get_data(G_OBJECT(img), "wb-dir");
  int sz = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(img), "wb-sz"));
  if (!name || !dir || sz <= 0) return;
  GdkPixbuf *pb = wb_themed_pixbuf(img, dir, sz, name);
  if (pb) { gtk_image_set_from_pixbuf(GTK_IMAGE(img), pb); g_object_unref(pb); }
}

static G_GNUC_UNUSED void wb_icon_set(GtkWidget *img, const char *name) {
  g_object_set_data_full(G_OBJECT(img), "wb-svg", g_strdup(name), g_free);
  wb_icon_restyle(img, NULL);
}

static G_GNUC_UNUSED GtkWidget *wb_icon_new(const char *dir, int size, const char *name) {
  GtkWidget *img = gtk_image_new();
  g_object_set_data_full(G_OBJECT(img), "wb-dir", g_strdup(dir), g_free);
  g_object_set_data(G_OBJECT(img), "wb-sz", GINT_TO_POINTER(size));
  g_signal_connect(img, "style-updated", G_CALLBACK(wb_icon_restyle), NULL);
  if (name) wb_icon_set(img, name);
  return img;
}

// ─── WbReader: watchdog line-reader subprocess ────────────────────────────────
// The child dies with the bar however the bar dies (PR_SET_PDEATHSIG — without
// it every restart leaks a `pactl subscribe`/`playerctl --follow`, and enough
// of those exhaust pipewire-pulse's client limit). If the child exits on its
// own (audio server restart), it is respawned with 1s→30s backoff.
// Teardown-safe: wb_reader_free() during a pending read defers the free to the
// read callback, so `user` is never touched after free.

typedef void (*WbLineFn)(const char *line, gpointer user);
typedef struct WbReader {
  char **argv;               // owned NULL-terminated copy
  WbLineFn on_line;
  gpointer user;
  GCancellable *cancel;
  GSubprocess *sub;
  GDataInputStream *in;
  guint respawn_src;
  int backoff_ms, priority, pending, dead;
} WbReader;

static void wb_reader_spawn(WbReader *r);

static void wb_reader_free_real(WbReader *r) {
  g_clear_object(&r->in);
  g_clear_object(&r->sub);
  g_clear_object(&r->cancel);
  g_strfreev(r->argv);
  g_free(r);
}

static void wb_child_pdeathsig(gpointer d) { (void)d; prctl(PR_SET_PDEATHSIG, SIGTERM); }

static gboolean wb_reader_respawn_cb(gpointer d) {
  WbReader *r = d;
  r->respawn_src = 0;
  if (!r->dead) wb_reader_spawn(r);
  return G_SOURCE_REMOVE;
}

static void wb_reader_line_cb(GObject *src, GAsyncResult *res, gpointer data) {
  WbReader *r = data;
  gsize len = 0;
  char *line = g_data_input_stream_read_line_finish(G_DATA_INPUT_STREAM(src), res, &len, NULL);
  r->pending = 0;
  if (r->dead || g_cancellable_is_cancelled(r->cancel)) { g_free(line); wb_reader_free_real(r); return; }
  if (!line) {   // child died / pipe closed: respawn with backoff
    g_clear_object(&r->in);
    g_clear_object(&r->sub);
    r->respawn_src = g_timeout_add(r->backoff_ms, wb_reader_respawn_cb, r);
    r->backoff_ms = r->backoff_ms >= 15000 ? 30000 : r->backoff_ms * 2;
    return;
  }
  r->backoff_ms = 1000;   // healthy again
  r->on_line(line, r->user);
  g_free(line);
  r->pending = 1;
  g_data_input_stream_read_line_async(r->in, r->priority, r->cancel, wb_reader_line_cb, r);
}

static void wb_reader_spawn(WbReader *r) {
  GSubprocessLauncher *ln = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                                      G_SUBPROCESS_FLAGS_STDERR_SILENCE);
  g_subprocess_launcher_set_child_setup(ln, wb_child_pdeathsig, NULL, NULL);
  r->sub = g_subprocess_launcher_spawnv(ln, (const char *const *)r->argv, NULL);
  g_object_unref(ln);
  if (!r->sub) {   // binary missing? retry later rather than dying silently
    r->respawn_src = g_timeout_add(r->backoff_ms, wb_reader_respawn_cb, r);
    r->backoff_ms = r->backoff_ms >= 15000 ? 30000 : r->backoff_ms * 2;
    return;
  }
  r->in = g_data_input_stream_new(g_subprocess_get_stdout_pipe(r->sub));
  r->pending = 1;
  g_data_input_stream_read_line_async(r->in, r->priority, r->cancel, wb_reader_line_cb, r);
}

// priority: G_PRIORITY_DEFAULT for sparse streams (pactl/playerctl);
// G_PRIORITY_DEFAULT_IDLE for chatty ones (cava frames) so UI work wins.
static G_GNUC_UNUSED WbReader *wb_reader_start(const char *const *argv,
                                               WbLineFn on_line, gpointer user,
                                               int priority) {
  WbReader *r = g_new0(WbReader, 1);
  r->argv = g_strdupv((char **)argv);
  r->on_line = on_line;
  r->user = user;
  r->cancel = g_cancellable_new();
  r->backoff_ms = 1000;
  r->priority = priority;
  wb_reader_spawn(r);
  return r;
}

static G_GNUC_UNUSED void wb_reader_free(WbReader *r) {
  if (!r) return;
  r->dead = 1;
  g_cancellable_cancel(r->cancel);
  if (r->respawn_src) { g_source_remove(r->respawn_src); r->respawn_src = 0; }
  if (r->sub) g_subprocess_force_exit(r->sub);
  if (!r->pending) wb_reader_free_real(r);   // else the read callback frees
}

#endif // WBCOMMON_H
