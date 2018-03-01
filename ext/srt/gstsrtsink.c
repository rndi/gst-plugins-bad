/* GStreamer SRT plugin based on libsrt
 * Copyright (C) 2017, Collabora Ltd.
 *   Author:Justin Kim <justin.kim@collabora.com>
 * Copyright (C) 2018, Haivision Systems Inc.
 *   Author: Roman Diouskine <rdiouskine@haivision.com>
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
 * SECTION:element-srtsink
 * @title: srtsink
 *
 * srtserversink is a network sink that sends <ulink url="http://www.srtalliance.org/">SRT</ulink>
 * packets to the network. Although SRT is an UDP-based protocol, srtsink works like
 * a server socket of connection-oriented protocol.
 *
 * <refsect2>
 * <title>Examples</title>
 * |[
 * gst-launch-1.0 -v audiotestsrc ! srtsink
 * ]| This pipeline shows how to serve SRT packets through the default port.
 * </refsect2>
 * 
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstsrtsink.h"
#include <srt/srt.h>
#include <gio/gio.h>

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY);

#define GST_CAT_DEFAULT gst_debug_srt_sink
GST_DEBUG_CATEGORY (GST_CAT_DEFAULT);

enum
{
  PROP_URI = 1,
  PROP_POLL_TIMEOUT,
  PROP_STATS,

  /*< private > */
  PROP_LAST
};

static GParamSpec *properties[PROP_LAST];

static void gst_srt_sink_uri_handler_init (gpointer g_iface,
    gpointer iface_data);
static gchar *gst_srt_sink_uri_get_uri (GstURIHandler * handler);
static gboolean gst_srt_sink_uri_set_uri (GstURIHandler * handler,
    const gchar * uri, GError ** error);


struct _GstSRTSinkPrivate
{
  gboolean cancelled;

  SRTSOCKET sock;
  gint poll_id;

  GMainLoop *loop;
  GMainContext *context;
  GSource *sink_source;
  GThread *thread;

  GList *clients;
};

#define GST_SRT_SINK_GET_PRIVATE(obj)  \
       (G_TYPE_INSTANCE_GET_PRIVATE ((obj), GST_TYPE_SRT_SINK, GstSRTSinkPrivate))

enum
{
  SIG_CLIENT_ADDED,
  SIG_CLIENT_REMOVED,

  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

#define gst_srt_sink_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE (GstSRTSink, gst_srt_sink,
    GST_TYPE_BASE_SINK, G_ADD_PRIVATE (GstSRTSink)
    G_IMPLEMENT_INTERFACE (GST_TYPE_URI_HANDLER, gst_srt_sink_uri_handler_init)
    GST_DEBUG_CATEGORY_INIT (GST_CAT_DEFAULT, "srtsink", 0, "SRT Sink"));

typedef struct
{
  int sock;
  GSocketAddress *sockaddr;
} SRTClient;

static SRTClient *
srt_client_new (void)
{
  SRTClient *client = g_new0 (SRTClient, 1);
  client->sock = SRT_INVALID_SOCK;
  return client;
}

static void
srt_client_free (SRTClient * client)
{
  g_return_if_fail (client != NULL);

  g_clear_object (&client->sockaddr);

  if (client->sock != SRT_INVALID_SOCK) {
    srt_close (client->sock);
  }

  g_free (client);
}

static void
srt_emit_client_removed (SRTClient * client, gpointer user_data)
{
  GstSRTSink *self = GST_SRT_SINK (user_data);
  g_return_if_fail (client != NULL && GST_IS_SRT_SINK (self));

  g_signal_emit (self, signals[SIG_CLIENT_REMOVED], 0, client->sock,
      client->sockaddr);
}

