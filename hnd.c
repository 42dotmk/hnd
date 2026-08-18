#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>
#include <X11/Xft/Xft.h>
#include <dbus/dbus.h>

#define STB_DS_IMPLEMENTATION
#include <stb_ds.h>

#define NIFACE "org.freedesktop.Notifications"
#define NPATH  "/org/freedesktop/Notifications"

enum { Low, Normal, Critical };            /* urgency */
enum { ColBg, ColSum, ColBody, ColBorder };
enum { Expired = 1, Dismissed = 2, Closed = 3 }; /* NotificationClosed reasons */

/* configuration */
static unsigned int minwidth       = 380;        /* narrowest notification (-w) */
static unsigned int maxwpct        = 50;         /* widest, % of monitor width (-W) */
static const unsigned int borderpx = 2;          /* border width */
static const unsigned int padding  = 8;          /* gap from screen corner */
static const unsigned int gap      = 8;          /* gap between notifications */
static const unsigned int hpad     = 10;         /* inner horizontal padding */
static const unsigned int vpad     = 8;          /* inner vertical padding */
static const unsigned int linespc  = 2;          /* extra px between lines */
static const unsigned int maxlines = 16;         /* lines per notification cap */
static const int atbottom          = 0;          /* stack from the bottom edge */
static const char fontname[]       = "Iosevka NFM:pixelsize=13"; /* fontconfig name, "monospace" as fallback */
/* default timeout ms per urgency when the sender leaves it up to us;
 * 0 = sticky until clicked */
static const long defaulttimeout[3] = { 5000, 5000, 0 };
/* bg, summary fg, body fg, border - per urgency */
static const char *colors[3][4] = {
	{ "#1a1b26", "#565f89", "#414868", "#3b4261" }, /* low */
	{ "#1a1b26", "#c0caf5", "#7aa2f7", "#3b4261" }, /* normal */
	{ "#1a1b26", "#f7768e", "#a9b1d6", "#f7768e" }, /* critical */
};

typedef struct {
	unsigned int id;
	Window win;
	int urgency;
	long deadline;    /* CLOCK_MONOTONIC ms, 0 = sticky */
	int hasdefault;   /* sender offered a "default" action */
	char text[4096];  /* wrapped display text, '\n' after every line */
	int len;
	int nlines;
	int sumlines;     /* leading lines that belong to the summary */
	int w;            /* window width, sized to the text */
	int h;            /* window height */
} Notif;

static Display *dpy;
static int screen, haverandr;
static int mx, my, mw, mh; /* monitor the stack sits on */
static Window root;
static GC gc;
static XftFont *font;
static XftColor xftpixels[3][4];
static int lineh;
static unsigned long pixels[3][4];
static DBusConnection *conn;
static int dbusfd;
static Notif *notifs; /* stb_ds array, in arrival order */
static unsigned int nextid = 1;
static volatile sig_atomic_t closereq;

static void
die(const char *msg)
{
	fputs(msg, stderr);
	exit(1);
}

/* a window may vanish between our request and the server acting on it */
static int
xerror(Display *d, XErrorEvent *ee)
{
	(void)d; (void)ee;
	return 0;
}

/* SIGUSR1 dismisses every visible notification */
static void
sighandler(int sig)
{
	(void)sig;
	closereq = 1;
}

static long
nowms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* pin the stack to the second monitor when there is one, like htray;
 * RandR lists the primary first. Whole screen without the extension. */
static void
updatemon(void)
{
	XRRMonitorInfo *info;
	int i, n;

	mx = 0;
	my = 0;
	mw = DisplayWidth(dpy, screen);
	mh = DisplayHeight(dpy, screen);
	if (!haverandr)
		return;
	if (!(info = XRRGetMonitors(dpy, root, True, &n)))
		return;
	if (n > 0) {
		i = n > 1 ? 1 : 0;
		mx = info[i].x;
		my = info[i].y;
		mw = info[i].width;
		mh = info[i].height;
	}
	XRRFreeMonitors(info);
}

/* copy src into dst minus markup: tags are dropped (<br> becomes a line
 * break), the five predefined entities are decoded, tabs turn to spaces */
