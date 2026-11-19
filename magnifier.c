/*
 * x11-magnifier - full-screen magnifier for X11, driven by Ctrl+Shift+two-finger scroll.
 *
 * Runs as a small daemon. It passively grabs Mod+Button4/5 (scroll up/down) on
 * the root window. The first "zoom in" snapshots the screen and shows a scaled
 * view of it in a full-screen override-redirect window. Zooming keeps the
 * content under the pointer fixed; pushing the pointer against a screen edge
 * pans the view. Scrolling down to 1.0x (or Esc, or a click) drops the overlay.
 *
 * Live refresh: while zoomed, the root's children are redirected with the
 * Composite extension (automatic mode, so no compositor is needed and it can
 * coexist with one). Each window's current pixmap is repainted over the
 * initial snapshot whenever XDamage reports a change, at a limited rate, so
 * typing in a terminal shows up in the zoomed view. -p disables this and
 * leaves the plain static snapshot.
 */
#define _POSIX_C_SOURCE 200809L
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/extensions/Xrender.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

/* ---- configuration (see usage()) ---- */
static unsigned int want_mods = ControlMask | ShiftMask;
static double step = 1.08;      /* zoom factor per scroll tick */
static double max_zoom = 12.0;
static int invert = 0;          /* swap scroll direction (natural scrolling) */
static int use_nearest = 0;     /* nearest-neighbour instead of bilinear */
static int edge_margin = 40;    /* px from screen edge where panning starts */
static double pan_speed = 1100; /* screen px/second at the very edge */
static int live = 1;            /* live refresh via Composite+Damage */
static double refresh_s = 0.2;  /* minimum seconds between refreshes */

/* ---- X state ---- */
static Display *dpy;
static Window root, ov;
static int scr, W, H, depth;
static Visual *vis;
static XRenderPictFormat *pfmt;
static Pixmap base, snap, back; /* base: pristine snapshot; snap: base + live windows */
static Picture psnap, pdst, pback;
static GC gc;
static int live_active, damage_ev, dirty;
static double last_refresh;

#define MAXTRACK 256
static struct { Window w; Damage d; int seen; } track[MAXTRACK];
static int ntrack;
static KeyCode esc_key;          /* key that leaves the zoomed view */
static KeySym exit_keysym = XK_Escape;

/* ---- zoom state ---- */
static int zoomed;
static double z = 1.0, ox, oy; /* view origin in source coords */

static volatile sig_atomic_t quit;

static void on_signal(int sig) { (void)sig; quit = 1; }

static int grab_failed;
static int err_handler(Display *d, XErrorEvent *e) {
    (void)d;
    if (e->error_code == BadAccess) grab_failed = 1;
    return 0;
}

