/*
 * GStreamer
 * Copyright (C) 2013 Julien Isorce <julien.isorce@collabora.co.uk>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#define GLIB_DISABLE_DEPRECATION_WARNINGS

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "../gstgl_fwd.h"
#include <gst/gl/gstglcontext.h>

#include "gstglwindow_dispmanx_egl.h"


#ifndef ELEMENT_CHANGE_LAYER
/* copied from interface/vmcs_host/vc_vchi_dispmanx.h of userland.git */
#define ELEMENT_CHANGE_LAYER          (1<<0)
#define ELEMENT_CHANGE_OPACITY        (1<<1)
#define ELEMENT_CHANGE_DEST_RECT      (1<<2)
#define ELEMENT_CHANGE_SRC_RECT       (1<<3)
#define ELEMENT_CHANGE_MASK_RESOURCE  (1<<4)
#define ELEMENT_CHANGE_TRANSFORM      (1<<5)
#endif

#define GST_CAT_DEFAULT gst_gl_window_debug

#define gst_gl_window_dispmanx_egl_parent_class parent_class
G_DEFINE_TYPE (GstGLWindowDispmanxEGL, gst_gl_window_dispmanx_egl,
    GST_GL_TYPE_WINDOW);

static guintptr gst_gl_window_dispmanx_egl_get_window_handle (GstGLWindow *
    window);
static void gst_gl_window_dispmanx_egl_set_window_handle (GstGLWindow * window,
    guintptr handle);
static void gst_gl_window_dispmanx_egl_set_preferred_size (GstGLWindow * window,
    gint width, gint height);
static void gst_gl_window_dispmanx_egl_show (GstGLWindow * window);
static void gst_gl_window_dispmanx_egl_close (GstGLWindow * window);
static gboolean gst_gl_window_dispmanx_egl_open (GstGLWindow * window,
    GError ** error);
static guintptr gst_gl_window_dispmanx_egl_get_display (GstGLWindow * window);


static void window_resize (GstGLWindowDispmanxEGL * window_egl, guint width,
    guint height, gboolean visible);

static void
gst_gl_window_dispmanx_egl_class_init (GstGLWindowDispmanxEGLClass * klass)
{
  GstGLWindowClass *window_class = (GstGLWindowClass *) klass;

  window_class->get_window_handle =
      GST_DEBUG_FUNCPTR (gst_gl_window_dispmanx_egl_get_window_handle);
  window_class->set_window_handle =
      GST_DEBUG_FUNCPTR (gst_gl_window_dispmanx_egl_set_window_handle);
  window_class->show = GST_DEBUG_FUNCPTR (gst_gl_window_dispmanx_egl_show);
  window_class->close = GST_DEBUG_FUNCPTR (gst_gl_window_dispmanx_egl_close);
  window_class->open = GST_DEBUG_FUNCPTR (gst_gl_window_dispmanx_egl_open);
  window_class->get_display =
      GST_DEBUG_FUNCPTR (gst_gl_window_dispmanx_egl_get_display);
  window_class->set_preferred_size =
      GST_DEBUG_FUNCPTR (gst_gl_window_dispmanx_egl_set_preferred_size);
}

static void
gst_gl_window_dispmanx_egl_init (GstGLWindowDispmanxEGL * window_egl)
{
  window_egl->egldisplay = EGL_DEFAULT_DISPLAY;

  window_egl->visible = FALSE;
  window_egl->display = 0;
  window_egl->dp_width = 0;
  window_egl->dp_height = 0;
  window_egl->native.element = 0;
  window_egl->native.width = 0;
  window_egl->native.height = 0;
}

/* Must be called in the gl thread */
GstGLWindowDispmanxEGL *
gst_gl_window_dispmanx_egl_new (GstGLDisplay * display)
{
  if ((gst_gl_display_get_handle_type (display) & GST_GL_DISPLAY_TYPE_EGL) == 0)
    /* we require an egl display to create dispmanx windows */
    return NULL;

  GST_DEBUG ("creating Dispmanx EGL window");

  return g_object_new (GST_GL_TYPE_WINDOW_DISPMANX_EGL, NULL);
}

static void
gst_gl_window_dispmanx_egl_close (GstGLWindow * window)
{
  GstGLWindowDispmanxEGL *window_egl;
  DISPMANX_UPDATE_HANDLE_T dispman_update;

  window_egl = GST_GL_WINDOW_DISPMANX_EGL (window);

  if (window_egl->native.element) {
    dispman_update = vc_dispmanx_update_start (0);
    vc_dispmanx_element_remove (dispman_update, window_egl->native.element);
    vc_dispmanx_update_submit_sync (dispman_update);
  }
  vc_dispmanx_display_close (window_egl->display);

  GST_GL_WINDOW_CLASS (parent_class)->close (window);
}

