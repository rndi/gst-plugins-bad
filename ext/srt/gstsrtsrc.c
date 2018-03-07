/* GStreamer SRT plugin based on libsrt
 * Copyright (C) 2017, Collabora Ltd.
 *   Author:Justin Kim <justin.kim@collabora.com>
 * Copyright (C) 2018, Haivision Systems Inc.
 *   Author: Roman Diouskine <rdiouskine@haivision.com>

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
 * SECTION:element-srtsrc
 * @title: srtsrc
 *
 * srtsrc is a network source that reads <ulink url="http://www.srtalliance.org/">SRT</ulink>
 * packets from the network. Although SRT is a protocol based on UDP, srtsrc works like
 * a server socket of connection-oriented protocol, but it accepts to only one client connection.
 *
 * <refsect2>
 * <title>Examples</title>
 * |[
 * gst-launch-1.0 -v srtsrc uri="srt://:7001" ! fakesink
 * ]| This pipeline shows how to bind SRT server by setting #GstSRTSrc:uri property. 
 * </refsect2>
 * 
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstsrtsrc.h"
#include <srt/srt.h>
#include <gio/gio.h>

static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY);

#define GST_CAT_DEFAULT gst_debug_srt_src
GST_DEBUG_CATEGORY (GST_CAT_DEFAULT);

struct _GstSRTSrcPrivate
{
  SRTSOCKET sock;
  gint poll_id;
  guint64 n_frames;
  gint n_reconnects;

  gboolean cancelled;
};

#define GST_SRT_SRC_GET_PRIVATE(obj)  \
       (G_TYPE_INSTANCE_GET_PRIVATE ((obj), GST_TYPE_SRT_SRC, GstSRTSrcPrivate))

enum
{
  PROP_URI = 1,
  PROP_CAPS,
  PROP_POLL_TIMEOUT,
  PROP_MAX_RECONNECTS,

  /*< private > */
  PROP_LAST
};

static GParamSpec *properties[PROP_LAST];

static void gst_srt_src_uri_handler_init (gpointer g_iface,
    gpointer iface_data);
static gchar *gst_srt_src_uri_get_uri (GstURIHandler * handler);
static gboolean gst_srt_src_uri_set_uri (GstURIHandler * handler,
    const gchar * uri, GError ** error);

#define gst_srt_src_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE (GstSRTSrc, gst_srt_src,
    GST_TYPE_PUSH_SRC, G_ADD_PRIVATE (GstSRTSrc)
    G_IMPLEMENT_INTERFACE (GST_TYPE_URI_HANDLER, gst_srt_src_uri_handler_init)
    GST_DEBUG_CATEGORY_INIT (GST_CAT_DEFAULT, "srtsrc", 0, "SRT Source"));

static void
gst_srt_src_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec)
{
  GstSRTSrc *self = GST_SRT_SRC (object);

  switch (prop_id) {
    case PROP_URI:
      if (self->uri != NULL) {
        gchar *uri_str = gst_srt_src_uri_get_uri (GST_URI_HANDLER (self));
        g_value_take_string (value, uri_str);
      }
      break;
    case PROP_CAPS:
      GST_OBJECT_LOCK (self);
      gst_value_set_caps (value, self->caps);
      GST_OBJECT_UNLOCK (self);
      break;
    case PROP_POLL_TIMEOUT:
      g_value_set_int (value, self->poll_timeout);
      break;
    case PROP_MAX_RECONNECTS:
      g_value_set_int (value, self->max_reconnects);
      break;
    default:
      if (!gst_srt_get_property (&self->params, object, prop_id, value)) {
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      }
      break;
  }
}

static void
gst_srt_src_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec)
{
  GstSRTSrc *self = GST_SRT_SRC (object);

  switch (prop_id) {
    case PROP_URI:
      gst_srt_src_uri_set_uri (GST_URI_HANDLER (self),
          g_value_get_string (value), NULL);
      break;
    case PROP_CAPS:
      GST_OBJECT_LOCK (self);
      g_clear_pointer (&self->caps, gst_caps_unref);
      self->caps = gst_caps_copy (gst_value_get_caps (value));
      GST_OBJECT_UNLOCK (self);
      break;
    case PROP_POLL_TIMEOUT:
      self->poll_timeout = g_value_get_int (value);
      break;
    case PROP_MAX_RECONNECTS:
      self->max_reconnects = g_value_get_int (value);
      break;
    default:
      if (!gst_srt_set_property (&self->params, object, prop_id, value)) {
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      }
      break;
  }
}