static void
stripmarkup(char *dst, const char *src, size_t size)
{
	size_t o = 0;

	while (*src && o < size - 1) {
		if (*src == '<') {
			if (!strncasecmp(src, "<br", 3))
				dst[o++] = '\n';
			while (*src && *src != '>')
				src++;
			if (*src)
				src++;
		} else if (!strncmp(src, "&amp;", 5)) {
			dst[o++] = '&';
			src += 5;
		} else if (!strncmp(src, "&lt;", 4)) {
			dst[o++] = '<';
			src += 4;
		} else if (!strncmp(src, "&gt;", 4)) {
			dst[o++] = '>';
			src += 4;
		} else if (!strncmp(src, "&quot;", 6)) {
			dst[o++] = '"';
			src += 6;
		} else if (!strncmp(src, "&apos;", 6)) {
			dst[o++] = '\'';
			src += 6;
		} else if (*src == '\r') {
			src++;
		} else if (*src == '\t') {
			dst[o++] = ' ';
			src++;
		} else {
			dst[o++] = *src++;
		}
	}
	dst[o] = '\0';
}

static void
addline(Notif *n, const char *s, int len)
{
	if (n->nlines >= (int)maxlines
	    || n->len + len + 2 > (int)sizeof n->text)
		return;
	memcpy(n->text + n->len, s, (size_t)len);
	n->len += len;
	n->text[n->len++] = '\n';
	n->text[n->len] = '\0';
	n->nlines++;
}

static int
textw(const char *s, int len)
{
	XGlyphInfo ext;

	XftTextExtentsUtf8(dpy, font, (FcChar8 *)s, len, &ext);
	return ext.xOff;
}

/* bytes of the UTF-8 sequence starting at c (1 on malformed input) */
static int
utf8len(unsigned char c)
{
	if (c >= 0xf0)
		return 4;
	if (c >= 0xe0)
		return 3;
	if (c >= 0xc0)
		return 2;
	return 1;
}

/* append src to n->text word-wrapped to avail px */
static void
wrapappend(Notif *n, const char *s, int avail)
{
	int w, cw, cl, len, brk;

	while (*s && n->nlines < (int)maxlines) {
		w = 0;
		len = 0;
		brk = -1;
		while (s[len] && s[len] != '\n') {
			cl = utf8len((unsigned char)s[len]);
			cw = textw(s + len, cl);
			if (w + cw > avail && len)
				break;
			if (s[len] == ' ')
				brk = len;
			w += cw;
			len += cl;
		}
		if (s[len] && s[len] != '\n' && brk > 0)
			len = brk; /* break at the last space that fit */
		addline(n, s, len);
		s += len;
		while (*s == ' ')
			s++;
		if (*s == '\n')
			s++;
	}
}

/* wrap summary and body and size the window to the text: as wide as the
 * longest line wants, between minwidth and maxwpct% of the monitor */
static void
settext(Notif *n, const char *summary, const char *body)
{
	char clean[sizeof n->text];
	const char *s, *e;
	int maxw, avail, tw, lw;

	updatemon();
	maxw = mw * (int)maxwpct / 100;
	if (maxw < (int)minwidth)
		maxw = (int)minwidth;
	avail = maxw - 2 * (int)hpad;
	n->len = 0;
	n->nlines = 0;
	n->text[0] = '\0';
	stripmarkup(clean, summary, sizeof clean);
	wrapappend(n, clean, avail);
	n->sumlines = n->nlines;
	if (body[0]) {
		stripmarkup(clean, body, sizeof clean);
		wrapappend(n, clean, avail);
	}
	if (!n->nlines)
		addline(n, "", 0);
	tw = 0;
	for (s = n->text; (e = strchr(s, '\n')); s = e + 1)
		if ((lw = textw(s, (int)(e - s))) > tw)
			tw = lw;
	n->w = tw + 2 * (int)hpad;
	if (n->w < (int)minwidth)
		n->w = (int)minwidth;
	n->h = 2 * (int)vpad + n->nlines * lineh;
}

static int
findwin(Window w)
{
	int i;

	for (i = 0; i < (int)arrlen(notifs); i++)
		if (notifs[i].win == w)
			return i;
	return -1;
}

static void
makewin(Notif *n)
{
	XSetWindowAttributes swa;

	swa.override_redirect = True;
	swa.background_pixel = pixels[n->urgency][ColBg];
	swa.border_pixel = pixels[n->urgency][ColBorder];
	swa.event_mask = ExposureMask|ButtonPressMask;
	n->win = XCreateWindow(dpy, root, 0, 0, (unsigned int)n->w,
	                       (unsigned int)n->h,
	                       borderpx, CopyFromParent, CopyFromParent,
	                       CopyFromParent, CWOverrideRedirect|CWBackPixel
	                       |CWBorderPixel|CWEventMask, &swa);
}

