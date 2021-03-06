/*
 * This file is part of the Nice GLib ICE library.
 *
 * (C) 2006, 2007 Collabora Ltd.
 *  Contact: Dafydd Harries
 * (C) 2006, 2007 Nokia Corporation. All rights reserved.
 *  Contact: Kai Vehmanen
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is the Nice GLib ICE library.
 *
 * The Initial Developers of the Original Code are Collabora Ltd and Nokia
 * Corporation. All Rights Reserved.
 *
 * Contributors:
 *   Dafydd Harries, Collabora Ltd.
 *
 * Alternatively, the contents of this file may be used under the terms of the
 * the GNU Lesser General Public License Version 2.1 (the "LGPL"), in which
 * case the provisions of LGPL are applicable instead of those above. If you
 * wish to allow use of your version of this file only under the terms of the
 * LGPL and not to allow others to use your version of this file under the
 * MPL, indicate your decision by deleting the provisions above and replace
 * them with the notice and other provisions required by the LGPL. If you do
 * not delete the provisions above, a recipient may use your version of this
 * file under either the MPL or the LGPL.
 */
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <string.h>

#include "gstnicesrc.h"

GST_DEBUG_CATEGORY_STATIC (nicesrc_debug);
#define GST_CAT_DEFAULT nicesrc_debug


#define BUFFER_SIZE (65536)

static gboolean
gst_nice_src_start (
    GstBaseSrc *basesrc);

static gboolean
gst_nice_src_stop (
    GstBaseSrc *basesrc);

static GstFlowReturn
gst_nice_src_create (
  GstPushSrc *basesrc,
  GstBuffer **buffer);

static gboolean
gst_nice_src_unlock (
    GstBaseSrc *basesrc);

static void
gst_nice_src_set_property (
  GObject *object,
  guint prop_id,
  const GValue *value,
  GParamSpec *pspec);

static void
gst_nice_src_get_property (
  GObject *object,
  guint prop_id,
  GValue *value,
  GParamSpec *pspec);


static void
gst_nice_src_dispose (GObject *object);

static GstStateChangeReturn
gst_nice_src_change_state (
    GstElement * element,
    GstStateChange transition);

static GstStaticPadTemplate gst_nice_src_src_template =
GST_STATIC_PAD_TEMPLATE (
    "src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY);

G_DEFINE_TYPE (GstNiceSrc, gst_nice_src, GST_TYPE_PUSH_SRC);

enum
{
  PROP_AGENT = 1,
  PROP_STREAM,
  PROP_COMPONENT
};


static void
gst_nice_src_class_init (GstNiceSrcClass *klass)
{
  GstPushSrcClass *gstpushsrc_class;
  GstBaseSrcClass *gstbasesrc_class;
  GstElementClass *gstelement_class;
  GObjectClass *gobject_class;

  GST_DEBUG_CATEGORY_INIT (nicesrc_debug, "nicesrc",
      0, "libnice source");

  gstpushsrc_class = (GstPushSrcClass *) klass;
  gstpushsrc_class->create = GST_DEBUG_FUNCPTR (gst_nice_src_create);

  gstbasesrc_class = (GstBaseSrcClass *) klass;
  gstbasesrc_class->start = GST_DEBUG_FUNCPTR (gst_nice_src_start);
  gstbasesrc_class->stop = GST_DEBUG_FUNCPTR (gst_nice_src_stop);
  gstbasesrc_class->unlock = GST_DEBUG_FUNCPTR (gst_nice_src_unlock);

  gobject_class = (GObjectClass *) klass;
  gobject_class->set_property = gst_nice_src_set_property;
  gobject_class->get_property = gst_nice_src_get_property;
  gobject_class->dispose = gst_nice_src_dispose;

  gstelement_class = (GstElementClass *) klass;
  gstelement_class->change_state = gst_nice_src_change_state;

  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&gst_nice_src_src_template));
#if GST_CHECK_VERSION (1,0,0)
  gst_element_class_set_metadata (gstelement_class,
#else
  gst_element_class_set_details_simple (gstelement_class,
#endif
      "ICE source",
      "Source",
      "Interactive UDP connectivity establishment",
      "Dafydd Harries <dafydd.harries@collabora.co.uk>");

  g_object_class_install_property (gobject_class, PROP_AGENT,
      g_param_spec_object (
         "agent",
         "Agent",
         "The NiceAgent this source is bound to",
         NICE_TYPE_AGENT,
         G_PARAM_READWRITE));

  g_object_class_install_property (gobject_class, PROP_STREAM,
      g_param_spec_uint (
         "stream",
         "Stream ID",
         "The ID of the stream to read from",
         0,
         G_MAXUINT,
         0,
         G_PARAM_READWRITE));

  g_object_class_install_property (gobject_class, PROP_COMPONENT,
      g_param_spec_uint (
         "component",
         "Component ID",
         "The ID of the component to read from",
         0,
         G_MAXUINT,
         0,
         G_PARAM_READWRITE));
}

