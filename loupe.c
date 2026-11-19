/*
 * screen-loupe - full-screen magnifier for X11, driven by Ctrl+Shift+two-finger scroll.
 *
 * Runs as a small daemon. It passively grabs Mod+Button4/5 (scroll up/down) on
 * the root window. The first "zoom in" snapshots the screen and shows a scaled
 * view of it in a full-screen override-redirect window. Zooming keeps the
 * content under the pointer fixed; pushing the pointer against a screen edge
 * pans the view. Scrolling down to 1.0x (or Esc, or a click) drops the overlay.
 *
 * This is the static-snapshot version: the zoomed view does not live-update.
 */
#define _POSIX_C_SOURCE 200809L
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/extensions/Xrender.h>
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

/* ---- X state ---- */
static Display *dpy;
static Window root, ov;
static int scr, W, H, depth;
static Visual *vis;
static XRenderPictFormat *pfmt;
static Pixmap snap, back;
static Picture psnap, pback;
static GC gc;
static KeyCode esc_key;

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

static void start_zoom(void) {
    /* Snapshot the visible screen server-side (before the overlay exists). */
    snap = XCreatePixmap(dpy, root, W, H, depth);
    XGCValues v;
    v.subwindow_mode = IncludeInferiors;
    GC sgc = XCreateGC(dpy, root, GCSubwindowMode, &v);
    XCopyArea(dpy, root, snap, sgc, 0, 0, W, H, 0, 0);
    XFreeGC(dpy, sgc);

    back = XCreatePixmap(dpy, root, W, H, depth);
    XRenderPictureAttributes pa;
    pa.repeat = RepeatPad;
    psnap = XRenderCreatePicture(dpy, snap, pfmt, CPRepeat, &pa);
    pback = XRenderCreatePicture(dpy, back, pfmt, 0, NULL);

    XSetWindowAttributes wa;
    wa.override_redirect = True;
    wa.background_pixmap = None;
    wa.event_mask = ButtonPressMask | ExposureMask;
    ov = XCreateWindow(dpy, root, 0, 0, W, H, 0, CopyFromParent, InputOutput,
                       CopyFromParent, CWOverrideRedirect | CWBackPixmap | CWEventMask, &wa);
    gc = XCreateGC(dpy, ov, 0, NULL);
    XMapRaised(dpy, ov);

    /* Esc exits, but only while zoomed so we don't steal it otherwise. */
    XGrabKey(dpy, esc_key, AnyModifier, root, False, GrabModeAsync, GrabModeAsync);
    zoomed = 1;
}

static void stop_zoom(void) {
    if (!zoomed) return;
    XUngrabKey(dpy, esc_key, AnyModifier, root);
    XUnmapWindow(dpy, ov);
    XFreeGC(dpy, gc);
    XDestroyWindow(dpy, ov);
    XRenderFreePicture(dpy, psnap);
    XRenderFreePicture(dpy, pback);
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

static void usage(const char *p) {
    fprintf(stderr,
        "usage: %s [options]\n"
        "  -m MODS   modifier(s) for scroll, e.g. ctrl+shift (default), super, ctrl+alt\n"
        "  -s STEP   zoom factor per scroll tick (default %.2f)\n"
        "  -M MAX    maximum zoom (default %.0f)\n"
        "  -e PX     edge-pan margin in pixels (default %d)\n"
        "  -n        nearest-neighbour scaling (crisp pixels) instead of bilinear\n"
        "  -i        invert scroll direction (for natural scrolling)\n"
        "Scroll up = zoom in, down = zoom out; at 1.0x the overlay goes away.\n"
        "Esc or a mouse click also exits the zoomed view.\n",
        p, step, max_zoom, edge_margin);
}

int main(int argc, char **argv) {
    int c;
    while ((c = getopt(argc, argv, "m:s:M:e:nih")) != -1) {
        switch (c) {
        case 'm':
            if (parse_mods(optarg, &want_mods)) { fprintf(stderr, "bad -m\n"); return 2; }
            break;
        case 's': step = atof(optarg); if (step <= 1.0) { fprintf(stderr, "step must be > 1\n"); return 2; } break;
        case 'M': max_zoom = atof(optarg); if (max_zoom <= 1.0) { fprintf(stderr, "bad -M\n"); return 2; } break;
        case 'e': edge_margin = atoi(optarg); break;
        case 'n': use_nearest = 1; break;
        case 'i': invert = 1; break;
        default: usage(argv[0]); return c == 'h' ? 0 : 2;
        }
    }

    dpy = XOpenDisplay(NULL);
    if (!dpy) { fprintf(stderr, "screen-loupe: cannot open display\n"); return 1; }
    scr = DefaultScreen(dpy);
    root = RootWindow(dpy, scr);
    W = DisplayWidth(dpy, scr);
    H = DisplayHeight(dpy, scr);
    depth = DefaultDepth(dpy, scr);
    vis = DefaultVisual(dpy, scr);
    pfmt = XRenderFindVisualFormat(dpy, vis);
    if (!pfmt) { fprintf(stderr, "screen-loupe: XRender not available\n"); return 1; }
    esc_key = XKeysymToKeycode(dpy, XK_Escape);

    XSetErrorHandler(err_handler);
    /* Grab with every NumLock/CapsLock combination so they don't break it. */
    unsigned int locks[] = {0, LockMask, Mod2Mask, LockMask | Mod2Mask};
    for (int b = Button4; b <= Button5; b++)
        for (unsigned i = 0; i < 4; i++)
            XGrabButton(dpy, b, want_mods | locks[i], root, False, ButtonPressMask,
                        GrabModeAsync, GrabModeAsync, None, None);
    XSync(dpy, False);
    if (grab_failed) {
        fprintf(stderr, "screen-loupe: that scroll+modifier combo is already grabbed by another client\n");
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
        last = t;
    }
    stop_zoom();
    XCloseDisplay(dpy);
    return 0;
}
