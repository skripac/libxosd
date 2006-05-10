/* XOSD
 
 Copyright (c) 2000 Andre Renaud (andre@ignavus.net)

 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; either version 2 of the License, or
 (at your option) any later version.
 
 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

#include <assert.h>
#include <pthread.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/shape.h>
#include <X11/Xatom.h>

#define NLINES 2 /* The number of lines displayed on the screen */

#define MUTEX_GET()  pthread_mutex_lock (&osd->mutex)
#define MUTEX_RELEASE() pthread_mutex_unlock (&osd->mutex)

#include "xosd.h"

/* stores the current error string if applicable */
char *xosd_error = "";

typedef enum {LINE_blank, LINE_text, LINE_percentage, LINE_slider} line_type;

typedef struct
   {
   line_type type;
   
   char *text;
   int percentage;
   unsigned int pixel;
   } xosd_line;

struct xosd
   {
   pthread_t event_thread;
   pthread_t timeout_thread;
   
   pthread_mutex_t mutex;
   pthread_cond_t cond;
   
   Display *display;
   int screen;
   Window window;
   unsigned int depth;
   Pixmap bitmap;
   Visual *visual;
   
   XFontSet fontset;
   
   GC gc;
   GC bitmap_gc;
   GC bitmap_gc_back;
   
   int width;
   int height;
   int x;
   int y;
   xosd_pos pos;
   int offset;
   int shadow_offset;
      
   int mapped;
   int done;
   
   unsigned int pixel;   
   XColor colour;
   Colormap colourmap;

   xosd_line lines[NLINES];
   int timeout;
   int timeout_time;
   };

static void draw_bar (xosd *osd, Drawable d, GC gc, int x, int y, 
		      int width, int height, int horiz)
   {   
   if (horiz)
	 XFillRectangle(osd->display, d, gc, x, y, width * 0.7, height);
   else
	 XFillRectangle(osd->display, d, gc, x, y + height / 3,
			width * 0.8, height / 3);      
   }

static void draw_slider (xosd *osd, Drawable d, GC gc, int x, int y, 
			 int percent)
   {
   int barw, barh;
   int nbars, on, i;
   XFontSetExtents *extents = XExtentsOfFontSet(osd->fontset);

   y -= - (extents->max_logical_extent.y);

   barh = - (extents->max_logical_extent.y);
   barw = barh / 2;

   nbars = (osd->width * 0.8) / barw;
   on = (nbars - 1) * percent / 100;

   for (i = 0; i < nbars; x += barw, i++)
      draw_bar (osd, d, gc, x, y, barw, barh, i == on);   
   }

static void draw_percentage (xosd *osd, Drawable d, GC gc, int x, int y,
			     int percent)
   {
   int barw, barh;
   int nbars, on, i;
   XFontSetExtents *extents = XExtentsOfFontSet(osd->fontset);

   y -= - (extents->max_logical_extent.y);

   barh = - (extents->max_logical_extent.y);
   barw = barh / 2;

   nbars = (osd->width * 0.8) / barw;
   on = nbars * percent / 100;

   for (i = 0; i < nbars; x += barw, i++)
      draw_bar (osd, d, gc, x, y, barw, barh, i < on);
   }


