/*
 * GStreamer
 * Copyright (C) 2015 Matthew Waters <matthew@centricular.com>
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

/**
 * SECTION:gstgtkglsink
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstgtkglsink.h"
#include "gtkgstglwidget.h"

GST_DEBUG_CATEGORY (gst_debug_gtk_gl_sink);
#define GST_CAT_DEFAULT gst_debug_gtk_gl_sink

static gboolean gst_gtk_gl_sink_start (GstBaseSink * bsink);
static gboolean gst_gtk_gl_sink_stop (GstBaseSink * bsink);
static gboolean gst_gtk_gl_sink_query (GstBaseSink * bsink, GstQuery * query);
static gboolean gst_gtk_gl_sink_propose_allocation (GstBaseSink * bsink,
    GstQuery * query);

static GstStaticPadTemplate gst_gtk_gl_sink_template =
GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE_WITH_FEATURES
        (GST_CAPS_FEATURE_MEMORY_GL_MEMORY, "RGBA")));

#define gst_gtk_gl_sink_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE (GstGtkGLSink, gst_gtk_gl_sink,
    GST_TYPE_GTK_BASE_SINK, GST_DEBUG_CATEGORY_INIT (gst_debug_gtk_gl_sink,
        "gtkglsink", 0, "Gtk Video Sink"));

static void
gst_gtk_gl_sink_class_init (GstGtkGLSinkClass * klass)
{
  GstElementClass *gstelement_class;
  GstBaseSinkClass *gstbasesink_class;
  GstGtkBaseSinkClass *gstgtkbasesink_class;

  gstelement_class = (GstElementClass *) klass;
  gstbasesink_class = (GstBaseSinkClass *) klass;
  gstgtkbasesink_class = (GstGtkBaseSinkClass *) klass;

  gstbasesink_class->query = gst_gtk_gl_sink_query;
  gstbasesink_class->propose_allocation = gst_gtk_gl_sink_propose_allocation;
  gstbasesink_class->start = gst_gtk_gl_sink_start;
  gstbasesink_class->stop = gst_gtk_gl_sink_stop;

  gstgtkbasesink_class->create_widget = gtk_gst_gl_widget_new;
  gstgtkbasesink_class->window_title = "Gtk+ GL renderer";

  gst_element_class_set_metadata (gstelement_class, "Gtk Video Sink",
      "Sink/Video", "A video sink that renders to a GtkWidget",
      "Matthew Waters <matthew@centricular.com>");

  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&gst_gtk_gl_sink_template));
}

static void
gst_gtk_gl_sink_init (GstGtkGLSink * gtk_sink)
{
}

static gboolean
gst_gtk_gl_sink_query (GstBaseSink * bsink, GstQuery * query)
{
  GstGtkGLSink *gtk_sink = GST_GTK_GL_SINK (bsink);
  gboolean res = FALSE;

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_CONTEXT:
    {
      const gchar *context_type;
      GstContext *context, *old_context;

      res = gst_gl_handle_context_query ((GstElement *) gtk_sink, query,
          &gtk_sink->display, &gtk_sink->gtk_context);

      if (gtk_sink->display)
        gst_gl_display_filter_gl_api (gtk_sink->display, GST_GL_API_OPENGL3);

      gst_query_parse_context_type (query, &context_type);

      if (g_strcmp0 (context_type, "gst.gl.local_context") == 0) {
        GstStructure *s;

        gst_query_parse_context (query, &old_context);

        if (old_context)
          context = gst_context_copy (old_context);
        else
          context = gst_context_new ("gst.gl.local_context", FALSE);

        s = gst_context_writable_structure (context);
        gst_structure_set (s, "context", GST_GL_TYPE_CONTEXT, gtk_sink->context,
            NULL);
        gst_query_set_context (query, context);
        gst_context_unref (context);

        res = gtk_sink->context != NULL;
      }
      GST_LOG_OBJECT (gtk_sink, "context query of type %s %i", context_type,
          res);
      break;
    }
    default:
      res = GST_BASE_SINK_CLASS (parent_class)->query (bsink, query);
      break;
  }

  return res;
}

static gboolean
gst_gtk_gl_sink_start (GstBaseSink * bsink)
{
  GstGtkBaseSink *base_sink = GST_GTK_BASE_SINK (bsink);
  GstGtkGLSink *gtk_sink = GST_GTK_GL_SINK (bsink);
  GtkGstGLWidget *gst_widget;

  if (!GST_BASE_SINK_CLASS (parent_class)->start (bsink))
    return FALSE;

  /* After this point, gtk_sink->widget will always be set */
  gst_widget = GTK_GST_GL_WIDGET (base_sink->widget);

  if (!gtk_gst_gl_widget_init_winsys (gst_widget))
    return FALSE;

  gtk_sink->display = gtk_gst_gl_widget_get_display (gst_widget);
  gtk_sink->context = gtk_gst_gl_widget_get_context (gst_widget);
  gtk_sink->gtk_context = gtk_gst_gl_widget_get_gtk_context (gst_widget);

  if (!gtk_sink->display || !gtk_sink->context || !gtk_sink->gtk_context)
    return FALSE;

  return TRUE;
}