static void
gst_nice_src_init (GstNiceSrc *src)
{
  gst_base_src_set_live (GST_BASE_SRC (src), TRUE);
  gst_base_src_set_format (GST_BASE_SRC (src), GST_FORMAT_TIME);
  gst_base_src_set_do_timestamp (GST_BASE_SRC (src), TRUE);
  src->agent = NULL;
  src->stream_id = 0;
  src->component_id = 0;
  src->mainctx = g_main_context_new ();
  src->mainloop = g_main_loop_new (src->mainctx, FALSE);
  src->outbufs = g_queue_new ();
  src->agent_io_thread = NULL;
  g_cond_init (&src->outcond);
}

static gpointer
gst_nice_src_agent_io_thread (gpointer data)
{
  GstNiceSrc *nicesrc = GST_NICE_SRC (data);

  GST_INFO_OBJECT (nicesrc, "starting agent io thread");
  g_main_loop_run (nicesrc->mainloop);
  GST_INFO_OBJECT (nicesrc, "exiting agent io thread");

  return NULL;
}

static gboolean
main_loop_running_cb (gpointer data)
{
  GstNiceSrc *nicesrc = GST_NICE_SRC (data);

  GST_OBJECT_LOCK (nicesrc);
  /* _start() and _stop() could both be waiting for the mainloop to start so we
   * need to broadcast */
  g_cond_broadcast (&nicesrc->outcond);
  GST_OBJECT_UNLOCK (nicesrc);

  return FALSE;
}

static gboolean
gst_nice_src_start (GstBaseSrc * basesrc)
{
  GstNiceSrc *nicesrc = GST_NICE_SRC (basesrc);
  GSource *source;
  gchar *thread_name;

  GST_OBJECT_LOCK (nicesrc);
  source = g_idle_source_new ();
  g_source_set_callback (source,
      (GSourceFunc) main_loop_running_cb, nicesrc, NULL);
  g_source_attach (source, nicesrc->mainctx);
  g_source_unref (source);

  thread_name = g_strdup_printf ("%s:agent_io", GST_OBJECT_NAME (nicesrc));
  nicesrc->agent_io_thread = g_thread_new (thread_name, gst_nice_src_agent_io_thread, nicesrc);
  g_free (thread_name);
  /* wait until the agent thread starts spinning the mainloop or _stop() is
   * called */
  while (GST_BASE_SRC_IS_STARTING (basesrc) &&
      !g_main_loop_is_running (nicesrc->mainloop))
    g_cond_wait (&nicesrc->outcond, GST_OBJECT_GET_LOCK (nicesrc));
  GST_OBJECT_UNLOCK (nicesrc);

  return TRUE;
}

static gboolean
gst_nice_src_stop (GstBaseSrc * basesrc)
{
  GstNiceSrc *nicesrc = GST_NICE_SRC (basesrc);
  GThread *agent_io_thread = NULL;

  GST_OBJECT_LOCK (nicesrc);
  /* here we wait for the agent thread created in _start() to be scheduled so
   * that we don't risk calling _quit() first and then _run() on the mainloop */
  while (!g_main_loop_is_running (nicesrc->mainloop))
    g_cond_wait (&nicesrc->outcond, GST_OBJECT_GET_LOCK (nicesrc));
  g_main_loop_quit (nicesrc->mainloop);
  agent_io_thread = nicesrc->agent_io_thread;
  nicesrc->agent_io_thread = NULL;
  GST_OBJECT_UNLOCK (nicesrc);

  g_thread_join (agent_io_thread);

  return TRUE;
}

static void
gst_nice_src_read_callback (NiceAgent *agent,
    guint stream_id,
    guint component_id,
    guint len,
    gchar *buf,
    gpointer data)
{
  GstBaseSrc *basesrc = GST_BASE_SRC (data);
  GstNiceSrc *nicesrc = GST_NICE_SRC (basesrc);
  GstBuffer *buffer = NULL;

  GST_LOG_OBJECT (agent, "Got buffer, getting out of the main loop");

#if GST_CHECK_VERSION (1,0,0)
  buffer = gst_buffer_new_allocate (NULL, len, NULL);
  gst_buffer_fill (buffer, 0, buf, len);
#else
  buffer = gst_buffer_new_and_alloc (len);
  memcpy (GST_BUFFER_DATA (buffer), buf, len);
#endif
  GST_OBJECT_LOCK (nicesrc);
  g_queue_push_tail (nicesrc->outbufs, buffer);
  g_cond_signal (&nicesrc->outcond);
  GST_OBJECT_UNLOCK (nicesrc);
}

static gboolean
gst_nice_src_unlock (GstBaseSrc *src)
{
  GstNiceSrc *nicesrc = GST_NICE_SRC (src);

  GST_OBJECT_LOCK (src);
  g_cond_signal (&nicesrc->outcond);
  GST_OBJECT_UNLOCK (src);

  return TRUE;
}