static gboolean
gst_gl_window_dispmanx_egl_open (GstGLWindow * window, GError ** error)
{
  GstGLWindowDispmanxEGL *window_egl = GST_GL_WINDOW_DISPMANX_EGL (window);
  gint ret = graphics_get_display_size (0, &window_egl->dp_width,
      &window_egl->dp_height);
  if (ret < 0) {
    g_set_error (error, GST_GL_WINDOW_ERROR,
        GST_GL_WINDOW_ERROR_RESOURCE_UNAVAILABLE, "Can't open display");
    return FALSE;
  }
  GST_DEBUG ("Got display size: %dx%d\n", window_egl->dp_width,
      window_egl->dp_height);

  window_egl->native.element = 0;

  return GST_GL_WINDOW_CLASS (parent_class)->open (window, error);
}

gboolean
gst_gl_window_dispmanx_egl_create_window (GstGLWindowDispmanxEGL * window_egl)
{
  window_egl->native.width = 0;
  window_egl->native.height = 0;
  window_egl->display = vc_dispmanx_display_open (0);
  window_resize (window_egl, 16, 16, FALSE);
  return TRUE;
}

static guintptr
gst_gl_window_dispmanx_egl_get_window_handle (GstGLWindow * window)
{
  GstGLWindowDispmanxEGL *window_egl;
  window_egl = GST_GL_WINDOW_DISPMANX_EGL (window);
  if (window_egl->native.element)
    return (guintptr) & window_egl->native;
  return 0;
}

static void
gst_gl_window_dispmanx_egl_set_window_handle (GstGLWindow * window,
    guintptr handle)
{
}

static void
gst_gl_window_dispmanx_egl_set_preferred_size (GstGLWindow * window, gint width,
    gint height)
{
  GstGLWindowDispmanxEGL *window_egl = GST_GL_WINDOW_DISPMANX_EGL (window);

  GST_DEBUG_OBJECT (window, "set preferred size to %dx%d", width, height);
  window_egl->preferred_width = width;
  window_egl->preferred_height = height;
}

static void
window_resize (GstGLWindowDispmanxEGL * window_egl, guint width, guint height,
    gboolean visible)
{
  GST_DEBUG ("resizing %s window from %ux%u to %ux%u",
      visible ? "visible" : "invisible", window_egl->native.width,
      window_egl->native.height, width, height);

  if (window_egl->display) {
    VC_RECT_T dst_rect;
    VC_RECT_T src_rect;
    GstVideoRectangle src, dst, res;
    DISPMANX_UPDATE_HANDLE_T dispman_update;
    uint32_t opacity = visible ? 255 : 0;
    VC_DISPMANX_ALPHA_T alpha =
        { DISPMANX_FLAGS_ALPHA_FIXED_ALL_PIXELS, opacity, 0 };

    /* Center width*height frame inside dp_width*dp_height */
    src.w = width;
    src.h = height;
    src.x = src.y = 0;
    dst.w = window_egl->dp_width;
    dst.h = window_egl->dp_height;
    dst.x = dst.y = 0;
    gst_video_sink_center_rect (src, dst, &res, FALSE);

    dst_rect.x = res.x;
    dst_rect.y = res.y;
    dst_rect.width = res.w;
    dst_rect.height = res.h;

    src_rect.x = 0;
    src_rect.y = 0;
    src_rect.width = width << 16;
    src_rect.height = height << 16;

    dispman_update = vc_dispmanx_update_start (0);

    if (window_egl->native.element) {
      uint32_t change_flags =
          ELEMENT_CHANGE_OPACITY | ELEMENT_CHANGE_DEST_RECT |
          ELEMENT_CHANGE_SRC_RECT;
      vc_dispmanx_element_change_attributes (dispman_update,
          window_egl->native.element, change_flags, 0, opacity, &dst_rect,
          &src_rect, 0, 0);
    } else {
      window_egl->native.element = vc_dispmanx_element_add (dispman_update,
          window_egl->display, 0, &dst_rect, 0, &src_rect,
          DISPMANX_PROTECTION_NONE, &alpha, 0, 0);
    }

    vc_dispmanx_update_submit_sync (dispman_update);

    if (GST_GL_WINDOW (window_egl)->resize)
      GST_GL_WINDOW (window_egl)->
          resize (GST_GL_WINDOW (window_egl)->resize_data, width, height);
  }

  window_egl->native.width = width;
  window_egl->native.height = height;
}

static void
gst_gl_window_dispmanx_egl_show (GstGLWindow * window)
{
  GstGLWindowDispmanxEGL *window_egl = GST_GL_WINDOW_DISPMANX_EGL (window);

  if (!window_egl->visible) {
    window_resize (window_egl, window_egl->preferred_width,
        window_egl->preferred_height, TRUE);
    window_egl->visible = TRUE;
  }
}

static guintptr
gst_gl_window_dispmanx_egl_get_display (GstGLWindow * window)
{
  GstGLWindowDispmanxEGL *window_egl;

  window_egl = GST_GL_WINDOW_DISPMANX_EGL (window);

  return (guintptr) window_egl->egldisplay;
}