static void expose (xosd *osd)
   {
   int line;
   int x, y;
   XFontSetExtents *extents;
   
   MUTEX_GET ();

   XFillRectangle (osd->display, osd->bitmap, osd->bitmap_gc_back,
		   0, 0, osd->width, osd->height);
   extents = XExtentsOfFontSet(osd->fontset);
   
   for (line = 0; line < NLINES; line ++)
      {
      x = 10;
      y = extents->max_logical_extent.height * (line + 1);

      switch (osd->lines[line].type)
	 {
	 case LINE_blank: break;
	 case LINE_text:
	    {
	    char *text;
	    int len;
	    
	    text = osd->lines[line].text;
	    if (!text)
	       break;	    
	    /* printf ("line: [%d] (%d, %d) %s\n", line, x, y, osd->lines[line]); */

	    len = strlen (text);
	    XmbDrawString (osd->display, osd->bitmap, osd->fontset,
			   osd->bitmap_gc, x, y,
			   text, len);

	    if (osd->shadow_offset)
	       {
	       XSetForeground (osd->display, osd->gc, 
			       BlackPixel(osd->display, osd->screen));

	       XmbDrawString (osd->display, osd->bitmap, osd->fontset,
			      osd->bitmap_gc, x + osd->shadow_offset, 
			      y + osd->shadow_offset,
			      text, len);
	       XmbDrawString (osd->display, osd->window, osd->fontset,
			      osd->gc, x + osd->shadow_offset, 
			      y + osd->shadow_offset,
			      text, len);
	       }

	    XSetForeground (osd->display, osd->gc, osd->lines[line].pixel);
	    
	    XmbDrawString (osd->display, osd->window, osd->fontset,
			   osd->gc, x, y,
			   text, len);
	    break;
	    }
	 
	 case LINE_percentage:
	    {
	    draw_percentage (osd, osd->bitmap, osd->bitmap_gc, x, y, 
			     osd->lines[line].percentage);
	    
	    if (osd->shadow_offset)
	       {
	       XSetForeground (osd->display, osd->gc, 
			       BlackPixel(osd->display, osd->screen));
	       draw_percentage (osd, osd->bitmap, osd->bitmap_gc, 
				x + osd->shadow_offset, y + osd->shadow_offset,
				osd->lines[line].percentage);
	       draw_percentage (osd, osd->window, osd->gc, 
				x + osd->shadow_offset, y + osd->shadow_offset,
				osd->lines[line].percentage);
	       }
	    
	    XSetForeground (osd->display, osd->gc, osd->lines[line].pixel);
	    draw_percentage (osd, osd->window, osd->gc, x, y,
			     osd->lines[line].percentage);
	    break;
	    }
	 
	 case LINE_slider:
	    {
	    draw_slider (osd, osd->bitmap, osd->bitmap_gc, x, y, 
			 osd->lines[line].percentage);
	    
	    if (osd->shadow_offset)
	       {
	       XSetForeground (osd->display, osd->gc, 
			       BlackPixel(osd->display, osd->screen));
	       draw_slider (osd, osd->bitmap, osd->bitmap_gc, 
			    x + osd->shadow_offset, y + osd->shadow_offset,
			    osd->lines[line].percentage);
	       draw_slider (osd, osd->window, osd->gc, 
			    x + osd->shadow_offset, y + osd->shadow_offset,
			    osd->lines[line].percentage);
	       }
	    
	    XSetForeground (osd->display, osd->gc, osd->lines[line].pixel);
	    draw_slider (osd, osd->window, osd->gc, x, y,
			 osd->lines[line].percentage);
	    break;
	    }
	 }
      }

   XShapeCombineMask (osd->display, osd->window,
		      ShapeBounding,
		      0, 0,
		      osd->bitmap,
		      ShapeSet);
   
   XFlush (osd->display);
   
   MUTEX_RELEASE ();
   }

static void *event_loop (void *osdv)
   {
   xosd *osd = osdv;
   XEvent report;   

   while (!osd->done)
      {
      
      /* XCheckIfEvent (osd->display, &report, */
      /* XNextEvent(osd->display, &report); */
      MUTEX_GET ();
      if (!XCheckWindowEvent (osd->display, osd->window, ExposureMask, &report))
	 {
	 MUTEX_RELEASE ();
	 usleep (500);
	 continue;
	 }
      MUTEX_RELEASE ();

      report.type &= 0x7f; /* remove the sent by server/manual send flag */

      switch (report.type)
	 {
	 case Expose :
	    {
	    if (report.xexpose.count != 0)
	       break;
	    expose (osd);
	    break;
	    }
	 
	 default:
	    printf ("%d\n", report.type);
	 }
      }   
   
   return NULL;
   }


static void *timeout_loop (void *osdv)
   {
   xosd *osd = osdv;

   if (osdv==NULL) {
     return NULL;
   }
   
   while (!osd->done)
      {
      usleep (1000);
      MUTEX_GET ();
      if (osd->timeout != -1 && 
	  osd->mapped && 
	  osd->timeout_time <= time(NULL))
	 {
	   
	 MUTEX_RELEASE ();
	 /* printf ("timeout_loop: hiding\n"); */
	 xosd_hide (osd);
	 }
      else
	 MUTEX_RELEASE ();
      }
   
   return NULL;
   }