static void
gst_srt_sink_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec)
{
  GstSRTSink *self = GST_SRT_SINK (object);
  GstSRTSinkPrivate *priv = GST_SRT_SINK_GET_PRIVATE (self);

  switch (prop_id) {
    case PROP_URI:
      if (self->uri != NULL) {
        gchar *uri_str = gst_srt_sink_uri_get_uri (GST_URI_HANDLER (self));
        g_value_take_string (value, uri_str);
      }
      break;
    case PROP_POLL_TIMEOUT:
      g_value_set_int (value, self->poll_timeout);
      break;
    case PROP_STATS:
    {
      GList *item;

      GST_OBJECT_LOCK (self);
      for (item = priv->clients; item; item = item->next) {
        SRTClient *client = item->data;
        GValue tmp = G_VALUE_INIT;

        g_value_init (&tmp, GST_TYPE_STRUCTURE);
        g_value_take_boxed (&tmp, gst_srt_get_stats (client->sockaddr,
                client->sock));
        gst_value_array_append_and_take_value (value, &tmp);
      }
      GST_OBJECT_UNLOCK (self);
      break;
    }
    default:
      if (!gst_srt_get_property (&self->params, object, prop_id, value)) {
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      }
      break;
  }
}

static void
gst_srt_sink_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec)
{
  GstSRTSink *self = GST_SRT_SINK (object);

  switch (prop_id) {
    case PROP_URI:
      gst_srt_sink_uri_set_uri (GST_URI_HANDLER (self),
          g_value_get_string (value), NULL);
      break;
    case PROP_POLL_TIMEOUT:
      self->poll_timeout = g_value_get_int (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static gboolean
idle_listen_callback (gpointer data)
{
  GstSRTSink *self = GST_SRT_SINK (data);
  GstSRTSinkPrivate *priv = GST_SRT_SINK_GET_PRIVATE (self);
  gint pollid = priv->poll_id;
  SRTSOCKET sock, rsock;
  SRT_SOCKSTATUS status;

  if (priv->cancelled)
    return G_SOURCE_CONTINUE;

  sock = priv->sock;
  status = srt_getsockstate (sock);
  if (status == SRTS_BROKEN || status == SRTS_CLOSED || status == SRTS_NONEXIST) {
    if (sock != SRT_INVALID_SOCK) {
      srt_epoll_remove_usock (pollid, sock);
      srt_close (sock);
    }

    sock = gst_srt_start_socket (GST_ELEMENT_CAST (self), &self->params);
    if (sock == SRT_INVALID_SOCK) {
      GST_ELEMENT_ERROR (self, RESOURCE, FAILED,
          ("Creating SRT socket: %s", srt_getlasterror_str ()), (NULL));

      GST_OBJECT_LOCK (self);
      priv->sock = SRT_INVALID_SOCK;
      GST_OBJECT_UNLOCK (self);

      return G_SOURCE_REMOVE;
    }

    if (srt_epoll_add_usock (pollid, sock, &(int) {
            SRT_EPOLL_IN | SRT_EPOLL_ERR}) != 0) {
      GST_ELEMENT_ERROR (self, RESOURCE, FAILED,
          ("Adding SRT socket to poll set: %s",
              srt_getlasterror_str ()), (NULL));
      srt_close (sock);

      GST_OBJECT_LOCK (self);
      priv->sock = SRT_INVALID_SOCK;
      GST_OBJECT_UNLOCK (self);

      return G_SOURCE_REMOVE;
    }

    GST_OBJECT_LOCK (self);
    priv->sock = sock;
    GST_OBJECT_UNLOCK (self);
  }

  g_assert (sock != SRT_INVALID_SOCK);

  if (srt_epoll_wait (pollid, &rsock, &(int) {
          1}, 0, 0, self->poll_timeout, 0, 0, 0, 0) < 0) {
    // TODO: Maybe add a 'yield' here in case epoll starts thrashing?
    return G_SOURCE_CONTINUE;
  }

  g_assert (sock == rsock);

  status = srt_getsockstate (sock);
  if (status == SRTS_CONNECTED) {
    char *msg;

    GST_WARNING_OBJECT (self, "Incoming data on SRT sink?");

    // discard incoming data
    msg = g_alloca (1024);
    while (msg != NULL && srt_recvmsg (sock, msg, 1024) > 0)
      continue;
  } else if (status == SRTS_LISTENING) {
    struct sockaddr sa;
    gint sa_len;
    SRTClient *client;

    client = srt_client_new ();
    client->sock = srt_accept (priv->sock, &sa, &sa_len);

    if (client->sock == SRT_INVALID_SOCK) {
      GST_WARNING_OBJECT (self, "Failed to accept SRT client socket: %s",
          srt_getlasterror_str ());
      srt_client_free (client);

      return G_SOURCE_CONTINUE;
    }

    client->sockaddr = g_socket_address_new_from_native (&sa, sa_len);

    GST_OBJECT_LOCK (self);
    priv->clients = g_list_append (priv->clients, client);
    GST_OBJECT_UNLOCK (self);

    g_signal_emit (self, signals[SIG_CLIENT_ADDED], 0, client->sock,
        client->sockaddr);
    GST_DEBUG_OBJECT (self, "client added");
  }

  return G_SOURCE_CONTINUE;
}

static gpointer
thread_func (gpointer data)
{
  GstSRTSink *self = GST_SRT_SINK (data);
  GstSRTSinkPrivate *priv = GST_SRT_SINK_GET_PRIVATE (self);

  g_main_loop_run (priv->loop);

  return NULL;
}

static gboolean
gst_srt_sink_start (GstBaseSink * sink)
{
  GstSRTSink *self = GST_SRT_SINK (sink);
  GstSRTSinkPrivate *priv = GST_SRT_SINK_GET_PRIVATE (self);
  GError *error = NULL;

  g_assert (sink != NULL);

  if (self->uri != NULL) {
    if (!gst_srt_init_params_from_uri (GST_ELEMENT_CAST (sink),
            &self->params, self->uri))
      return FALSE;
  }

  if (!gst_srt_validate_params (GST_ELEMENT_CAST (sink), &self->params))
    return FALSE;

  priv->poll_id = srt_epoll_create ();
  if (priv->poll_id < 0) {
    GST_ELEMENT_ERROR (self, LIBRARY, INIT, (NULL),
        ("failed to create poll id for SRT socket (reason: %s)",
            srt_getlasterror_str ()));
    return FALSE;
  }

  priv->context = g_main_context_new ();
  priv->sink_source = g_idle_source_new ();
  g_source_set_callback (priv->sink_source,
      (GSourceFunc) idle_listen_callback, gst_object_ref (self),
      (GDestroyNotify) gst_object_unref);

  g_source_attach (priv->sink_source, priv->context);
  priv->loop = g_main_loop_new (priv->context, TRUE);

  priv->thread = g_thread_try_new ("srtsink", thread_func, self, &error);
  if (error != NULL) {
    GST_WARNING_OBJECT (self, "failed to create thread (reason: %s)",
        error->message);

    srt_epoll_release (priv->poll_id);
    priv->poll_id = SRT_ERROR;
    g_error_free (error);

    return FALSE;
  }

  return TRUE;
}

static GstFlowReturn
gst_srt_sink_render (GstBaseSink * sink, GstBuffer * buffer)
{
  GstSRTSink *self = GST_SRT_SINK (sink);
  GstSRTSinkPrivate *priv = GST_SRT_SINK_GET_PRIVATE (self);
  GList *clients;
  GstMapInfo info;

  if ((FALSE)) {
    GST_TRACE_OBJECT (self, "sending buffer %p, offset %"
        G_GINT64_FORMAT ", offset_end %" G_GINT64_FORMAT
        ", timestamp %" GST_TIME_FORMAT ", duration %" GST_TIME_FORMAT
        ", size %" G_GSIZE_FORMAT,
        buffer, GST_BUFFER_OFFSET (buffer),
        GST_BUFFER_OFFSET_END (buffer),
        GST_TIME_ARGS (GST_BUFFER_TIMESTAMP (buffer)),
        GST_TIME_ARGS (GST_BUFFER_DURATION (buffer)),
        gst_buffer_get_size (buffer));
  }

  if (!gst_buffer_map (buffer, &info, GST_MAP_READ)) {
    GST_ELEMENT_ERROR (self, RESOURCE, READ,
        ("Could not map the input stream"), (NULL));
    return GST_FLOW_ERROR;
  }

  GST_OBJECT_LOCK (sink);
  if (self->params.conn_mode == GST_SRT_LISTENER_CONNECTION) {
    clients = priv->clients;
    while (clients != NULL) {
      SRTClient *client = clients->data;
      clients = clients->next;

      if (srt_getsockstate (client->sock != SRTS_CONNECTED)) {
        GST_WARNING_OBJECT (self, "%s", srt_getlasterror_str ());

        priv->clients = g_list_remove (priv->clients, client);
        GST_OBJECT_UNLOCK (sink);
        g_signal_emit (self, signals[SIG_CLIENT_REMOVED], 0, client->sock,
            client->sockaddr);
        srt_client_free (client);
        GST_OBJECT_LOCK (sink);
        GST_DEBUG_OBJECT (self, "client removed");
      } else if (srt_sendmsg (client->sock, (const char *) info.data, info.size,
              -1, 1) <= 0) {
        GST_WARNING_OBJECT (self, "Send failed: %s", srt_getlasterror_str ());
        continue;
      }
    }
  } else if (srt_getsockstate (priv->sock == SRTS_CONNECTED)) {
    if (srt_sendmsg (priv->sock, (const char *) info.data, info.size,
            -1, 1) <= 0) {
      GST_WARNING_OBJECT (self, "Send failed: %s", srt_getlasterror_str ());
    }
  }
  GST_OBJECT_UNLOCK (sink);

  gst_buffer_unmap (buffer, &info);

  return TRUE;
}

static gboolean
gst_srt_sink_stop (GstBaseSink * sink)
{
  GstSRTSink *self = GST_SRT_SINK (sink);
  GstSRTSinkPrivate *priv = GST_SRT_SINK_GET_PRIVATE (self);
  GList *clients;

  // close main socket to abort the poll
  priv->cancelled = TRUE;
  srt_close (priv->sock);
  priv->sock = SRT_INVALID_SOCK;

  if (priv->loop) {
    g_main_loop_quit (priv->loop);
    g_thread_join (priv->thread);
    g_clear_pointer (&priv->loop, g_main_loop_unref);
    g_clear_pointer (&priv->thread, g_thread_unref);
  }

  if (priv->sink_source) {
    g_source_destroy (priv->sink_source);
    g_clear_pointer (&priv->sink_source, g_source_unref);
  }

  g_clear_pointer (&priv->context, g_main_context_unref);

  clients = priv->clients;
  priv->clients = NULL;

  g_list_foreach (clients, (GFunc) srt_emit_client_removed, self);
  g_list_free_full (clients, (GDestroyNotify) srt_client_free);

  srt_epoll_remove_usock (priv->poll_id, priv->sock);
  srt_epoll_release (priv->poll_id);
  priv->poll_id = -1;
  priv->cancelled = FALSE;

  return TRUE;
}

static gboolean
gst_srt_sink_unlock (GstBaseSink * sink)
{
  GstSRTSink *self = GST_SRT_SINK (sink);
  GstSRTSinkPrivate *priv = GST_SRT_SINK_GET_PRIVATE (self);

  priv->cancelled = TRUE;

  return TRUE;
}

static gboolean
gst_srt_sink_unlock_stop (GstBaseSink * sink)
{
  GstSRTSink *self = GST_SRT_SINK (sink);
  GstSRTSinkPrivate *priv = GST_SRT_SINK_GET_PRIVATE (self);

  priv->cancelled = FALSE;

  return TRUE;
}

static void
gst_srt_sink_class_init (GstSRTSinkClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *gstelement_class = GST_ELEMENT_CLASS (klass);
  GstBaseSinkClass *gstbasesink_class = GST_BASE_SINK_CLASS (klass);

  gobject_class->set_property = gst_srt_sink_set_property;
  gobject_class->get_property = gst_srt_sink_get_property;

  properties[PROP_URI] = g_param_spec_string ("uri", "URI",
      "URI in the form of srt://address:port?key1=val1&key2=val2", NULL,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  properties[PROP_POLL_TIMEOUT] =
      g_param_spec_int ("poll-timeout", "Poll Timeout",
      "Return poll wait after timeout miliseconds (-1 = infinite)", -1,
      G_MAXINT32, SRT_DEFAULT_POLL_TIMEOUT,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  properties[PROP_STATS] = gst_param_spec_array ("stats", "Statistics",
      "Array of GstStructures containing SRT statistics",
      g_param_spec_boxed ("stats", "Statistics",
          "Statistics for one client", GST_TYPE_STRUCTURE,
          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS),
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (gobject_class, PROP_LAST, properties);

  gst_srt_install_properties (gobject_class);

  /**
   * GstSRTSink::client-added:
   * @gstsrtserversink: the srtserversink element that emitted this signal
   * @sock: the client socket descriptor that was added to srtserversink
   * @addr: the pointer of "struct sockaddr" that describes the @sock
   * @addr_len: the length of @addr
   * 
   * The given socket descriptor was added to srtserversink.
   */
  signals[SIG_CLIENT_ADDED] =
      g_signal_new ("client-added", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET (GstSRTSinkClass, client_added),
      NULL, NULL, g_cclosure_marshal_generic, G_TYPE_NONE,
      2, G_TYPE_INT, G_TYPE_SOCKET_ADDRESS);

  /**
   * GstSRTSink::client-removed:
   * @gstsrtserversink: the srtserversink element that emitted this signal
   * @sock: the client socket descriptor that was added to srtserversink
   * @addr: the pointer of "struct sockaddr" that describes the @sock
   * @addr_len: the length of @addr
   *
   * The given socket descriptor was removed from srtserversink.
   */
  signals[SIG_CLIENT_REMOVED] =
      g_signal_new ("client-removed", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET (GstSRTSinkClass,
          client_removed), NULL, NULL, g_cclosure_marshal_generic, G_TYPE_NONE,
      2, G_TYPE_INT, G_TYPE_SOCKET_ADDRESS);

  gst_element_class_add_static_pad_template (gstelement_class, &sink_template);
  gst_element_class_set_metadata (gstelement_class,
      "SRT sink", "Sink/Network",
      "Send data over the network via SRT",
      "Justin Kim <justin.kim@collabora.com>");

  gstbasesink_class->start = GST_DEBUG_FUNCPTR (gst_srt_sink_start);
  gstbasesink_class->stop = GST_DEBUG_FUNCPTR (gst_srt_sink_stop);
  gstbasesink_class->unlock = GST_DEBUG_FUNCPTR (gst_srt_sink_unlock);
  gstbasesink_class->unlock_stop = GST_DEBUG_FUNCPTR (gst_srt_sink_unlock_stop);
  gstbasesink_class->render = GST_DEBUG_FUNCPTR (gst_srt_sink_render);
}

static void
gst_srt_sink_init (GstSRTSink * self)
{
  GstSRTSinkPrivate *priv = GST_SRT_SINK_GET_PRIVATE (self);

  memset ((gpointer) & self->params, 0, sizeof (self->params));
  gst_srt_default_params (&self->params, TRUE);

  self->poll_timeout = SRT_DEFAULT_POLL_TIMEOUT;
  priv->sock = SRT_INVALID_SOCK;
  priv->poll_id = -1;
}

static GstURIType
gst_srt_sink_uri_get_type (GType type)
{
  return GST_URI_SINK;
}

static const gchar *const *
gst_srt_sink_uri_get_protocols (GType type)
{
  static const gchar *protocols[] = { SRT_URI_SCHEME, NULL };

  return protocols;
}

static gchar *
gst_srt_sink_uri_get_uri (GstURIHandler * handler)
{
  gchar *uri_str;
  GstSRTSink *self = GST_SRT_SINK (handler);

  GST_OBJECT_LOCK (self);
  uri_str = gst_uri_to_string (self->uri);
  GST_OBJECT_UNLOCK (self);

  return uri_str;
}

static gboolean
gst_srt_sink_uri_set_uri (GstURIHandler * handler,
    const gchar * uri, GError ** error)
{
  GstSRTSink *self = GST_SRT_SINK (handler);
  gboolean ret = TRUE;
  GstUri *parsed_uri = gst_uri_from_string (uri);

  GST_TRACE_OBJECT (self, "Requested URI=%s", uri);

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
gst_srt_sink_uri_handler_init (gpointer g_iface, gpointer iface_data)
{
  GstURIHandlerInterface *iface = (GstURIHandlerInterface *) g_iface;

  iface->get_type = gst_srt_sink_uri_get_type;
  iface->get_protocols = gst_srt_sink_uri_get_protocols;
  iface->get_uri = gst_srt_sink_uri_get_uri;
  iface->set_uri = gst_srt_sink_uri_set_uri;
}
