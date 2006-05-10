/*
 * XOSD
 * 
 * Copyright (c) 2000 Andre Renaud (andre@ignavus.net)
 * 
 * This program is free software; you can redistribute it and/or modify it 
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 * 
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License along 
 * with this program; if not, write to the Free Software Foundation, Inc., 
 * 675 Mass Ave, Cambridge, MA 02139, USA. 
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>

#include <assert.h>
#include <pthread.h>
#include <errno.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/shape.h>
#include <X11/Xatom.h>
#ifdef HAVE_XINERAMA
#  include <X11/extensions/Xinerama.h>
#endif

#include "xosd.h"

/* gcc -O2 optimizes debugging away if Dnone is chosen. {{{ */
static const enum DEBUG_LEVEL {
  Dnone = 0,          /* Nothing */
  Dfunction = (1<<0), /* Function enter/exit */
  Dlocking = (1<<1),  /* Locking */
  Dselect = (1<<2),   /* select() processing */
  Dtrace = (1<<3),    /* Programm progess */
  Dvalue = (1<<4),    /* Computed values */
  Dupdate = (1<<5),   /* Display update processing */
  Dall = -1           /* Everything */
} _xosd_debug_level = Dnone;
#define DEBUG(lvl, fmt, ...) \
  do { \
    if (_xosd_debug_level & lvl) \
      fprintf (stderr, "%s:%-4d %ld@%s: " fmt "\n", __FILE__, __LINE__, \
          pthread_self(), __PRETTY_FUNCTION__ ,## __VA_ARGS__); \
  } while (0)
#define FUNCTION_START(lvl) \
  do { \
    if (_xosd_debug_level & Dfunction && _xosd_debug_level & lvl) \
      fprintf (stderr, "%s:%-4d %ld<%s\n", __FILE__, __LINE__, \
          pthread_self(), __PRETTY_FUNCTION__); \
  } while (0)
#define FUNCTION_END(lvl) \
  do { \
    if (_xosd_debug_level & Dfunction && _xosd_debug_level & lvl) \
      fprintf (stderr, "%s:%-4d %ld>%s\n",  __FILE__, __LINE__, \
          pthread_self(), __PRETTY_FUNCTION__); \
  } while (0)
/* }}} */

// #ifdef X_HAVE_UTF8_STRING
// #define XDRAWSTRING Xutf8DrawString
// #else
#define XDRAWSTRING XmbDrawString
// #endif
#define SLIDER_SCALE 0.8
#define SLIDER_SCALE_ON 0.7

const char *osd_default_font =
  "-misc-fixed-medium-r-semicondensed--*-*-*-*-c-*-*-*";
static const char *osd_default_colour = "green";

// const char* osd_default_font="adobe-helvetica-bold-r-*-*-10-*";
// const char* osd_default_font="-adobe-helvetica-bold-r-*-*-10-*";

typedef struct
{
  enum { LINE_blank, LINE_text, LINE_percentage, LINE_slider } type;

  char *text;
  int width;
  int percentage;
} xosd_line;

struct xosd
{
  pthread_t event_thread;       /* handles X events */
  pthread_t timeout_thread;     /* handle automatic hide after
                                 * timeout */

  pthread_mutex_t mutex;        /* mutual exclusion to protect struct */
  pthread_cond_t cond_hide;     /* signal hide events */
  pthread_cond_t cond_time;     /* signal timeout */

  Display *display;
  int screen;
  Window window;
  unsigned int depth;
  Pixmap mask_bitmap;
  Pixmap line_bitmap;
  Visual *visual;

  XFontSet fontset;
  XRectangle *extent;

  GC gc;
  GC mask_gc;
  GC mask_gc_back;

  int screen_width;
  int screen_height;
  int screen_xpos;
  int height;
  int line_height;
  int x;
  int y;
  xosd_pos pos;
  xosd_align align;
  int hoffset;
  int voffset;
  int shadow_offset;
  XColor shadow_colour;
  unsigned int shadow_pixel;
  int outline_offset;
  XColor outline_colour;
  unsigned int outline_pixel;
  int bar_length;

  int mapped;
  int done;

  unsigned int pixel;
  XColor colour;
  Colormap colourmap;

  xosd_line *lines;
  int number_lines;

  int timeout;                  /* delta time */
  struct timespec timeout_time; /* Next absolute timeout */

  enum { FILL_OUTLINE = 1, FILL_SHADOW = 2, FILL_FACE = 4 } fill_mask;
};

/** Global error string. */
char *xosd_error;

/*
 * Forward declarations of internal functions. 
 */
static void set_timeout(xosd *);
static void show(xosd *);
static void hide(xosd *);
static void update_pos(xosd * osd);

static /*inline*/ void _xosd_lock(xosd *osd) {
  FUNCTION_START(Dlocking);
  pthread_mutex_lock(&osd->mutex);
  FUNCTION_END(Dlocking);
}
static /*inline*/ void _xosd_unlock(xosd *osd) {
  FUNCTION_START(Dlocking);
  pthread_mutex_unlock(&osd->mutex);
}

static void
draw_bar(xosd * osd, Drawable d, GC gc, int x, int y,
         int percent, int is_slider, int set_color, int draw_all)
{
  int barw, barh, nbars;
  int on, i, xx;
  struct bar
  {
    int w, h, y;
  } bar[2];

  assert(osd);
  FUNCTION_START(Dfunction);

  /*
   * bar size and dimension 
   */
  bar[0].h = bar[1].h = barh = -osd->extent->y;
  bar[0].w = bar[1].w = barw = barh / 2;
  bar[0].y = bar[1].y = y;

  bar[0].h /= 3;
  bar[1].w *= SLIDER_SCALE_ON;
  bar[0].w *= SLIDER_SCALE;
  bar[0].y += bar[0].h;

  // check how to behave
  if (osd->bar_length == -1) {
    nbars = (osd->screen_width * SLIDER_SCALE) / barw;
  } else {
    nbars = osd->bar_length;

    // fix x coord
    switch (osd->align) {
    case XOSD_left:
      break;
    case XOSD_center:
      x = (osd->screen_width - (nbars * barw)) / 2;
      break;
    case XOSD_right:
      x = osd->screen_width - (nbars * barw) - x;
      break;
    default:
      break;
    }
  }
  on = ((nbars - is_slider) * percent) / 100;

  DEBUG(Dvalue, "percent=%d, nbars=%d, on=%d", percent, nbars, on);

  /*
   * Outline 
   */
  if (osd->outline_offset && (draw_all || !(osd->fill_mask & FILL_OUTLINE))) {
    if (set_color)
      XSetForeground(osd->display, gc, osd->outline_pixel);
    for (xx = x, i = 0; i < nbars; xx += barw, i++) {
      struct bar *b = &(bar[is_slider ? (i == on) : (i < on)]);
      XFillRectangle(osd->display, d, gc,
                     xx - osd->outline_offset,
                     b->y - osd->outline_offset,
                     b->w + (2 * osd->outline_offset),
                     b->h + (2 * osd->outline_offset));
    }
  }
/*
 * Shadow 
 */
  if (osd->shadow_offset && (draw_all || !(osd->fill_mask & FILL_SHADOW))) {
    if (set_color)
      XSetForeground(osd->display, gc, osd->shadow_pixel);
    for (xx = x, i = 0; i < nbars; xx += barw, i++) {
      struct bar *b = &(bar[is_slider ? (i == on) : (i < on)]);
      XFillRectangle(osd->display, d, gc,
                     xx + osd->shadow_offset,
                     b->y + osd->shadow_offset, b->w, b->h);
    }
  }
  /*
   * Bar/Slider 
   */
  if (draw_all || !(osd->fill_mask & FILL_FACE)) {
    if (set_color)
      XSetForeground(osd->display, gc, osd->pixel);
    for (xx = x, i = 0; i < nbars; xx += barw, i++) {
      struct bar *b = &(bar[is_slider ? (i == on) : (i < on)]);
      XFillRectangle(osd->display, d, gc, xx, b->y, b->w, b->h);
    }
  }
}

static void
draw_with_mask(xosd * osd, xosd_line * l, int inX, int inPlace, int inY,
               int draw_line_bitmap)
{
  int len = strlen(l->text);
  FUNCTION_START(Dfunction);
  XDRAWSTRING(osd->display,
              osd->mask_bitmap,
              osd->fontset,
              osd->mask_gc, inX, inPlace + inY, l->text, len);
  if (draw_line_bitmap) {
    XDRAWSTRING(osd->display,
                osd->line_bitmap,
                osd->fontset, osd->gc, inX, inY, l->text, len);
  }
}

static void
expose_line(xosd * osd, int line)
{
  int x = 10;
  int y = osd->line_height * line;
  int i;
  xosd_line *l = &osd->lines[line];
  assert(osd);
  FUNCTION_START(Dfunction);

  osd->fill_mask = osd->outline_offset ? FILL_OUTLINE :
    osd->shadow_offset ? FILL_SHADOW : FILL_FACE;

  switch (osd->fill_mask) {
  case FILL_FACE:
    XSetForeground(osd->display, osd->gc, osd->pixel);
    break;
  case FILL_SHADOW:
    XSetForeground(osd->display, osd->gc, osd->shadow_pixel);
    break;
  case FILL_OUTLINE:
    XSetForeground(osd->display, osd->gc, osd->outline_pixel);
    break;
  }

  XFillRectangle(osd->display, osd->line_bitmap, osd->gc,
      0, 0, osd->screen_width, osd->line_height);

  /*
   * don't need to lock here because functions that call expose_line
   * should have already locked the mutex 
   */
  XFillRectangle(osd->display, osd->mask_bitmap, osd->mask_gc_back,
                 0, y, osd->screen_width, osd->line_height);

  switch (l->type) {
  case LINE_blank:
    break;

  case LINE_text:
    if (l->text == NULL)
      break;
    if (!osd->fontset) {
      DEBUG(Dtrace, "CRITICAL: No fontset");
      return;
    }

    switch (osd->align) {
    case XOSD_left:
      break;
    case XOSD_center:
      x = (osd->screen_width - l->width) / 2;
      break;
    case XOSD_right:
      x = osd->screen_width - l->width - x;
      break;
    default:
      break;
    }

    osd->extent->y -= osd->outline_offset;
    if (osd->shadow_offset) {

      XSetForeground(osd->display, osd->gc, osd->shadow_pixel);

      draw_with_mask(osd, l,
                     x + osd->shadow_offset,
                     y, osd->shadow_offset - osd->extent->y,
                     !(osd->fill_mask & FILL_SHADOW));

    }

    if (osd->outline_offset) {
      XSetForeground(osd->display, osd->gc, osd->outline_pixel);
      int draw_line_bitmap = !(osd->fill_mask & FILL_OUTLINE);

      /* FIXME: echo . | osd_cat -O 50 -p middle -A center */
      for (i = 1; i <= osd->outline_offset; i++) {
        draw_with_mask(osd, l, x - i, y, -i - osd->extent->y, draw_line_bitmap);
        draw_with_mask(osd, l, x    , y, -i - osd->extent->y, draw_line_bitmap);
        draw_with_mask(osd, l, x + i, y, -i - osd->extent->y, draw_line_bitmap);
        draw_with_mask(osd, l, x - i, y,     -osd->extent->y, draw_line_bitmap);
        draw_with_mask(osd, l, x + i, y,     -osd->extent->y, draw_line_bitmap);
        draw_with_mask(osd, l, x - i, y,  i - osd->extent->y, draw_line_bitmap);
        draw_with_mask(osd, l, x    , y,  i - osd->extent->y, draw_line_bitmap);
        draw_with_mask(osd, l, x + i, y,  i - osd->extent->y, draw_line_bitmap);
      }
    }

    XSetForeground(osd->display, osd->gc, osd->pixel);

    osd->extent->y += osd->outline_offset;

    draw_with_mask(osd, l, x, y, -osd->extent->y,
                   !(osd->fill_mask & FILL_FACE));
    XCopyArea(osd->display, osd->line_bitmap, osd->window, osd->gc, 0, 0,
              osd->screen_width, osd->line_height, 0, y);
    break;

  case LINE_percentage:
  case LINE_slider:

    switch (osd->align) {
    case XOSD_left:
      break;
    case XOSD_center:
      x = osd->screen_width * ((1 - SLIDER_SCALE) / 2);
      break;
    case XOSD_right:
      x = osd->screen_width * (1 - SLIDER_SCALE);
      break;
    default:
      break;
    }

    draw_bar(osd, osd->mask_bitmap, osd->mask_gc, x, y, l->percentage,
             l->type == LINE_slider, 0, 1);
    draw_bar(osd, osd->line_bitmap, osd->gc, x, 0, l->percentage,
             l->type == LINE_slider, 1, 0);

    XCopyArea(osd->display, osd->line_bitmap, osd->window, osd->gc, 0, 0,
              osd->screen_width, osd->line_height, 0, y);

    break;
  }
}

/** Handle X11 events
 * This is running in it's own thread for Expose-events.
 */
static void *
event_loop(void *osdv)
{
  xosd *osd = osdv;
  XEvent report;
  int line, y;

  FUNCTION_START(Dfunction);
  DEBUG(Dtrace, "event thread started");
  assert(osd);
  usleep(500);

  while (!osd->done) {
    // DEBUG(Dtrace, "checking window event");
    XWindowEvent(osd->display, osd->window, ExposureMask, &report);
    if (osd->done)
      break;

    report.type &= 0x7f;        /* remove the sent by server/manual send
                                 * flag */

    switch (report.type) {
    case Expose:
      DEBUG(Dtrace, "expose");
      if (report.xexpose.count == 0) {
        _xosd_lock(osd);
        for (line = 0; line < osd->number_lines; line++) {
          y = osd->line_height * line;
          if (report.xexpose.y >= y + osd->line_height)
            continue;
          if (report.xexpose.y + report.xexpose.height < y)
            continue;
          expose_line(osd, line);
        }
        XShapeCombineMask(osd->display, osd->window, ShapeBounding,
                          0, 0, osd->mask_bitmap, ShapeSet);
        XFlush(osd->display);
        _xosd_unlock(osd);
      }
      break;

    case NoExpose:
      break;

    default:
      // fprintf (stderr, "%d\n", report.type);
      break;
    }
  }

  return NULL;
}


/** Handle timeout events to auto hide output.
 * This is running in it's own thread and waits for timeout notifications.
 */
static void *
timeout_loop(void *osdv)
{
  xosd *osd = osdv;
  assert(osd);
  FUNCTION_START(Dfunction);

  _xosd_lock(osd);
  while (!osd->done) {
    /*
     * Wait for timeout or change of timeout 
     */
    int cond;

    if (osd->timeout_time.tv_sec) {
      DEBUG(Dtrace, "waiting for timeout");
      cond = pthread_cond_timedwait(&osd->cond_time,
                                    &osd->mutex, &osd->timeout_time);

      /*
       * check for timeout. Other condition is variable signaled,
       * which means new timeout 
       */
      if (cond == ETIMEDOUT) {
        DEBUG(Dtrace, "hiding");
        osd->timeout_time.tv_sec = 0;

        if (osd->mapped) {
          hide(osd);
        }
      }
    } else {
      /*
       * once we get the condition variable being signaled, then
       * it's time for another timeout 
       */
      DEBUG(Dtrace, "waiting on condition variable");
      cond = pthread_cond_wait(&osd->cond_time, &osd->mutex);

    }
  }
  _xosd_unlock(osd);

  return NULL;
}

static int
display_string(xosd * osd, xosd_line * l, char *string)
{
  assert(osd);
  FUNCTION_START(Dfunction);
  if (!osd->fontset) {
    DEBUG(Dtrace, "CRITICAL: No fontset");
    return -1;
  }

  if (string && *string) {
    XRectangle rect;
    int len = strlen(string);
    l->type = LINE_text;
    if (l->text == NULL) {
      l->text = strdup(string);
    } else {
      realloc(l->text, len + 1);
      strcpy(l->text, string);
    }
    _xosd_lock(osd);
    XmbTextExtents(osd->fontset, l->text, len, NULL, &rect);
    l->width = rect.width;
    _xosd_unlock(osd);
  } else {
    l->type = LINE_blank;
    if (l->text != NULL) {
      free(l->text);
      l->text = NULL;
    }
  }

  return 0;
}

static int
display_percentage(xosd * osd, xosd_line * l, int percentage)
{
  assert(osd);
  FUNCTION_START(Dfunction);

  if (percentage < 0)
    percentage = 0;
  if (percentage > 100)
    percentage = 100;

  l->type = LINE_percentage;
  l->percentage = percentage;

  return 0;
}

static int
display_slider(xosd * osd, xosd_line * l, int percentage)
{
  assert(osd);
  FUNCTION_START(Dfunction);

  if (percentage < 0)
    percentage = 0;
  if (percentage > 100)
    percentage = 100;

  l->type = LINE_slider;
  l->percentage = percentage;

  return 0;
}

static void
resize(xosd * osd)
{                               /* Requires mutex lock. */
  assert(osd);
  FUNCTION_START(Dfunction);
  XResizeWindow(osd->display, osd->window, osd->screen_width, osd->height);
  XFreePixmap(osd->display, osd->mask_bitmap);
  osd->mask_bitmap =
    XCreatePixmap(osd->display, osd->window, osd->screen_width,
                  osd->height, 1);
  XFreePixmap(osd->display, osd->line_bitmap);
  osd->line_bitmap =
    XCreatePixmap(osd->display, osd->window, osd->screen_width,
                  osd->line_height, osd->depth);
}

static int
force_redraw(xosd * osd, int line)
{                               /* Requires mutex lock. */
  assert(osd);
  FUNCTION_START(Dfunction);
  resize(osd);
  for (line = 0; line < osd->number_lines; line++) {
    expose_line(osd, line);
  }
  XShapeCombineMask(osd->display, osd->window, ShapeBounding, 0, 0,
                    osd->mask_bitmap, ShapeSet);
  XFlush(osd->display);

  /*
   * if (!osd->mapped) show (osd); 
   */

  return 0;
}

static int
set_font(xosd * osd, const char *font)
{                               /* Requires mutex lock. */
  XFontSet fontset2;
  char **missing;
  int nmissing;
  char *defstr;
  int line;

  XFontSetExtents *extents;

  assert(osd);
  FUNCTION_START(Dfunction);

  /*
   * Try to create the new font. If it doesn't succeed, keep old font. 
   */
  fontset2 = XCreateFontSet(osd->display, font, &missing, &nmissing, &defstr);
  XFreeStringList(missing);
  if (fontset2 == NULL) {
    xosd_error = "Requested font not found";
    return -1;
  }

  if (osd->fontset) {
    XFreeFontSet(osd->display, osd->fontset);
    osd->fontset = NULL;
  }
  osd->fontset = fontset2;

  extents = XExtentsOfFontSet(osd->fontset);
  osd->extent = &extents->max_logical_extent;

  osd->line_height =
    osd->extent->height + osd->shadow_offset + 2 * osd->outline_offset;
  osd->height = osd->line_height * osd->number_lines;
  for (line = 0; line < osd->number_lines; line++) {
    xosd_line *l = &osd->lines[line];

    if (l->type == LINE_text && l->text != NULL) {
      XRectangle rect;

      XmbTextExtents(osd->fontset, l->text, strlen(l->text), NULL, &rect);
      l->width = rect.width;
    }
  }

  return 0;
}

static int
parse_colour(xosd * osd, XColor * col, int *pixel, const char *colour)
{
  int retval = 0;

  FUNCTION_START(Dfunction);
  DEBUG(Dtrace, "getting colourmap");
  osd->colourmap = DefaultColormap(osd->display, osd->screen);

  DEBUG(Dtrace, "parsing colour");
  if (XParseColor(osd->display, osd->colourmap, colour, col)) {

    DEBUG(Dtrace, "attempting to allocate colour");
    if (XAllocColor(osd->display, osd->colourmap, col)) {
      DEBUG(Dtrace, "allocation sucessful");
      *pixel = col->pixel;
    } else {
      DEBUG(Dtrace, "defaulting to white. could not allocate colour");
      *pixel = WhitePixel(osd->display, osd->screen);
      retval = -1;
    }
  } else {
    DEBUG(Dtrace, "could not poarse colour. defaulting to white");
    *pixel = WhitePixel(osd->display, osd->screen);
    retval = -1;
  }

  return retval;
}


static int
set_colour(xosd * osd, const char *colour)
{                               /* Requires mutex lock. */
  int retval = 0;

  FUNCTION_START(Dfunction);
  assert(osd);

  retval = parse_colour(osd, &osd->colour, &osd->pixel, colour);

  DEBUG(Dtrace, "setting foreground");
  XSetForeground(osd->display, osd->gc, osd->pixel);
  DEBUG(Dtrace, "setting background");
  XSetBackground(osd->display, osd->gc,
                 WhitePixel(osd->display, osd->screen));

  FUNCTION_END(Dfunction);
  return retval;
}

/* Tell window manager to put window topmost. {{{ */
void stay_on_top(Display * dpy, Window win)
{
  Atom gnome, net_wm, type;
  int format;
  unsigned long nitems, bytesafter;
  unsigned char *args = NULL;
  Window root = DefaultRootWindow(dpy);

  FUNCTION_START(Dfunction);
  /*
   * build atoms 
   */
  gnome = XInternAtom(dpy, "_WIN_SUPPORTING_WM_CHECK", False);
  net_wm = XInternAtom(dpy, "_NET_SUPPORTED", False);

  /*
   * gnome-compilant 
   * tested with icewm + WindowMaker 
   */
  if (Success == XGetWindowProperty
      (dpy, root, gnome, 0, (65536 / sizeof(long)), False,
       AnyPropertyType, &type, &format, &nitems, &bytesafter, &args) &&
      nitems > 0) {
    /*
     * FIXME: check capabilities 
     */
    XClientMessageEvent xev;
    Atom gnome_layer = XInternAtom(dpy, "_WIN_LAYER", False);

    memset(&xev, 0, sizeof(xev));
    xev.type = ClientMessage;
    xev.window = win;
    xev.message_type = gnome_layer;
    xev.format = 32;
    xev.data.l[0] = 6 /* WIN_LAYER_ONTOP */;

    XSendEvent(dpy, DefaultRootWindow(dpy), False, SubstructureNotifyMask,
        (XEvent *) & xev);
    XFree(args);
  }
  /*
   * netwm compliant.
   * tested with kde 
   */
  else if (Success == XGetWindowProperty
           (dpy, root, net_wm, 0, (65536 / sizeof(long)), False,
            AnyPropertyType, &type, &format, &nitems, &bytesafter, &args)
           && nitems > 0) {
    XEvent e;
    Atom net_wm_state = XInternAtom(dpy, "_NET_WM_STATE", False);
    Atom net_wm_top = XInternAtom(dpy, "_NET_WM_STATE_STAYS_ON_TOP", False);

    memset(&e, 0, sizeof(e));
    e.xclient.type = ClientMessage;
    e.xclient.message_type = net_wm_state;
    e.xclient.display = dpy;
    e.xclient.window = win;
    e.xclient.format = 32;
    e.xclient.data.l[0] = 1 /* _NET_WM_STATE_ADD */;
    e.xclient.data.l[1] = net_wm_top;
    e.xclient.data.l[2] = 0l;
    e.xclient.data.l[3] = 0l;
    e.xclient.data.l[4] = 0l;

    XSendEvent(dpy, DefaultRootWindow(dpy), False,
        SubstructureRedirectMask, &e);
    XFree(args);
  }
  XRaiseWindow(dpy, win);
}
/* }}} */

/* xosd_init -- Create a new xosd "object" {{{
 * Deprecated: Use xosd_create. */
xosd *
xosd_init(const char *font, const char *colour, int timeout, xosd_pos pos,
          int voffset, int shadow_offset, int number_lines)
{
  xosd *osd = xosd_create(number_lines);

  FUNCTION_START(Dfunction);
  if (osd == NULL) {
    return NULL;
  }

  if (xosd_set_font(osd, font) == -1) {
    if (xosd_set_font(osd, osd_default_font) == -1) {
      xosd_destroy(osd);
      /*
       * we do not set xosd_error, as set_font has already set it to 
       * a sensible error message. 
       */
      return NULL;
    }
  }
  xosd_set_colour(osd, colour);
  xosd_set_timeout(osd, timeout);
  xosd_set_pos(osd, pos);
  xosd_set_vertical_offset(osd, voffset);
  xosd_set_shadow_offset(osd, shadow_offset);

  resize(osd);

  return osd;
}
/* }}} */

/* xosd_create -- Create a new xosd "object" {{{ */
xosd *
xosd_create(int number_lines)
{
  xosd *osd;
  int event_basep, error_basep, i;
  char *display;
  XSetWindowAttributes setwinattr;
#ifdef HAVE_XINERAMA
  int screens;
  int dummy_a, dummy_b;
  XineramaScreenInfo *screeninfo = NULL;
#endif

  FUNCTION_START(Dfunction);
  DEBUG(Dtrace, "X11 thread support");
  if (!XInitThreads()) {
    xosd_error = "xlib is not thread-safe";
    return NULL;
  }

  DEBUG(Dtrace, "getting display");
  display = getenv("DISPLAY");
  if (!display) {
    xosd_error = "No display";
    return NULL;
  }

  DEBUG(Dtrace, "Mallocing osd");
  osd = malloc(sizeof(xosd));
  memset(osd, 0, sizeof(xosd));
  if (osd == NULL) {
    xosd_error = "Out of memory";
    goto error0;
  }

  DEBUG(Dtrace, "initializing mutex");
  pthread_mutex_init(&osd->mutex, NULL);
  DEBUG(Dtrace, "initializing condition");
  pthread_cond_init(&osd->cond_hide, NULL);
  pthread_cond_init(&osd->cond_time, NULL);

  DEBUG(Dtrace, "initializing number lines");
  osd->number_lines = number_lines;
  osd->lines = malloc(sizeof(xosd_line) * osd->number_lines);
  if (osd->lines == NULL) {
    xosd_error = "Out of memory";
    goto error1;
  }

  for (i = 0; i < osd->number_lines; i++) {
    osd->lines[i].type = LINE_blank;
    osd->lines[i].text = NULL;
  }

  DEBUG(Dtrace, "misc osd variable initialization");
  osd->mapped = 0;
  osd->done = 0;
  osd->pos = XOSD_top;
  osd->hoffset = 0;
  osd->align = XOSD_left;
  osd->voffset = 0;
  osd->timeout = -1;
  osd->timeout_time.tv_sec = 0;
  osd->fontset = NULL;

  DEBUG(Dtrace, "Display query");
  osd->display = XOpenDisplay(display);
  if (!osd->display) {
    xosd_error = "Cannot open display";
    goto error2;
  }
  osd->screen = XDefaultScreen(osd->display);

  DEBUG(Dtrace, "x shape extension query");
  if (!XShapeQueryExtension(osd->display, &event_basep, &error_basep)) {
    xosd_error = "X-Server does not support shape extension";
    goto error3;
  }

  osd->visual = DefaultVisual(osd->display, osd->screen);
  osd->depth = DefaultDepth(osd->display, osd->screen);

  DEBUG(Dtrace, "font selection info");
  set_font(osd, osd_default_font);
  if (osd->fontset == NULL) {
    /*
     * if we still don't have a fontset, then abort 
     */
    xosd_error = "Default font not found";
    goto error3;
  }

  DEBUG(Dtrace, "width and height initialization");
#ifdef HAVE_XINERAMA
  if (XineramaQueryExtension(osd->display, &dummy_a, &dummy_b) &&
      (screeninfo = XineramaQueryScreens(osd->display, &screens)) &&
      XineramaIsActive(osd->display)) {
    osd->screen_width = screeninfo[0].width;
    osd->screen_height = screeninfo[0].height;
    osd->screen_xpos = screeninfo[0].x_org;
  } else
#endif
  {
    osd->screen_width = XDisplayWidth(osd->display, osd->screen);
    osd->screen_height = XDisplayHeight(osd->display, osd->screen);
    osd->screen_xpos = 0;
  }
#ifdef HAVE_XINERAMA
  if (screeninfo)
    XFree(screeninfo);
#endif
  osd->height = osd->line_height * osd->number_lines;
  osd->bar_length = -1;         // init bar_length with -1: draw_bar
  // behaves like unpached

  DEBUG(Dtrace, "creating X Window");
  setwinattr.override_redirect = 1;

  osd->window = XCreateWindow(osd->display,
                              XRootWindow(osd->display, osd->screen),
                              0, 0,
                              osd->screen_width, osd->height,
                              0,
                              osd->depth,
                              CopyFromParent,
                              osd->visual, CWOverrideRedirect, &setwinattr);
  XStoreName(osd->display, osd->window, "XOSD");

  osd->mask_bitmap =
    XCreatePixmap(osd->display, osd->window, osd->screen_width,
                  osd->height, 1);
  osd->line_bitmap =
    XCreatePixmap(osd->display, osd->window, osd->screen_width,
                  osd->line_height, osd->depth);

  osd->gc = XCreateGC(osd->display, osd->window, 0, NULL);
  osd->mask_gc = XCreateGC(osd->display, osd->mask_bitmap, 0, NULL);
  osd->mask_gc_back = XCreateGC(osd->display, osd->mask_bitmap, 0, NULL);

  XSetForeground(osd->display, osd->mask_gc_back,
                 BlackPixel(osd->display, osd->screen));
  XSetBackground(osd->display, osd->mask_gc_back,
                 WhitePixel(osd->display, osd->screen));

  XSetForeground(osd->display, osd->mask_gc,
                 WhitePixel(osd->display, osd->screen));
  XSetBackground(osd->display, osd->mask_gc,
                 BlackPixel(osd->display, osd->screen));


  DEBUG(Dtrace, "setting colour");
  set_colour(osd, osd_default_colour);

  DEBUG(Dtrace, "Request exposure events");
  XSelectInput(osd->display, osd->window, ExposureMask);

  DEBUG(Dtrace, "stay on top");
  stay_on_top(osd->display, osd->window);

  DEBUG(Dtrace, "finale resize");
  update_pos(osd);              /* Shoule be inside lock, but no threads
                                 * yet */
  resize(osd);                  /* Shoule be inside lock, but no threads
                                 * yet */

  DEBUG(Dtrace, "initializing event thread");
  pthread_create(&osd->event_thread, NULL, event_loop, osd);

  DEBUG(Dtrace, "initializing timeout thread");
  pthread_create(&osd->timeout_thread, NULL, timeout_loop, osd);

  return osd;

error3:
  XCloseDisplay(osd->display);
error2:
  free(osd->lines);
error1:
  pthread_cond_destroy(&osd->cond_time);
  pthread_cond_destroy(&osd->cond_hide);
  pthread_mutex_destroy(&osd->mutex);
  free(osd);
error0:
  return NULL;
}
/* }}} */

/* xosd_uninit -- Destroy a xosd "object" {{{
 * Deprecated: Use xosd_destroy. */
int
xosd_uninit(xosd * osd)
{
  FUNCTION_START(Dfunction);
  return xosd_destroy(osd);
}
/* }}} */

/* xosd_destroy -- Destroy a xosd "object" {{{ */
int
xosd_destroy(xosd * osd)
{
  int i;

  FUNCTION_START(Dfunction);
  if (osd == NULL)
    return -1;

  DEBUG(Dtrace, "waiting for threads to exit");
  _xosd_lock(osd);
  osd->done = 1;
  /*
   * Send signal to timeout-thread, will quit. 
   */
  pthread_cond_signal(&osd->cond_time);
  _xosd_unlock(osd);

  /*
   * Send fake XExpose-event to event-thread, will quit. 
   */
  DEBUG(Dtrace, "Send fake expose");
  {
    XEvent event = {
      .xexpose = {
                  .type = Expose,
                  .send_event = True,
                  .display = osd->display,
                  .window = osd->window,
                  .count = 0,
                  }
    };
    XSendEvent(osd->display, osd->window, False, ExposureMask, &event);
    XFlush(osd->display);
  }

  DEBUG(Dtrace, "join threads");
  pthread_join(osd->event_thread, NULL);
  pthread_join(osd->timeout_thread, NULL);

  DEBUG(Dtrace, "freeing X resources");
  XFreeGC(osd->display, osd->gc);
  XFreeGC(osd->display, osd->mask_gc);
  XFreeGC(osd->display, osd->mask_gc_back);
  XFreePixmap(osd->display, osd->line_bitmap);
  if (osd->fontset)
    XFreeFontSet(osd->display, osd->fontset);
  XFreePixmap(osd->display, osd->mask_bitmap);
  XDestroyWindow(osd->display, osd->window);

  XCloseDisplay(osd->display);

  DEBUG(Dtrace, "freeing lines");
  for (i = 0; i < osd->number_lines; i++) {
    if (osd->lines[i].text)
      free(osd->lines[i].text);
  }
  free(osd->lines);

  DEBUG(Dtrace, "destroying condition and mutex");
  pthread_cond_destroy(&osd->cond_time);
  pthread_cond_destroy(&osd->cond_hide);
  pthread_mutex_destroy(&osd->mutex);

  DEBUG(Dtrace, "freeing osd structure");
  free(osd);

  FUNCTION_END(Dfunction);
  return 0;
}
/* }}} */

/* xosd_set_bar_length  -- Set length of percentage and slider bar {{{ */
int
xosd_set_bar_length(xosd * osd, int length)
{
  FUNCTION_START(Dfunction);
  if (osd == NULL)
    return -1;

  if (length == 0)
    return -1;
  if (length < -1)
    return -1;

  osd->bar_length = length;

  return 0;
}
/* }}} */

/* xosd_display -- Display information {{{ */
int
xosd_display(xosd * osd, int line, xosd_command command, ...)
{
  int len;
  va_list a;
  char *string;
  int percent;
  xosd_line *l = &osd->lines[line];

  FUNCTION_START(Dfunction);
  if (osd == NULL)
    return -1;

  if (line < 0 || line >= osd->number_lines) {
    xosd_error = "xosd_display: Invalid Line Number";
    return -1;
  }

  va_start(a, command);
  switch (command) {
  case XOSD_string:
    {
      string = va_arg(a, char *);
      len = display_string(osd, l, string);
      break;
    }

  case XOSD_printf:
    {
      char buf[2000];

      string = va_arg(a, char *);
      if (vsnprintf(buf, sizeof(buf), string, a) >= sizeof(buf)) {
        return -1;
      }
      len = display_string(osd, l, buf);
      break;
    }

  case XOSD_percentage:
    {
      percent = va_arg(a, int);

      display_percentage(osd, l, percent);

      len = percent;
      break;
    }

  case XOSD_slider:
    {
      percent = va_arg(a, int);

      display_slider(osd, l, percent);

      len = percent;
      break;
    }

  default:
    {
      len = -1;
      xosd_error = "xosd_display: Unknown command";
    }
  }
  va_end(a);

  _xosd_lock(osd);
  force_redraw(osd, line);
  if (!osd->mapped)
    show(osd);
  set_timeout(osd);
  _xosd_unlock(osd);

  return len;
}
/* }}} */

/* xosd_is_onscreen -- Returns weather the display is show {{{ */
int
xosd_is_onscreen(xosd * osd)
{
  FUNCTION_START(Dfunction);
  if (osd == NULL)
    return -1;
  return osd->mapped;
}
/* }}} */

/* xosd_wait_until_no_display -- Wait until nothing is displayed {{{ */
int
xosd_wait_until_no_display(xosd * osd)
{
  FUNCTION_START(Dfunction);
  if (osd == NULL)
    return -1;

  _xosd_lock(osd);
  while (osd->mapped)
    pthread_cond_wait(&osd->cond_hide, &osd->mutex);
  _xosd_unlock(osd);

  return 0;
}
/* }}} */

/* xosd_set_colour -- Change the colour of the display {{{ */
int
xosd_set_colour(xosd * osd, const char *colour)
{
  int retval = 0;

  FUNCTION_START(Dfunction);
  if (osd == NULL)
    return -1;

  _xosd_lock(osd);
  retval = set_colour(osd, colour);
  force_redraw(osd, -1);
  _xosd_unlock(osd);

  return retval;
}
/* }}} */

/* xosd_set_shadow_colour -- Change the colour of the shadow {{{ */
int
xosd_set_shadow_colour(xosd * osd, const char *colour)
{
  int retval = 0;

  FUNCTION_START(Dfunction);
  if (osd == NULL)
    return -1;

  _xosd_lock(osd);
  retval = parse_colour(osd, &osd->shadow_colour, &osd->shadow_pixel, colour);
  force_redraw(osd, -1);
  _xosd_unlock(osd);

  return retval;
}
/* }}} */

/* xosd_set_outline_colour -- Change the colour of the outline {{{ */
int
xosd_set_outline_colour(xosd * osd, const char *colour)
{
  int retval = 0;

  FUNCTION_START(Dfunction);
  if (osd == NULL)
    return -1;

  _xosd_lock(osd);
  retval =
    parse_colour(osd, &osd->outline_colour, &osd->outline_pixel, colour);
  force_redraw(osd, -1);
  _xosd_unlock(osd);

  return retval;
}
/* }}} */

/* xosd_set_font -- Change the text-display font {{{
 * Might return error if fontset can't be created. **/
int
xosd_set_font(xosd * osd, const char *font)
{
  int ret = 0;

  FUNCTION_START(Dfunction);
  if (font == NULL)
    return -1;
  if (osd == NULL)
    return -1;

  _xosd_lock(osd);
  ret = set_font(osd, font);
  if (ret == 0)
    resize(osd);
  _xosd_unlock(osd);

  return ret;
}
/* }}} */

static void
update_pos(xosd * osd)
{                               /* Requires mutex lock. */
  assert(osd);
  FUNCTION_START(Dfunction);
  switch (osd->pos) {
  case XOSD_bottom:
    osd->y = osd->screen_height - osd->height - osd->voffset;
    break;
  case XOSD_middle:
    osd->y = osd->screen_height / 2 - osd->height - osd->voffset;
    break;
  case XOSD_top:
  default:
    osd->y = osd->voffset;
    break;
  }

  switch (osd->align) {
  case XOSD_left:
    osd->x = osd->hoffset + osd->screen_xpos;
    break;
  case XOSD_center:
    osd->x = osd->hoffset + osd->screen_xpos;
    /*
     * which direction should this default to, left or right offset 
     */
    break;
  case XOSD_right:
    // osd->x = XDisplayWidth (osd->display, osd->screen) - osd->width 
    // - osd->hoffset; 
    osd->x = -(osd->hoffset) + osd->screen_xpos;
    /*
     * neither of these work right, I want the offset to flip so
     * +offset is to the left instead of to the right when aligned
     * right 
     */
    break;
  default:
    osd->x = 0;
    break;
  }

  XMoveWindow(osd->display, osd->window, osd->x, osd->y);
}

/* xosd_set_shadow_offset -- Change the offset of the text shadow {{{ */
int
xosd_set_shadow_offset(xosd * osd, int shadow_offset)
{
  FUNCTION_START(Dfunction);
  if (osd == NULL)
    return -1;

  if (shadow_offset < 0)
    return -1;
  _xosd_lock(osd);
  osd->shadow_offset = shadow_offset;
  force_redraw(osd, -1);
  _xosd_unlock(osd);

  return 0;
}
/* }}} */

/* xosd_set_outline_offset -- Change the offset of the text outline {{{ */
int
xosd_set_outline_offset(xosd * osd, int outline_offset)
{
  FUNCTION_START(Dfunction);
  if (osd == NULL)
    return -1;

  if (outline_offset < 0)
    return -1;
  _xosd_lock(osd);
  osd->outline_offset = outline_offset;
  force_redraw(osd, -1);
  _xosd_unlock(osd);

  return 0;
}
/* }}} */

/* xosd_set_vertical_offset -- Change the number of pixels the display is offset from the position {{{ */
int
xosd_set_vertical_offset(xosd * osd, int voffset)
{
  FUNCTION_START(Dfunction);
  if (osd == NULL)
    return -1;

  _xosd_lock(osd);
  osd->voffset = voffset;
  update_pos(osd);
  _xosd_unlock(osd);

  return 0;
}
/* }}} */

/* xosd_set_horizontal_offset -- Change the number of pixels the display is offset from the position {{{ */
int
xosd_set_horizontal_offset(xosd * osd, int hoffset)
{
  FUNCTION_START(Dfunction);
  if (osd == NULL)
    return -1;

  _xosd_lock(osd);
  osd->hoffset = hoffset;
  update_pos(osd);
  _xosd_unlock(osd);

  return 0;
}
/* }}} */

/* xosd_set_pos -- Change the vertical position of the display {{{ */
int
xosd_set_pos(xosd * osd, xosd_pos pos)
{
  FUNCTION_START(Dfunction);
  if (osd == NULL)
    return -1;

  _xosd_lock(osd);
  osd->pos = pos;
  update_pos(osd);
  force_redraw(osd, -1);
  _xosd_unlock(osd);

  return 0;
}
/* }}} */

/* xosd_set_align -- Change the horizontal alignment of the display {{{ */
int
xosd_set_align(xosd * osd, xosd_align align)
{
  FUNCTION_START(Dfunction);
  if (osd == NULL)
    return -1;

  _xosd_lock(osd);
  osd->align = align;
  update_pos(osd);
  force_redraw(osd, -1);
  _xosd_unlock(osd);

  return 0;
}
/* }}} */

/* xosd_get_colour -- Gets the RGB value of the display's colour {{{ */
int
xosd_get_colour(xosd * osd, int *red, int *green, int *blue)
{
  FUNCTION_START(Dfunction);
  if (osd == NULL)
    return -1;

  if (red)
    *red = osd->colour.red;
  if (blue)
    *blue = osd->colour.blue;
  if (green)
    *green = osd->colour.green;

  return 0;
}
/* }}} */

/** Change automatic timeout. **/
static void
set_timeout(xosd * osd)
{                               /* Requires mutex lock. */
  FUNCTION_START(Dfunction);
  assert(osd);
  osd->timeout_time.tv_sec = (osd->timeout > 0)
    ? osd->timeout_time.tv_sec = time(NULL) + osd->timeout : 0;
  pthread_cond_signal(&osd->cond_time);
}

/* xosd_set_timeout -- Change the time before display is hidden. {{{ */
int
xosd_set_timeout(xosd * osd, int timeout)
{
  FUNCTION_START(Dfunction);
  if (osd == NULL)
    return -1;
  _xosd_lock(osd);
  osd->timeout = timeout;
  set_timeout(osd);
  _xosd_unlock(osd);
  return 0;
}
/* }}} */

/** Hide current lines. **/
static void
hide(xosd * osd)
{                               /* Requires mutex lock. */
  FUNCTION_START(Dfunction);
  assert(osd);
  osd->mapped = 0;
  XUnmapWindow(osd->display, osd->window);
  XFlush(osd->display);
  pthread_cond_broadcast(&osd->cond_hide);
}

/* xosd_hide -- hide the display {{{ */
int
xosd_hide(xosd * osd)
{
  FUNCTION_START(Dfunction);
  if (osd == NULL)
    return -1;
  if (osd->mapped) {
    _xosd_lock(osd);
    hide(osd);
    _xosd_unlock(osd);
    return 0;
  }
  return -1;
}
/* }}} */

/** Show current lines (again). **/
static void
show(xosd * osd)
{                               /* Requires mutex lock. */
  FUNCTION_START(Dfunction);
  assert(osd);
  osd->mapped = 1;
  XMapRaised(osd->display, osd->window);
  XFlush(osd->display);
}

/* xosd_show -- Show the display after being hidden {{{ */
int
xosd_show(xosd * osd)
{
  FUNCTION_START(Dfunction);
  if (osd == NULL)
    return -1;
  if (!osd->mapped) {
    _xosd_lock(osd);
    show(osd);
    _xosd_unlock(osd);
    return 0;
  }
  return -1;
}
/* }}} */

/* xosd_scroll -- Scroll the display up "lines" number of lines {{{ */
int
xosd_scroll(xosd * osd, int lines)
{
  int i;
  xosd_line *src, *dst;

  FUNCTION_START(Dfunction);
  if (osd == NULL)
    return -1;
  if (lines <= 0 || lines > osd->number_lines)
    return -1;

  _xosd_lock(osd);
  /* Clear old text */
  for (i=0, src=osd->lines; i < lines; i++,src++)
    if (src->text) {
      free(src->text);
      src->text = NULL;
    }
  /* Move following lines forward */
  for (dst=osd->lines; i < osd->number_lines; i++)
    *dst++ = *src++;
  /* Blank new lines */
  for (;dst < src; dst++) {
    dst->type = LINE_blank;
    dst->text = NULL;
  }
  force_redraw(osd, -1);
  _xosd_unlock(osd);
  return 0;
}
/* }}} */

/* xosd_get_number_lines -- Get the maximum number of lines allowed {{{ */
int
xosd_get_number_lines(xosd * osd)
{
  FUNCTION_START(Dfunction);
  if (osd == NULL)
    return -1;

  return osd->number_lines;
}
/* }}} */

/* vim: foldmethod=marker tabstop=2 shiftwidth=2 expandtab
 */