/* restack newest-last and slot every window down the monitor edge;
 * mapping an already-mapped window just raises it */
static void
layout(void)
{
	int i, x, y, n = (int)arrlen(notifs);

	updatemon();
	y = atbottom ? my + mh - (int)padding : my + (int)padding;
	for (i = 0; i < n; i++) {
		x = mx + mw - notifs[i].w - 2 * (int)borderpx - (int)padding;
		if (atbottom)
			y -= notifs[i].h + 2 * (int)borderpx;
		XMoveResizeWindow(dpy, notifs[i].win, x, y,
		                  (unsigned int)notifs[i].w,
		                  (unsigned int)notifs[i].h);
		if (atbottom)
			y -= (int)gap;
		else
			y += notifs[i].h + 2 * (int)borderpx + (int)gap;
		XMapRaised(dpy, notifs[i].win);
	}
}

static void
raiseall(void)
{
	int i;

	for (i = 0; i < (int)arrlen(notifs); i++)
		XRaiseWindow(dpy, notifs[i].win);
}

static void
draw(Notif *n)
{
	XftDraw *xd;
	const char *s = n->text, *e;
	int i = 0, y = (int)vpad + font->ascent;

	XClearWindow(dpy, n->win);
	if (!(xd = XftDrawCreate(dpy, n->win, DefaultVisual(dpy, screen),
	                         DefaultColormap(dpy, screen))))
		return;
	while ((e = strchr(s, '\n'))) {
		XftDrawStringUtf8(xd, &xftpixels[n->urgency]
		                  [i < n->sumlines ? ColSum : ColBody],
		                  font, (int)hpad, y, (FcChar8 *)s,
		                  (int)(e - s));
		s = e + 1;
		y += lineh;
		i++;
	}
	XftDrawDestroy(xd);
}

static void
signalclosed(unsigned int id, unsigned int reason)
{
	DBusMessage *sig;
	dbus_uint32_t i = id, r = reason;

	if (!(sig = dbus_message_new_signal(NPATH, NIFACE,
	                                    "NotificationClosed")))
		return;
	dbus_message_append_args(sig, DBUS_TYPE_UINT32, &i,
	                         DBUS_TYPE_UINT32, &r, DBUS_TYPE_INVALID);
	dbus_connection_send(conn, sig, NULL);
	dbus_connection_flush(conn);
	dbus_message_unref(sig);
}

static void
signalaction(unsigned int id)
{
	DBusMessage *sig;
	dbus_uint32_t i = id;
	const char *key = "default";

	if (!(sig = dbus_message_new_signal(NPATH, NIFACE, "ActionInvoked")))
		return;
	dbus_message_append_args(sig, DBUS_TYPE_UINT32, &i,
	                         DBUS_TYPE_STRING, &key, DBUS_TYPE_INVALID);
	dbus_connection_send(conn, sig, NULL);
	dbus_connection_flush(conn);
	dbus_message_unref(sig);
}

static void
closenotif(int i, unsigned int reason)
{
	unsigned int id = notifs[i].id;

	XDestroyWindow(dpy, notifs[i].win);
	arrdel(notifs, i);
	layout();
	signalclosed(id, reason);
}

static void
closeall(unsigned int reason)
{
	int i;

	for (i = (int)arrlen(notifs) - 1; i >= 0; i--)
		closenotif(i, reason);
}

static void
sendmsg(DBusMessage *msg)
{
	dbus_connection_send(conn, msg, NULL);
	dbus_connection_flush(conn);
	dbus_message_unref(msg);
}

static void
senderr(DBusMessage *msg, const char *name, const char *text)
{
	DBusMessage *reply;

	if (dbus_message_get_no_reply(msg))
		return;
	if ((reply = dbus_message_new_error(msg, name, text)))
		sendmsg(reply);
}

/* org.freedesktop.Notifications.Notify(app_name, replaces_id, app_icon,
 * summary, body, actions, hints, expire_timeout) -> id */