static int display_string (xosd *osd, int line, char *string)
   {
   assert (osd);
   
   osd->lines[line].type = LINE_text;
   osd->lines[line].pixel = osd->pixel;
   
   if (string)
      {
      osd->lines[line].text =
	 realloc (osd->lines[line].text, strlen (string) + 1);
      strcpy (osd->lines[line].text, string);
      }
   else
      {
      osd->lines[line].text = realloc (osd->lines[line].text, 1);
      osd->lines[line].text[0] = '\0';
      }

   return 0;
   }

static int display_percentage (xosd *osd, int line, int percentage)
   {
   assert (osd);
   
   if (percentage < 0)
      percentage = 0;
   if (percentage > 100)
      percentage = 100;
   
   osd->lines[line].type = LINE_percentage;
   osd->lines[line].percentage = percentage;
   osd->lines[line].pixel = osd->pixel;
   
   return 0;
   }

static int display_slider (xosd *osd, int line, int percentage)
   {
   assert (osd);
   
   if (percentage < 0)
      percentage = 0;
   if (percentage > 100)
      percentage = 100;
   
   osd->lines[line].type = LINE_slider;
   osd->lines[line].percentage = percentage;
   osd->lines[line].pixel = osd->pixel;
   
   return 0;
   }

static int force_redraw (xosd *osd)
   {
   assert (osd);
   
   expose (osd);

   if (!osd->mapped)
      {
      MUTEX_GET ();
      XMapRaised (osd->display, osd->window);
      osd->mapped = 1;
      MUTEX_RELEASE ();
      }

   return 0;
   }

static int set_font (xosd *osd, char *font)
   {
   char **missing;
   int nmissing;
   char *defstr;
   XFontSetExtents *extents;

   assert (osd);
   XFontSet fontset;

   MUTEX_GET ();

   /* don't assign directly, so that we still have a good state
      if the new font is invalid */
   fontset = XCreateFontSet (osd->display, font,
                                  &missing, &nmissing, &defstr);
   if (fontset == NULL)
      {
        xosd_error = "Invalid font";
        MUTEX_RELEASE();
      return -1;
      }

   /* free an existing fontset if there was one previously */
   if (osd->fontset)
     XFreeFontSet(osd->display, osd->fontset);

   osd->fontset = fontset;
   extents = XExtentsOfFontSet(osd->fontset);
   
   osd->width = XDisplayWidth (osd->display, osd->screen);
   osd->height = extents->max_logical_extent.height * NLINES + 10;

   XResizeWindow (osd->display, osd->window, osd->width, osd->height);

   if (osd->bitmap)
   XFreePixmap (osd->display, osd->bitmap);

   osd->bitmap = XCreatePixmap (osd->display, osd->window,
				osd->width, osd->height,
				1);
   if (!osd->bitmap)
     {
       xosd_error = "Couldn't create pixmap";
       MUTEX_RELEASE ();
       return -1;
     }

   MUTEX_RELEASE ();
   
   return 0;
   }

static int set_colour (xosd *osd, char *colour)
   {
   assert (osd);

   MUTEX_GET ();
   
   osd->colourmap = DefaultColormap (osd->display, osd->screen);
   
   if (XParseColor (osd->display, osd->colourmap, colour, &osd->colour))
      {
      if (XAllocColor(osd->display, osd->colourmap, &osd->colour))
	 {
	 osd->pixel = osd->colour.pixel;
	 }
      else
	 {
	 osd->pixel = WhitePixel(osd->display, osd->screen);
	 }
      }
   else
      {
      osd->pixel = WhitePixel(osd->display, osd->screen);
      }      

   XSetForeground (osd->display, osd->gc, osd->pixel);
   XSetBackground (osd->display, osd->gc,
		   WhitePixel (osd->display, osd->screen));
   
   MUTEX_RELEASE ();
   
   return 0;
   }



static int set_timeout (xosd *osd, int timeout)
   {
   osd->timeout = timeout;
   osd->timeout_time = time (NULL) + timeout;
   return 0;
   }