static GstFlowReturn
gst_nice_src_create (
  GstPushSrc *basesrc,
  GstBuffer **buffer)
{
  GstNiceSrc *nicesrc = GST_NICE_SRC (basesrc);

  GST_LOG_OBJECT (nicesrc, "create called");

  GST_OBJECT_LOCK (basesrc);
  if (g_queue_is_empty (nicesrc->outbufs))
    g_cond_wait (&nicesrc->outcond, GST_OBJECT_GET_LOCK (nicesrc));

  *buffer = g_queue_pop_head (nicesrc->outbufs);
  GST_OBJECT_UNLOCK (basesrc);

  if (*buffer != NULL) {
    GST_LOG_OBJECT (nicesrc, "Got buffer, pushing");
    return GST_FLOW_OK;
  } else {
    GST_LOG_OBJECT (nicesrc, "Got interrupting, returning wrong-state");
#if GST_CHECK_VERSION (1,0,0)
    return GST_FLOW_FLUSHING;
#else
    return GST_FLOW_WRONG_STATE;
#endif
  }

}

static void
gst_nice_src_dispose (GObject *object)
{
  GstNiceSrc *src = GST_NICE_SRC (object);

  if (src->agent)
    g_object_unref (src->agent);
  src->agent = NULL;

  if (src->mainloop)
    g_main_loop_unref (src->mainloop);
  src->mainloop = NULL;

  if (src->mainctx)
    g_main_context_unref (src->mainctx);
  src->mainctx = NULL;

  if (src->outbufs) {
    g_queue_foreach (src->outbufs, (GFunc) gst_buffer_unref, NULL);
    g_queue_free (src->outbufs);
  }
  src->outbufs = NULL;
  g_cond_clear (&src->outcond);

  G_OBJECT_CLASS (gst_nice_src_parent_class)->dispose (object);
}

static void
gst_nice_src_set_property (
  GObject *object,
  guint prop_id,
  const GValue *value,
  GParamSpec *pspec)
{
  GstNiceSrc *src = GST_NICE_SRC (object);

  switch (prop_id)
    {
    case PROP_AGENT:
      if (src->agent)
        GST_ERROR_OBJECT (object,
            "Changing the agent on a nice src not allowed");
      else
        src->agent = g_value_dup_object (value);
      break;

    case PROP_STREAM:
      src->stream_id = g_value_get_uint (value);
      break;

    case PROP_COMPONENT:
      src->component_id = g_value_get_uint (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
gst_nice_src_get_property (
  GObject *object,
  guint prop_id,
  GValue *value,
  GParamSpec *pspec)
{
  GstNiceSrc *src = GST_NICE_SRC (object);

  switch (prop_id)
    {
    case PROP_AGENT:
      g_value_set_object (value, src->agent);
      break;

    case PROP_STREAM:
      g_value_set_uint (value, src->stream_id);
      break;

    case PROP_COMPONENT:
      g_value_set_uint (value, src->component_id);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static GstStateChangeReturn
gst_nice_src_change_state (GstElement * element, GstStateChange transition)
{
  GstNiceSrc *src;
  GstStateChangeReturn ret;

  src = GST_NICE_SRC (element);

  switch (transition) {
    case GST_STATE_CHANGE_NULL_TO_READY:
      if (src->agent == NULL)
        {
          GST_ERROR_OBJECT (element,
              "Trying to start Nice source without an agent set");
          return GST_STATE_CHANGE_FAILURE;
        }
      else if (src->stream_id == 0)
          {
            GST_ERROR_OBJECT (element,
                "Trying to start Nice source without a stream set");
            return GST_STATE_CHANGE_FAILURE;
          }
      else if (src->component_id == 0)
          {
            GST_ERROR_OBJECT (element,
                "Trying to start Nice source without a component set");
            return GST_STATE_CHANGE_FAILURE;
          }
      break;
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      nice_agent_attach_recv (src->agent, src->stream_id, src->component_id,
          src->mainctx, NULL, NULL);
      GST_OBJECT_LOCK (src);
      g_queue_foreach (src->outbufs, (GFunc) gst_buffer_unref, NULL);
      g_queue_clear (src->outbufs);
      GST_OBJECT_UNLOCK (src);
      break;
    case GST_STATE_CHANGE_READY_TO_PAUSED:
    case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
    case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
    case GST_STATE_CHANGE_READY_TO_NULL:
    default:
      break;
  }

  ret = GST_ELEMENT_CLASS (gst_nice_src_parent_class)->change_state (element,
      transition);

  switch (transition) {
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      nice_agent_attach_recv (src->agent, src->stream_id, src->component_id,
          src->mainctx, gst_nice_src_read_callback, (gpointer) src);
      break;
    case GST_STATE_CHANGE_NULL_TO_READY:
    case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
    case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
    case GST_STATE_CHANGE_PAUSED_TO_READY:
    case GST_STATE_CHANGE_READY_TO_NULL:
    default:
      break;
  }

  return ret;
}