static void
notify(DBusMessage *msg)
{
	DBusMessageIter it, sub, entry, var;
	DBusMessage *reply;
	Notif *n = NULL, nn;
	const char *summary, *body, *key, *s;
	dbus_uint32_t rid, id;
	dbus_int32_t timeout;
	unsigned char byte;
	int i, urgency = Normal, hasdefault = 0, iskey = 1;

	if (!dbus_message_has_signature(msg, "susssasa{sv}i")) {
		senderr(msg, DBUS_ERROR_INVALID_ARGS, "bad Notify signature");
		return;
	}
	dbus_message_iter_init(msg, &it); /* app_name, unused */
	dbus_message_iter_next(&it);
	dbus_message_iter_get_basic(&it, &rid);
	dbus_message_iter_next(&it); /* app_icon, unused */
	dbus_message_iter_next(&it);
	dbus_message_iter_get_basic(&it, &summary);
	dbus_message_iter_next(&it);
	dbus_message_iter_get_basic(&it, &body);
	dbus_message_iter_next(&it);
	/* actions come in key/label pairs; we only honour "default" */
	dbus_message_iter_recurse(&it, &sub);
	while (dbus_message_iter_get_arg_type(&sub) == DBUS_TYPE_STRING) {
		dbus_message_iter_get_basic(&sub, &s);
		if (iskey && !strcmp(s, "default"))
			hasdefault = 1;
		iskey = !iskey;
		dbus_message_iter_next(&sub);
	}
	dbus_message_iter_next(&it);
	dbus_message_iter_recurse(&it, &sub); /* hints: only urgency matters */
	while (dbus_message_iter_get_arg_type(&sub) == DBUS_TYPE_DICT_ENTRY) {
		dbus_message_iter_recurse(&sub, &entry);
		dbus_message_iter_get_basic(&entry, &key);
		dbus_message_iter_next(&entry);
		dbus_message_iter_recurse(&entry, &var);
		if (!strcmp(key, "urgency")
		    && dbus_message_iter_get_arg_type(&var) == DBUS_TYPE_BYTE) {
			dbus_message_iter_get_basic(&var, &byte);
			urgency = byte > Critical ? Normal : (int)byte;
		}
		dbus_message_iter_next(&sub);
	}
	dbus_message_iter_next(&it);
	dbus_message_iter_get_basic(&it, &timeout);

	for (i = 0; rid && i < (int)arrlen(notifs); i++)
		if (notifs[i].id == rid) {
			n = &notifs[i];
			break;
		}
	if (!n) {
		memset(&nn, 0, sizeof nn);
		nn.id = nextid++;
		if (!nextid)
			nextid = 1;
		arrput(notifs, nn);
		n = &notifs[arrlen(notifs) - 1];
	} else if (n->win) {
		/* replacement may change size or urgency; start fresh */
		XDestroyWindow(dpy, n->win);
		n->win = 0;
	}
	n->urgency = urgency;
	n->hasdefault = hasdefault;
	settext(n, summary, body);
	if (timeout > 0)
		n->deadline = nowms() + timeout;
	else if (timeout == 0 || !defaulttimeout[urgency])
		n->deadline = 0;
	else
		n->deadline = nowms() + defaulttimeout[urgency];
	makewin(n);
	layout();
	XFlush(dpy);

	id = n->id;
	if (!dbus_message_get_no_reply(msg)
	    && (reply = dbus_message_new_method_return(msg))) {
		dbus_message_append_args(reply, DBUS_TYPE_UINT32, &id,
		                         DBUS_TYPE_INVALID);
		sendmsg(reply);
	}
}

static void
closecall(DBusMessage *msg)
{
	DBusMessage *reply;
	dbus_uint32_t id;
	int i;

	if (!dbus_message_get_args(msg, NULL, DBUS_TYPE_UINT32, &id,
	                           DBUS_TYPE_INVALID)) {
		senderr(msg, DBUS_ERROR_INVALID_ARGS, "expected uint32 id");
		return;
	}
	for (i = 0; i < (int)arrlen(notifs); i++)
		if (notifs[i].id == id) {
			closenotif(i, Closed);
			break;
		}
	if (!dbus_message_get_no_reply(msg)
	    && (reply = dbus_message_new_method_return(msg)))
		sendmsg(reply);
}

static void
capabilities(DBusMessage *msg)
{
	DBusMessage *reply;
	DBusMessageIter it, arr;
	static const char *caps[] = { "body", "actions" };
	size_t i;

	if (!(reply = dbus_message_new_method_return(msg)))
		return;
	dbus_message_iter_init_append(reply, &it);
	dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "s", &arr);
	for (i = 0; i < sizeof caps / sizeof caps[0]; i++)
		dbus_message_iter_append_basic(&arr, DBUS_TYPE_STRING,
		                               &caps[i]);
	dbus_message_iter_close_container(&it, &arr);
	sendmsg(reply);
}