xosd *xosd_init (char *font, char *colour, int timeout, xosd_pos pos, int offset,
		 int shadow_offset)
   {
   xosd *osd;
   int event_basep, error_basep, inputmask, i;
   char *display;
   XSetWindowAttributes setwinattr;
   long data;
   char **missing;
   int nmissing;
   char *defstr;
   XFontSetExtents *extents;
   Atom a;

   
   /* fprintf(stderr, "Hello!\n"); */
   display = getenv ("DISPLAY");
   if (!display)
      {
        xosd_error = "No display";
        return NULL;
      }
   
   osd = malloc (sizeof (xosd));
   
   pthread_mutex_init (&osd->mutex, NULL);
   pthread_cond_init (&osd->cond, NULL);
   
   osd->display = XOpenDisplay (display);
   osd->screen = XDefaultScreen (osd->display);
   
   if (!osd->display)
      {
        xosd_error = "No display";
      free(osd);
      return NULL;
      }
   
   if (!XShapeQueryExtension (osd->display, &event_basep, &error_basep))
      {
        xosd_error = "No shape extensions";
      free(osd);
      return NULL;
      }

   osd->visual = DefaultVisual (osd->display, osd->screen);
   osd->depth = DefaultDepth (osd->display, osd->screen);

   extents = XExtentsOfFontSet(osd->fontset);
   
   /* resized when we select a font */
   osd->width = 1;
   osd->height = 1;
   
   setwinattr.override_redirect = 1;
   osd->window = XCreateWindow (osd->display,
				   XRootWindow (osd->display, osd->screen),
				   0, 0,
				   osd->width, osd->height,
				   0,
				   osd->depth,
				   CopyFromParent,
				   osd->visual,
				   CWOverrideRedirect,
				   &setwinattr);

   /* make sure set_font doesn't try to free these */
   osd->fontset = (XFontSet)NULL;
   osd->bitmap = (Pixmap)NULL;
   /* it is the caller's responsibility to trap a failed font allocation and
      try fixed instead - this library should not make that decision */
   if (set_font(osd, font) == -1)
     {
       free(osd);
       return NULL;
     }

   XStoreName (osd->display, osd->window, "XOSD");
   osd->pos = pos;
   xosd_set_offset (osd, offset);
   
   osd->gc = XCreateGC (osd->display, osd->window, 0, NULL);
   osd->bitmap_gc = XCreateGC (osd->display, osd->bitmap, 0, NULL);
   osd->bitmap_gc_back = XCreateGC (osd->display, osd->bitmap, 
				       0, NULL);
   XSetForeground (osd->display, osd->bitmap_gc_back,
		   BlackPixel (osd->display, osd->screen));
   XSetBackground (osd->display, osd->bitmap_gc_back,
		   WhitePixel (osd->display, osd->screen));
   
   XSetForeground (osd->display, osd->bitmap_gc,
		   WhitePixel (osd->display, osd->screen));
   XSetBackground (osd->display, osd->bitmap_gc,
		   BlackPixel (osd->display, osd->screen));
   
   set_colour (osd, colour);
   set_timeout (osd, timeout);

   inputmask = ExposureMask ;
   XSelectInput (osd->display, osd->window, inputmask);

   data = 6;
   a = XInternAtom (osd->display, "_WIN_LAYER", True);
   if (a != None)
      {
      XChangeProperty (osd->display,
		       osd->window,
		       XInternAtom (osd->display, "_WIN_LAYER", True),
		       XA_CARDINAL, 
		       32, 
		       PropModeReplace, 
		       (unsigned char *)&data,
		       1);
      }
   
   osd->mapped = 0;
   osd->done = 0;
   osd->shadow_offset = shadow_offset;

   for (i = 0; i < NLINES; i++)
      {
      osd->lines[i].type = LINE_text;
      osd->lines[i].text = NULL;
      }
   
   pthread_create (&osd->event_thread, NULL, event_loop, osd);
   pthread_create (&osd->timeout_thread, NULL, timeout_loop, osd);
   
   return osd;
   }