static gboolean
gst_gtk_gl_sink_stop (GstBaseSink * bsink)
{
  GstGtkGLSink *gtk_sink = GST_GTK_GL_SINK (bsink);

  if (gtk_sink->display) {
    gst_object_unref (gtk_sink->display);
    gtk_sink->display = NULL;
  }

  if (gtk_sink->context) {
    gst_object_unref (gtk_sink->context);
    gtk_sink->context = NULL;
  }

  if (gtk_sink->gtk_context) {
    gst_object_unref (gtk_sink->gtk_context);
    gtk_sink->gtk_context = NULL;
  }

  return TRUE;
}

static gboolean
gst_gtk_gl_sink_propose_allocation (GstBaseSink * bsink, GstQuery * query)
{
  GstGtkGLSink *gtk_sink = GST_GTK_GL_SINK (bsink);
  GstBufferPool *pool = NULL;
  GstStructure *config;
  GstCaps *caps;
  guint size;
  gboolean need_pool;

  if (!gtk_sink->display || !gtk_sink->context)
    return FALSE;

  gst_query_parse_allocation (query, &caps, &need_pool);

  if (caps == NULL)
    goto no_caps;

  if (need_pool) {
    GstVideoInfo info;

    if (!gst_video_info_from_caps (&info, caps))
      goto invalid_caps;

    GST_DEBUG_OBJECT (gtk_sink, "create new pool");
    pool = gst_gl_buffer_pool_new (gtk_sink->context);

    /* the normal size of a frame */
    size = info.size;

    config = gst_buffer_pool_get_config (pool);
    gst_buffer_pool_config_set_params (config, caps, size, 0, 0);
    if (!gst_buffer_pool_set_config (pool, config))
      goto config_failed;

    /* we need at least 2 buffer because we hold on to the last one */
    gst_query_add_allocation_pool (query, pool, size, 2, 0);
    gst_object_unref (pool);
  }

  /* we also support various metadata */
  gst_query_add_allocation_meta (query, GST_VIDEO_META_API_TYPE, 0);

  if (gtk_sink->context->gl_vtable->FenceSync)
    gst_query_add_allocation_meta (query, GST_GL_SYNC_META_API_TYPE, 0);

  return TRUE;

  /* ERRORS */
no_caps:
  {
    GST_DEBUG_OBJECT (bsink, "no caps specified");
    return FALSE;
  }
invalid_caps:
  {
    GST_DEBUG_OBJECT (bsink, "invalid caps specified");
    return FALSE;
  }
config_failed:
  {
    GST_DEBUG_OBJECT (bsink, "failed setting config");
    return FALSE;
  }
}