static void
serverinfo(DBusMessage *msg)
{
	DBusMessage *reply;
	const char *name = "hnd", *vendor = "hackable", *version = "0.1",
	           *spec = "1.2";

	if (!(reply = dbus_message_new_method_return(msg)))
		return;
	dbus_message_append_args(reply, DBUS_TYPE_STRING, &name,
	                         DBUS_TYPE_STRING, &vendor,
	                         DBUS_TYPE_STRING, &version,
	                         DBUS_TYPE_STRING, &spec, DBUS_TYPE_INVALID);
	sendmsg(reply);
}

static void
introspect(DBusMessage *msg)
{
	DBusMessage *reply;
	static const char *xml =
	    "<node>\n"
	    " <interface name=\"" NIFACE "\">\n"
	    "  <method name=\"Notify\">\n"
	    "   <arg type=\"s\" direction=\"in\"/>"
	    "<arg type=\"u\" direction=\"in\"/>"
	    "<arg type=\"s\" direction=\"in\"/>"
	    "<arg type=\"s\" direction=\"in\"/>"
	    "<arg type=\"s\" direction=\"in\"/>"
	    "<arg type=\"as\" direction=\"in\"/>"
	    "<arg type=\"a{sv}\" direction=\"in\"/>"
	    "<arg type=\"i\" direction=\"in\"/>"
	    "<arg type=\"u\" direction=\"out\"/>\n"
	    "  </method>\n"
	    "  <method name=\"CloseNotification\">"
	    "<arg type=\"u\" direction=\"in\"/></method>\n"
	    "  <method name=\"GetCapabilities\">"
	    "<arg type=\"as\" direction=\"out\"/></method>\n"
	    "  <method name=\"GetServerInformation\">"
	    "<arg type=\"s\" direction=\"out\"/>"
	    "<arg type=\"s\" direction=\"out\"/>"
	    "<arg type=\"s\" direction=\"out\"/>"
	    "<arg type=\"s\" direction=\"out\"/></method>\n"
	    "  <signal name=\"NotificationClosed\">"
	    "<arg type=\"u\"/><arg type=\"u\"/></signal>\n"
	    "  <signal name=\"ActionInvoked\">"
	    "<arg type=\"u\"/><arg type=\"s\"/></signal>\n"
	    " </interface>\n"
	    "</node>\n";

	if (!(reply = dbus_message_new_method_return(msg)))
		return;
	dbus_message_append_args(reply, DBUS_TYPE_STRING, &xml,
	                         DBUS_TYPE_INVALID);
	sendmsg(reply);
}

static void
handlemsg(DBusMessage *msg)
{
	if (dbus_message_is_method_call(msg, NIFACE, "Notify"))
		notify(msg);
	else if (dbus_message_is_method_call(msg, NIFACE,
	                                     "CloseNotification"))
		closecall(msg);
	else if (dbus_message_is_method_call(msg, NIFACE, "GetCapabilities"))
		capabilities(msg);
	else if (dbus_message_is_method_call(msg, NIFACE,
	                                     "GetServerInformation"))
		serverinfo(msg);
	else if (dbus_message_is_method_call(msg,
	         "org.freedesktop.DBus.Introspectable", "Introspect"))
		introspect(msg);
	else if (dbus_message_get_type(msg) == DBUS_MESSAGE_TYPE_METHOD_CALL)
		senderr(msg, DBUS_ERROR_UNKNOWN_METHOD, "no such method");
}

static void
handle(XEvent *ev)
{
	int i;

	switch (ev->type) {
	case Expose:
		if (ev->xexpose.count == 0
		    && (i = findwin(ev->xexpose.window)) >= 0)
			draw(&notifs[i]);
		break;
	case ButtonPress:
		if ((i = findwin(ev->xbutton.window)) < 0)
			break;
		if (ev->xbutton.button == Button3) {
			closeall(Dismissed);
		} else {
			if (ev->xbutton.button == Button1
			    && notifs[i].hasdefault)
				signalaction(notifs[i].id);
			closenotif(i, Dismissed);
		}
		break;
	/* keep the stack on top when other windows restack above it */
	case ConfigureNotify:
		if (ev->xconfigure.event == root
		    && findwin(ev->xconfigure.window) < 0)
			raiseall();
		break;
	case MapNotify:
		if (ev->xmap.event == root
		    && findwin(ev->xmap.window) < 0)
			raiseall();
		break;
	}
}