static double clampd(double v, double lo, double hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

static void clamp_origin(void) {
    ox = clampd(ox, 0, W - W / z);
    oy = clampd(oy, 0, H - H / z);
}

static void refresh_snap(void);

static void render(void) {
    XTransform t = {{
        {XDoubleToFixed(1.0 / z), 0, XDoubleToFixed(ox)},
        {0, XDoubleToFixed(1.0 / z), XDoubleToFixed(oy)},
        {0, 0, XDoubleToFixed(1.0)},
    }};
    XRenderSetPictureTransform(dpy, psnap, &t);
    XRenderSetPictureFilter(dpy, psnap, use_nearest ? "nearest" : "bilinear", NULL, 0);
    XRenderComposite(dpy, PictOpSrc, psnap, None, pback, 0, 0, 0, 0, 0, 0, W, H);
    XCopyArea(dpy, back, ov, gc, 0, 0, W, H, 0, 0);
    XFlush(dpy);
}

static double now(void);

static void track_window(Window w, int *found) {
    for (int i = 0; i < ntrack; i++)
        if (track[i].w == w) { track[i].seen = 1; *found = 1; return; }
    if (ntrack < MAXTRACK) {
        track[ntrack].w = w;
        track[ntrack].d = XDamageCreate(dpy, w, XDamageReportNonEmpty);
        track[ntrack].seen = 1;
        ntrack++;
    }
}

/* Rebuild snap = base + current contents of every viewable top-level window. */
static void refresh_snap(void) {
    Window rr, pr, *kids = NULL;
    unsigned int n = 0;
    for (int i = 0; i < ntrack; i++) track[i].seen = 0;
    XCopyArea(dpy, base, snap, gc, 0, 0, W, H, 0, 0);
    if (XQueryTree(dpy, root, &rr, &pr, &kids, &n)) {
        for (unsigned i = 0; i < n; i++) { /* bottom-to-top stacking order */
            Window w = kids[i];
            XWindowAttributes a;
            if (w == ov || !XGetWindowAttributes(dpy, w, &a)) continue;
            if (a.class != InputOutput || a.map_state != IsViewable) continue;
            int found = 0;
            track_window(w, &found);
            XRenderPictFormat *f = XRenderFindVisualFormat(dpy, a.visual);
            if (!f) continue;
            Pixmap wp = XCompositeNameWindowPixmap(dpy, w);
            Picture wpic = XRenderCreatePicture(dpy, wp, f, 0, NULL);
            XRenderComposite(dpy, PictOpOver, wpic, None, pdst, 0, 0, 0, 0,
                             a.x + a.border_width, a.y + a.border_width, a.width, a.height);
            XRenderFreePicture(dpy, wpic);
            XFreePixmap(dpy, wp);
        }
        if (kids) XFree(kids);
    }
    /* drop damage objects for windows that went away */
    for (int i = 0; i < ntrack;) {
        if (!track[i].seen) { XDamageDestroy(dpy, track[i].d); track[i] = track[--ntrack]; }
        else i++;
    }
    last_refresh = now();
    dirty = 0;
}

static void start_zoom(void) {
    /* Snapshot the visible screen server-side (before the overlay exists). */
    XGCValues v;
    v.subwindow_mode = IncludeInferiors;
    gc = XCreateGC(dpy, root, GCSubwindowMode, &v);
    base = XCreatePixmap(dpy, root, W, H, depth);
    XCopyArea(dpy, root, base, gc, 0, 0, W, H, 0, 0);
    snap = XCreatePixmap(dpy, root, W, H, depth);
    XCopyArea(dpy, base, snap, gc, 0, 0, W, H, 0, 0);

    back = XCreatePixmap(dpy, root, W, H, depth);
    XRenderPictureAttributes pa;
    pa.repeat = RepeatPad;
    psnap = XRenderCreatePicture(dpy, snap, pfmt, CPRepeat, &pa);
    pdst = XRenderCreatePicture(dpy, snap, pfmt, 0, NULL);
    pback = XRenderCreatePicture(dpy, back, pfmt, 0, NULL);

    XSetWindowAttributes wa;
    wa.override_redirect = True;
    wa.background_pixmap = None;
    wa.event_mask = ButtonPressMask | ExposureMask;
    ov = XCreateWindow(dpy, root, 0, 0, W, H, 0, CopyFromParent, InputOutput,
                       CopyFromParent, CWOverrideRedirect | CWBackPixmap | CWEventMask, &wa);
    XMapRaised(dpy, ov);

    /* Esc exits, but only while zoomed so we don't steal it otherwise. */
    grab_failed = 0;
    XGrabKey(dpy, esc_key, AnyModifier, root, False, GrabModeAsync, GrabModeAsync);
    XSync(dpy, False);
    if (grab_failed)
        fprintf(stderr, "x11-magnifier: exit key is grabbed by another client; "
                        "use a click or scroll down instead (or set exit_key)\n");
    zoomed = 1;

    if (live) {
        XCompositeRedirectSubwindows(dpy, root, CompositeRedirectAutomatic);
        XWindowAttributes ra;
        XGetWindowAttributes(dpy, root, &ra);
        XSelectInput(dpy, root, ra.your_event_mask | SubstructureNotifyMask);
        live_active = 1;
        dirty = 0;
        last_refresh = 0;
        refresh_snap(); /* also starts damage tracking */
    }
}

static void stop_zoom(void) {
    if (!zoomed) return;
    XUngrabKey(dpy, esc_key, AnyModifier, root);
    if (live_active) {
        for (int i = 0; i < ntrack; i++) XDamageDestroy(dpy, track[i].d);
        ntrack = 0;
        XCompositeUnredirectSubwindows(dpy, root, CompositeRedirectAutomatic);
        XWindowAttributes ra;
        XGetWindowAttributes(dpy, root, &ra);
        XSelectInput(dpy, root, ra.your_event_mask & ~SubstructureNotifyMask);
        live_active = 0;
    }
    XUnmapWindow(dpy, ov);
    XFreeGC(dpy, gc);
    XDestroyWindow(dpy, ov);
    XRenderFreePicture(dpy, psnap);
    XRenderFreePicture(dpy, pdst);
    XRenderFreePicture(dpy, pback);
    XFreePixmap(dpy, base);
    XFreePixmap(dpy, snap);
    XFreePixmap(dpy, back);
    XFlush(dpy);
    zoomed = 0;
    z = 1.0;
}

/* Zoom by factor f, keeping the source point under pointer (px,py) fixed. */
static void zoom_by(double f, int px, int py) {
    double nz = clampd(z * f, 1.0, max_zoom);
    if (nz == z && zoomed) return;
    if (nz <= 1.0001) { stop_zoom(); return; }
    if (!zoomed) { z = 1.0; ox = oy = 0; start_zoom(); }
    double sx = ox + px / z, sy = oy + py / z;
    z = nz;
    ox = sx - px / z;
    oy = sy - py / z;
    clamp_origin();
    render();
}

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* Pan while the pointer is pushed against a screen edge. */
static void pan_tick(double dt) {
    Window rr, cr;
    int rx, ry, wx, wy;
    unsigned int mask;
    if (!XQueryPointer(dpy, root, &rr, &cr, &rx, &ry, &wx, &wy, &mask)) return;
    double dx = 0, dy = 0;
    if (rx < edge_margin) dx = -(double)(edge_margin - rx) / edge_margin;
    else if (rx > W - 1 - edge_margin) dx = (double)(rx - (W - 1 - edge_margin)) / edge_margin;
    if (ry < edge_margin) dy = -(double)(edge_margin - ry) / edge_margin;
    else if (ry > H - 1 - edge_margin) dy = (double)(ry - (H - 1 - edge_margin)) / edge_margin;
    if (dx == 0 && dy == 0) return;
    double nox = ox + dx * pan_speed * dt / z, noy = oy + dy * pan_speed * dt / z;
    double sx = ox, sy = oy;
    ox = nox; oy = noy;
    clamp_origin();
    if (ox != sx || oy != sy) render();
}

static void handle(XEvent *e) {
    switch (e->type) {
    case ButtonPress: {
        unsigned b = e->xbutton.button;
        if (b == Button4 || b == Button5) {
            if ((e->xbutton.state & want_mods) != want_mods) break; /* plain scroll */
            int up = (b == Button4) != invert;
            zoom_by(up ? step : 1.0 / step, e->xbutton.x_root, e->xbutton.y_root);
        } else if (zoomed && b <= Button3) {
            stop_zoom(); /* click dismisses the zoom */
        }
        break;
    }
    case KeyPress:
        if (zoomed && e->xkey.keycode == esc_key) stop_zoom();
        break;
    case Expose:
        if (zoomed && e->xexpose.count == 0) render();
        break;
    default:
        if (!live_active) break;
        if (e->type == damage_ev + XDamageNotify) {
            XDamageNotifyEvent *de = (XDamageNotifyEvent *)e;
            XDamageSubtract(dpy, de->damage, None, None);
            dirty = 1;
        } else if (e->type == MapNotify || e->type == UnmapNotify ||
                   e->type == ConfigureNotify || e->type == CreateNotify ||
                   e->type == DestroyNotify || e->type == ReparentNotify ||
                   e->type == CirculateNotify) {
            dirty = 1;
        }
        break;
    }
}

static int parse_mods(const char *s, unsigned int *out) {
    char *buf = strdup(s), *save = NULL;
    unsigned int m = 0;
    for (char *t = strtok_r(buf, "+", &save); t; t = strtok_r(NULL, "+", &save)) {
        if (!strcasecmp(t, "ctrl") || !strcasecmp(t, "control")) m |= ControlMask;
        else if (!strcasecmp(t, "shift")) m |= ShiftMask;
        else if (!strcasecmp(t, "alt")) m |= Mod1Mask;
        else if (!strcasecmp(t, "super")) m |= Mod4Mask;
        else { free(buf); return -1; }
    }
    free(buf);
    *out = m;
    return m ? 0 : -1;
}

static int parse_bool(const char *v, int *out) {
    if (!strcasecmp(v, "1") || !strcasecmp(v, "on") || !strcasecmp(v, "yes") || !strcasecmp(v, "true")) *out = 1;
    else if (!strcasecmp(v, "0") || !strcasecmp(v, "off") || !strcasecmp(v, "no") || !strcasecmp(v, "false")) *out = 0;
    else return -1;
    return 0;
}

/* One setting, from the config file or a command-line flag. 0 ok, -1 bad. */
static int apply_setting(const char *k, const char *v) {
    if (!strcmp(k, "modifiers")) return parse_mods(v, &want_mods);
    if (!strcmp(k, "exit_key")) {
        KeySym ks = XStringToKeysym(v);
        if (ks == NoSymbol) return -1;
        exit_keysym = ks;
        return 0;
    }
    if (!strcmp(k, "step")) { double d = atof(v); if (d <= 1.0) return -1; step = d; return 0; }
    if (!strcmp(k, "max_zoom")) { double d = atof(v); if (d <= 1.0) return -1; max_zoom = d; return 0; }
    if (!strcmp(k, "edge_margin")) { edge_margin = atoi(v); return 0; }
    if (!strcmp(k, "refresh_ms")) { refresh_s = atof(v) / 1000.0; return 0; }
    if (!strcmp(k, "live")) return parse_bool(v, &live);
    if (!strcmp(k, "nearest")) return parse_bool(v, &use_nearest);
    if (!strcmp(k, "invert")) return parse_bool(v, &invert);
    return -1;
}

static char *trim(char *p) {
    while (*p == ' ' || *p == '\t') p++;
    char *e = p + strlen(p);
    while (e > p && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r')) *--e = 0;
    return p;
}

/* key = value lines, '#' comments. A missing default file is fine. */
static int load_config(const char *path, int must_exist) {
    FILE *f = fopen(path, "r");
    if (!f) {
        if (must_exist) { fprintf(stderr, "x11-magnifier: cannot read %s\n", path); return -1; }
        return 0;
    }
    char line[512];
    int n = 0, rc = 0;
    while (fgets(line, sizeof line, f)) {
        n++;
        char *h = strchr(line, '#');
        if (h) *h = 0;
        char *p = trim(line);
        if (!*p) continue;
        char *eq = strchr(p, '=');
        if (!eq) { fprintf(stderr, "x11-magnifier: %s:%d: expected key = value\n", path, n); rc = -1; continue; }
        *eq = 0;
        if (apply_setting(trim(p), trim(eq + 1))) {
            fprintf(stderr, "x11-magnifier: %s:%d: bad setting\n", path, n);
            rc = -1;
        }
    }
    fclose(f);
    return rc;
}

static void usage(const char *p) {
    fprintf(stderr,
        "usage: %s [options]\n"
        "  -m MODS   modifier(s) for scroll, e.g. ctrl+shift (default), super, ctrl+alt\n"
        "  -k KEY    key that leaves the zoomed view, an X keysym name (default Escape)\n"
        "  -c FILE   config file (default ~/.config/x11-magnifier/config); flags override it\n"
        "  -s STEP   zoom factor per scroll tick (default %.2f)\n"
        "  -M MAX    maximum zoom (default %.0f)\n"
        "  -e PX     edge-pan margin in pixels (default %d)\n"
        "  -r MS     min milliseconds between live refreshes (default 200)\n"
        "  -p        plain static snapshot, no live refresh\n"
        "  -n        nearest-neighbour scaling (crisp pixels) instead of bilinear\n"
        "  -i        invert scroll direction (for natural scrolling)\n"
        "Scroll up = zoom in, down = zoom out; at 1.0x the overlay goes away.\n"
        "The exit key or a mouse click also leaves the zoomed view.\n"
        "Config file keys: modifiers, exit_key, step, max_zoom, edge_margin,\n"
        "refresh_ms, live, nearest, invert (see the man page).\n",
        p, step, max_zoom, edge_margin);
}

int main(int argc, char **argv) {
    /* defaults < config file < command-line flags */
    const char *cfg = NULL;
    for (int a = 1; a + 1 < argc; a++)
        if (!strcmp(argv[a], "-c")) cfg = argv[a + 1];
    if (cfg) {
        if (load_config(cfg, 1)) return 2;
    } else {
        char path[1024];
        const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
        if (xdg && *xdg) snprintf(path, sizeof path, "%s/x11-magnifier/config", xdg);
        else if (home) snprintf(path, sizeof path, "%s/.config/x11-magnifier/config", home);
        else path[0] = 0;
        if (path[0] && load_config(path, 0)) return 2;
    }

    int c;
    while ((c = getopt(argc, argv, "m:k:c:s:M:e:r:pnih")) != -1) {
        int bad = 0;
        switch (c) {
        case 'm': bad = apply_setting("modifiers", optarg); break;
        case 'k': bad = apply_setting("exit_key", optarg); break;
        case 'c': break; /* handled above */
        case 's': bad = apply_setting("step", optarg); break;
        case 'M': bad = apply_setting("max_zoom", optarg); break;
        case 'e': bad = apply_setting("edge_margin", optarg); break;
        case 'r': bad = apply_setting("refresh_ms", optarg); break;
        case 'p': live = 0; break;
        case 'n': use_nearest = 1; break;
        case 'i': invert = 1; break;
        default: usage(argv[0]); return c == 'h' ? 0 : 2;
        }
        if (bad) { fprintf(stderr, "x11-magnifier: bad value for -%c: %s\n", c, optarg); return 2; }
    }

    dpy = XOpenDisplay(NULL);
    if (!dpy) { fprintf(stderr, "x11-magnifier: cannot open display\n"); return 1; }
    scr = DefaultScreen(dpy);
    root = RootWindow(dpy, scr);
    W = DisplayWidth(dpy, scr);
    H = DisplayHeight(dpy, scr);
    depth = DefaultDepth(dpy, scr);
    vis = DefaultVisual(dpy, scr);
    pfmt = XRenderFindVisualFormat(dpy, vis);
    if (!pfmt) { fprintf(stderr, "x11-magnifier: XRender not available\n"); return 1; }
    esc_key = XKeysymToKeycode(dpy, exit_keysym);
    if (!esc_key) { fprintf(stderr, "x11-magnifier: exit key has no keycode on this keyboard\n"); return 1; }
    if (live) {
        int ev, er, cmaj, cmin;
        if (!XCompositeQueryExtension(dpy, &ev, &er) ||
            !XCompositeQueryVersion(dpy, &cmaj, &cmin) || (cmaj == 0 && cmin < 2) ||
            !XDamageQueryExtension(dpy, &damage_ev, &er)) {
            fprintf(stderr, "x11-magnifier: Composite/Damage unavailable, using static snapshot\n");
            live = 0;
        }
    }

    XSetErrorHandler(err_handler);
    /* Grab with every NumLock/CapsLock combination so they don't break it. */
    unsigned int locks[] = {0, LockMask, Mod2Mask, LockMask | Mod2Mask};
    for (int b = Button4; b <= Button5; b++)
        for (unsigned i = 0; i < 4; i++)
            XGrabButton(dpy, b, want_mods | locks[i], root, False, ButtonPressMask,
                        GrabModeAsync, GrabModeAsync, None, None);
    XSync(dpy, False);
    if (grab_failed) {
        fprintf(stderr, "x11-magnifier: that scroll+modifier combo is already grabbed by another client\n");
        return 1;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    struct pollfd pfd = {ConnectionNumber(dpy), POLLIN, 0};
    double last = now();
    while (!quit) {
        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);
            handle(&ev);
        }
        poll(&pfd, 1, zoomed ? 16 : -1);
        double t = now();
        if (zoomed) pan_tick(t - last);
        if (live_active && dirty && t - last_refresh >= refresh_s) {
            refresh_snap();
            render();
        }
        last = t;
    }
    stop_zoom();
    XCloseDisplay(dpy);
    return 0;
}