int xosd_uninit (xosd *osd)
   {
   int i;
   
   assert (osd);

   MUTEX_GET ();   
   osd->done = 1;
   MUTEX_RELEASE ();

   pthread_join (osd->event_thread, NULL);
   pthread_join (osd->timeout_thread, NULL);

   XFreePixmap (osd->display, osd->bitmap);
   XDestroyWindow (osd->display, osd->window);

   for (i = 0; i < NLINES; i++)
      {
      if (osd->lines[i].text)
	 free (osd->lines[i].text);
      }
   
   pthread_cond_destroy (&osd->cond);
   pthread_mutex_destroy (&osd->mutex);
   
   free (osd);
   
   return 0;
   }

int xosd_display (xosd *osd, int line, xosd_command command, ...)
   {
   int len;
   va_list a;
   char *string;
   int percent;
   
   assert (line >= 0 && line < NLINES);
   assert (osd);
   
   osd->timeout_time = time(NULL) + osd->timeout;

   va_start (a, command);
   switch (command)
      {
      case XOSD_string :
	 {
	 string = va_arg (a, char *);
	 len = display_string (osd, line, string);
	 break;
	 }

      case XOSD_percentage :
	 {
	 percent = va_arg (a, int);

	 display_percentage (osd, line, percent);
	 
	 len = percent;
	 break;
	 }
      
      case XOSD_slider :
	 {
	 percent = va_arg (a, int);
	 
	 display_slider (osd, line, percent);
	 
	 len = percent;
	 break;
	 }
      
      default :
	 {
           xosd_error = "Unknown command";
	 len = -1;
	 }
      }
   va_end (a);

   force_redraw (osd);
   
   return len;
   }

int xosd_is_onscreen(xosd* osd) {
  fail_if_null_osd(osd);
  return osd->mapped;
}

int xosd_wait_until_no_display(xosd* osd) {
  fail_if_null_osd(osd);

  while (xosd_is_onscreen(osd)) {
    MUTEX_GET();
    pthread_cond_wait(&osd->cond, &osd->mutex);
    MUTEX_RELEASE();
  }

  return 0;
}


int xosd_set_colour (xosd *osd, char *colour)
   {
   set_colour (osd, colour);
   
   return 0;
   }


int xosd_set_font (xosd *osd, char *font)
   {
   set_font (osd, font);
   
   /* force_redraw (osd); */
   
   return 0;
   }

static void xosd_update_pos (xosd *osd)
   {
   osd->x = 0;
   if (osd->pos == XOSD_bottom)
      osd->y = XDisplayHeight (osd->display, osd->screen) - osd->height - osd->offset;
   else
      osd->y = osd->offset;

   XMoveWindow (osd->display, osd->window, osd->x, osd->y);
   }

int xosd_set_shadow_offset (xosd *osd, int shadow_offset)
   {
   assert (osd);
   
   osd->shadow_offset = shadow_offset;
   
   return 0;
   }

int xosd_set_offset (xosd *osd, int offset)
   {
   assert (osd);

   osd->offset = offset;   

   xosd_update_pos (osd);
   
   return 0;
   }

int xosd_set_pos (xosd *osd, xosd_pos pos)
   {
   assert (osd);
   
   osd->pos = pos;
   
   xosd_update_pos (osd);
   
   return 0;
   }

int xosd_get_colour (xosd *osd, int *red, int *green, int *blue)
   {
   assert (osd);
   
   if (red)
      *red = osd->colour.red;
   if (blue)
      *blue = osd->colour.blue;
   if (green)
      *green = osd->colour.green;

   return 0;
   }

int xosd_set_timeout (xosd *osd, int timeout)
   {
   set_timeout (osd, timeout);
   /* xosd_show (osd); */
   return 0;
   }


int xosd_hide (xosd *osd)
   {
   assert (osd);
   
   if (osd->mapped)
      {
      MUTEX_GET ();
      osd->mapped = 0;
      XUnmapWindow (osd->display, osd->window);
      XFlush (osd->display);
      pthread_cond_broadcast(&osd->cond);
      MUTEX_RELEASE ();
      return 0;
      } 
   else {
     return -1;
      }
   
   }

int xosd_show (xosd *osd)
   {
   assert (osd);
   
   if (!osd->mapped)
      {
      MUTEX_GET ();
      osd->mapped = 1;
      XMapRaised (osd->display, osd->window);
      XFlush (osd->display);
      MUTEX_RELEASE ();
      return 0;
      }
   else 
     {
       return -1;
     }
   
   }