static unsigned long
getcolor(const char *name)
{
	XColor c, dummy;

	if (!XAllocNamedColor(dpy, DefaultColormap(dpy, screen), name,
	                      &c, &dummy))
		die("hnd: cannot allocate color\n");
	return c.pixel;
}

static void
setup(void)
{
	DBusError err;
	struct sigaction sa;
	int i, j, di;

	dbus_error_init(&err);
	if (!(conn = dbus_bus_get(DBUS_BUS_SESSION, &err)))
		die("hnd: cannot connect to the session bus\n");
	dbus_connection_set_exit_on_disconnect(conn, FALSE);
	if (dbus_bus_request_name(conn, NIFACE, DBUS_NAME_FLAG_DO_NOT_QUEUE,
	                          &err)
	    != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER)
		die("hnd: another notification daemon is running\n");
	if (!dbus_connection_get_unix_fd(conn, &dbusfd))
		die("hnd: cannot get the D-Bus connection fd\n");

	if (!(dpy = XOpenDisplay(NULL)))
		die("hnd: cannot open display\n");
	XSetErrorHandler(xerror);
	screen = DefaultScreen(dpy);
	root = RootWindow(dpy, screen);
	haverandr = XRRQueryExtension(dpy, &di, &di);
	for (i = 0; i < 3; i++)
		for (j = 0; j < 4; j++) {
			pixels[i][j] = getcolor(colors[i][j]);
			if (!XftColorAllocName(dpy,
			                       DefaultVisual(dpy, screen),
			                       DefaultColormap(dpy, screen),
			                       colors[i][j], &xftpixels[i][j]))
				die("hnd: cannot allocate colors\n");
		}
	if (!(font = XftFontOpenName(dpy, screen, fontname))
	    && !(font = XftFontOpenName(dpy, screen, "monospace")))
		die("hnd: cannot load font\n");
	lineh = font->ascent + font->descent + (int)linespc;
	gc = XCreateGC(dpy, root, 0, NULL);
	XSelectInput(dpy, root, SubstructureNotifyMask);

	memset(&sa, 0, sizeof sa);
	sa.sa_handler = sighandler;
	sigemptyset(&sa.sa_mask); /* no SA_RESTART: select must wake up */
	sigaction(SIGUSR1, &sa, NULL);
	XSync(dpy, False);
}

static void
run(void)
{
	XEvent ev;
	DBusMessage *msg;
	fd_set fds;
	struct timeval tv, *ptv;
	long now, next, d;
	int i, nfds, xfd = ConnectionNumber(dpy);

	for (;;) {
		if (!dbus_connection_read_write(conn, 0))
			die("hnd: lost the D-Bus connection\n");
		while ((msg = dbus_connection_pop_message(conn))) {
			handlemsg(msg);
			dbus_message_unref(msg);
		}
		if (closereq) {
			closereq = 0;
			closeall(Dismissed);
		}
		now = nowms();
		for (i = (int)arrlen(notifs) - 1; i >= 0; i--)
			if (notifs[i].deadline && notifs[i].deadline <= now)
				closenotif(i, Expired);
		while (XPending(dpy)) {
			XNextEvent(dpy, &ev);
			handle(&ev);
		}
		XFlush(dpy);

		next = 0;
		for (i = 0; i < (int)arrlen(notifs); i++)
			if (notifs[i].deadline
			    && (!next || notifs[i].deadline < next))
				next = notifs[i].deadline;
		ptv = NULL;
		if (next) {
			d = next - nowms();
			if (d < 0)
				d = 0;
			tv.tv_sec = d / 1000;
			tv.tv_usec = (d % 1000) * 1000;
			ptv = &tv;
		}
		FD_ZERO(&fds);
		FD_SET(xfd, &fds);
		FD_SET(dbusfd, &fds);
		nfds = (xfd > dbusfd ? xfd : dbusfd) + 1;
		if (select(nfds, &fds, NULL, NULL, ptv) < 0
		    && errno != EINTR)
			die("hnd: select failed\n");
	}
}

int
main(int argc, char *argv[])
{
	int i;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-w") && i + 1 < argc)
			minwidth = (unsigned int)atoi(argv[++i]);
		else if (!strcmp(argv[i], "-W") && i + 1 < argc)
			maxwpct = (unsigned int)atoi(argv[++i]);
		else
			die("usage: hnd [-w minwidth] [-W maxwidthpct]\n");
	}
	if (!minwidth || maxwpct < 1 || maxwpct > 100)
		die("hnd: bad width option\n");
	setup();
	run();
}