static void
gst_srt_src_finalize (GObject * object)
{
  GstSRTSrc *self = GST_SRT_SRC (object);

  g_clear_pointer (&self->uri, gst_uri_unref);
  g_clear_pointer (&self->caps, gst_caps_unref);
  gst_srt_default_params (&self->params, FALSE);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static GstCaps *
gst_srt_src_get_caps (GstBaseSrc * src, GstCaps * filter)
{
  GstSRTSrc *self = GST_SRT_SRC (src);
  GstCaps *result, *caps = NULL;

  GST_OBJECT_LOCK (self);
  if (self->caps != NULL) {
    caps = gst_caps_ref (self->caps);
  }
  GST_OBJECT_UNLOCK (self);

  if (caps) {
    if (filter) {
      result = gst_caps_intersect_full (filter, caps, GST_CAPS_INTERSECT_FIRST);
      gst_caps_unref (caps);
    } else {
      result = caps;
    }
  } else {
    result = (filter) ? gst_caps_ref (filter) : gst_caps_new_any ();
  }

  return result;
}

static gboolean
gst_srt_src_start (GstBaseSrc * src)
{
  GstSRTSrc *self = GST_SRT_SRC (src);
  GstSRTSrcPrivate *priv = GST_SRT_SRC_GET_PRIVATE (self);

  g_assert (src != NULL);

  if (self->uri != NULL) {
    if (!gst_srt_init_params_from_uri (GST_ELEMENT_CAST (src),
            &self->params, self->uri))
      return FALSE;
  }

  if (!gst_srt_validate_params (GST_ELEMENT_CAST (src), &self->params))
    return FALSE;

  if (srt_startup () != 0) {
    GST_ELEMENT_ERROR (self, LIBRARY, INIT, (NULL),
        ("failed to initialize SRT library (reason: %s)",
            srt_getlasterror_str ()));
    return FALSE;
  }

  if ((FALSE)) {
    srt_setloglevel (LOG_DEBUG);
  }

  priv->poll_id = srt_epoll_create ();
  if (priv->poll_id < 0) {
    GST_ELEMENT_ERROR (self, LIBRARY, INIT, (NULL),
        ("failed to create poll id for SRT socket (reason: %s)",
            srt_getlasterror_str ()));
    return FALSE;
  }

  priv->cancelled = FALSE;

  return TRUE;
}

static gboolean
gst_srt_src_stop (GstBaseSrc * src)
{
  GstSRTSrc *self = GST_SRT_SRC (src);
  GstSRTSrcPrivate *priv = GST_SRT_SRC_GET_PRIVATE (self);

  if (priv->poll_id >= 0) {
    srt_epoll_remove_usock (priv->poll_id, priv->sock);
    srt_epoll_release (priv->poll_id);
    priv->poll_id = SRT_ERROR;
  }

  if (priv->sock != SRT_INVALID_SOCK) {
    srt_close (priv->sock);
    priv->sock = SRT_INVALID_SOCK;
  }

  priv->cancelled = FALSE;

  srt_cleanup ();

  return TRUE;
}

static gboolean
gst_srt_src_unlock (GstBaseSrc * src)
{
  GstSRTSrc *self = GST_SRT_SRC (src);
  GstSRTSrcPrivate *priv = GST_SRT_SRC_GET_PRIVATE (self);

  priv->cancelled = TRUE;

  return TRUE;
}

static gboolean
gst_srt_src_unlock_stop (GstBaseSrc * src)
{
  GstSRTSrc *self = GST_SRT_SRC (src);
  GstSRTSrcPrivate *priv = GST_SRT_SRC_GET_PRIVATE (self);

  priv->cancelled = FALSE;

  return TRUE;
}

static GstFlowReturn
gst_srt_src_fill (GstPushSrc * src, GstBuffer * outbuf)
{
  GstSRTSrc *self = GST_SRT_SRC (src);
  GstSRTSrcPrivate *priv = GST_SRT_SRC_GET_PRIVATE (self);
  gint pollid = priv->poll_id;

  g_assert (pollid > 0);

  while (!priv->cancelled) {
    SRTSOCKET sock, rsock;
    SRT_SOCKSTATUS status;

    sock = priv->sock;
    status = srt_getsockstate (sock);
    if (status == SRTS_BROKEN || status == SRTS_CLOSED
        || status == SRTS_NONEXIST) {

      if (sock != SRT_INVALID_SOCK) {
        srt_epoll_remove_usock (pollid, sock);
        srt_close (sock);
        priv->sock = SRT_INVALID_SOCK;

        if (self->max_reconnects >= 0 &&
            ++(priv->n_reconnects) > self->max_reconnects) {
          GST_ELEMENT_ERROR (self, RESOURCE, FAILED,
              ("Exceeded maximum re-connection attempts (%d/%d)",
                  priv->n_reconnects, self->max_reconnects), (NULL));
          return GST_FLOW_EOS;
        }

        if (priv->n_frames != 0) {
          GST_LOG_OBJECT (self, "SRT source has disconnected");
        }
      }

      priv->n_frames = 0;

      sock = gst_srt_start_socket (GST_ELEMENT_CAST (self), &self->params);
      if (sock == SRT_INVALID_SOCK) {
        GST_ELEMENT_ERROR (self, RESOURCE, FAILED,
            ("Creating SRT socket: %s", srt_getlasterror_str ()), (NULL));

        return GST_FLOW_ERROR;
      }

      if (srt_epoll_add_usock (pollid, sock, &(int) {
              SRT_EPOLL_IN | SRT_EPOLL_ERR}) != 0) {
        GST_ELEMENT_ERROR (self, RESOURCE, FAILED,
            ("Adding SRT socket to poll set: %s",
                srt_getlasterror_str ()), (NULL));
        srt_close (sock);

        return GST_FLOW_ERROR;
      }
      priv->sock = sock;
    }

    g_assert (sock != SRT_INVALID_SOCK);

    if (srt_epoll_wait (pollid, &rsock, &(int) {
            1}, 0, 0, self->poll_timeout, 0, 0, 0, 0) < 0) {
      // TODO: Maybe add a 'yield' here in case epoll starts thrashing?
      continue;
    }

    g_assert (sock == rsock);

    status = srt_getsockstate (sock);
    if (status == SRTS_CONNECTED) {
      GstMapInfo info;
      gsize bufsize, recvlen;

      if (!gst_buffer_map (outbuf, &info, GST_MAP_WRITE)) {
        GST_ELEMENT_ERROR (self, RESOURCE, WRITE,
            ("Could not map the output stream"), (NULL));
        return GST_FLOW_ERROR;
      }

      bufsize = gst_buffer_get_size (outbuf);
      for (recvlen = 0; recvlen < bufsize;) {
        gint ret;

        ret = srt_recvmsg (sock, (char *) (info.data + recvlen),
            bufsize - recvlen);

        if (ret <= 0)
          break;

        recvlen += ret;
      }
      gst_buffer_unmap (outbuf, &info);

      if (recvlen == 0) {
        GST_WARNING_OBJECT (self,
            "Error receiving data on SRT socket: %s", srt_getlasterror_str ());
        continue;
      }

      gst_buffer_resize (outbuf, 0, recvlen);

      if (priv->n_frames == 0) {
        GST_BUFFER_FLAG_SET (outbuf, GST_BUFFER_FLAG_DISCONT);
        GST_LOG_OBJECT (self, "SRT source is connected");
      }

      if (++priv->n_frames == G_MAXUINT64)
        priv->n_frames = 1;

      // Reset re-connect counter since we only counting
      // consecutive failing attempts
      priv->n_reconnects = 0;

      return GST_FLOW_OK;
    } else if (status == SRTS_LISTENING) {
      SRTSOCKET nsock;
      struct sockaddr client_sa;
      size_t client_sa_len;

      nsock = srt_accept (sock, &client_sa, (int *) &client_sa_len);
      if (nsock == SRT_INVALID_SOCK) {
        GST_WARNING_OBJECT (self,
            "Error accepting client connection on SRT socket: %s",
            srt_getlasterror_str ());
        continue;
      }

      if (srt_epoll_add_usock (pollid, nsock, &(int) {
              SRT_EPOLL_IN | SRT_EPOLL_ERR}) != 0) {
        GST_WARNING_OBJECT (self,
            "Error adding SRT client socket to poll set: %s",
            srt_getlasterror_str ());
        srt_close (nsock);
        continue;
      }
      // One client at a time so stop listening and
      // continue with the new client
      srt_epoll_remove_usock (pollid, sock);
      srt_close (sock);
      priv->sock = nsock;

      priv->n_frames = 0;
      GST_LOG_OBJECT (self, "SRT listener connected");
    }
  }

  // cancelled
  return GST_FLOW_FLUSHING;
}

static void
gst_srt_src_class_init (GstSRTSrcClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *gstelement_class = GST_ELEMENT_CLASS (klass);
  GstBaseSrcClass *gstbasesrc_class = GST_BASE_SRC_CLASS (klass);
  GstPushSrcClass *gstpushsrc_class = GST_PUSH_SRC_CLASS (klass);

  gobject_class->set_property = gst_srt_src_set_property;
  gobject_class->get_property = gst_srt_src_get_property;
  gobject_class->finalize = gst_srt_src_finalize;

  /**
   * GstSRTSrc:uri:
   * 
   * The URI used by SRT Connection.
   */
  properties[PROP_URI] = g_param_spec_string ("uri", "URI",
      "URI in the form of srt://address:port?key1=val1&key2=val2", NULL,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  /**
   * GstSRTSrc:caps:
   *
   * The Caps used by the source pad.
   */
  properties[PROP_CAPS] =
      g_param_spec_boxed ("caps", "Caps", "The caps of the source pad",
      GST_TYPE_CAPS, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  properties[PROP_POLL_TIMEOUT] =
      g_param_spec_int ("poll-timeout", "Poll Timeout",
      "Return poll wait after timeout miliseconds (-1 = infinite)",
      -1, G_MAXINT32, SRT_DEFAULT_POLL_TIMEOUT,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  properties[PROP_MAX_RECONNECTS] =
      g_param_spec_int ("max-reconnects", "Max re-connection attempts",
      "Maximum consecutive re-connection attempts (-1 = infinite)",
      -1, G_MAXINT32, SRT_DEFAULT_MAX_RECONNECTS,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (gobject_class, PROP_LAST, properties);

  gst_srt_install_properties (gobject_class);

  gstbasesrc_class->get_caps = GST_DEBUG_FUNCPTR (gst_srt_src_get_caps);

  gstbasesrc_class->start = GST_DEBUG_FUNCPTR (gst_srt_src_start);
  gstbasesrc_class->stop = GST_DEBUG_FUNCPTR (gst_srt_src_stop);
  gstbasesrc_class->unlock = GST_DEBUG_FUNCPTR (gst_srt_src_unlock);
  gstbasesrc_class->unlock_stop = GST_DEBUG_FUNCPTR (gst_srt_src_unlock_stop);

  gstpushsrc_class->fill = GST_DEBUG_FUNCPTR (gst_srt_src_fill);

  gst_element_class_add_static_pad_template (gstelement_class, &src_template);
  gst_element_class_set_metadata (gstelement_class,
      "SRT source", "Source/Network",
      "Receive data over the network via SRT",
      "Justin Kim <justin.kim@collabora.com>\n"
      "Roman Diouskine <rdiouskine@haivision.com");
}

static void
gst_srt_src_init (GstSRTSrc * self)
{
  GstSRTSrcPrivate *priv = GST_SRT_SRC_GET_PRIVATE (self);

  memset ((gpointer) & self->params, 0, sizeof (self->params));
  gst_srt_default_params (&self->params, FALSE);

  self->poll_timeout = SRT_DEFAULT_POLL_TIMEOUT;
  self->max_reconnects = SRT_DEFAULT_MAX_RECONNECTS;
  priv->sock = SRT_INVALID_SOCK;
  priv->poll_id = -1;
  priv->n_frames = 0;

  /* configure basesrc to be a live source */
  gst_base_src_set_live (GST_BASE_SRC (self), TRUE);
  /* make basesrc output a segment in time */
  gst_base_src_set_format (GST_BASE_SRC (self), GST_FORMAT_TIME);
  /* make basesrc set timestamps on outgoing buffers based on the running_time
   * when they were captured */
  gst_base_src_set_do_timestamp (GST_BASE_SRC (self), TRUE);

  gst_base_src_set_blocksize (GST_BASE_SRC (self), 1316 * 10);
}

static GstURIType
gst_srt_src_uri_get_type (GType type)
{
  return GST_URI_SRC;
}

static const gchar *const *
gst_srt_src_uri_get_protocols (GType type)
{
  static const gchar *protocols[] = { SRT_URI_SCHEME, NULL };

  return protocols;
}

static gchar *
gst_srt_src_uri_get_uri (GstURIHandler * handler)
{
  gchar *uri_str;
  GstSRTSrc *self = GST_SRT_SRC (handler);

  GST_OBJECT_LOCK (self);
  uri_str = gst_uri_to_string (self->uri);
  GST_OBJECT_UNLOCK (self);

  return uri_str;
}

static gboolean
gst_srt_src_uri_set_uri (GstURIHandler * handler,
    const gchar * uri, GError ** error)
{
  GstSRTSrc *self = GST_SRT_SRC (handler);
  gboolean ret = TRUE;
  GstUri *parsed_uri = gst_uri_from_string (uri);

  if (g_strcmp0 (gst_uri_get_scheme (parsed_uri), SRT_URI_SCHEME) != 0) {
    g_set_error (error, GST_URI_ERROR, GST_URI_ERROR_BAD_URI,
        "Invalid SRT URI scheme");
    ret = FALSE;
    goto out;
  }

  GST_OBJECT_LOCK (self);

  g_clear_pointer (&self->uri, gst_uri_unref);
  self->uri = gst_uri_ref (parsed_uri);

  GST_OBJECT_UNLOCK (self);

out:
  g_clear_pointer (&parsed_uri, gst_uri_unref);
  return ret;
}

static void
gst_srt_src_uri_handler_init (gpointer g_iface, gpointer iface_data)
{
  GstURIHandlerInterface *iface = (GstURIHandlerInterface *) g_iface;

  iface->get_type = gst_srt_src_uri_get_type;
  iface->get_protocols = gst_srt_src_uri_get_protocols;
  iface->get_uri = gst_srt_src_uri_get_uri;
  iface->set_uri = gst_srt_src_uri_set_uri;
}
