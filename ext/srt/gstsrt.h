/* GStreamer
 * Copyright (C) 2017, Collabora Ltd.
 *   Author: Olivier Crete <olivier.crete@collabora.com>
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

#ifndef __GST_SRT_H__
#define __GST_SRT_H__

#include <gst/gst.h>
#include <gio/gio.h>
#include <gio/gnetworking.h>

#include <srt/srt.h>

#define SRT_URI_SCHEME "srt"
#define SRT_DEFAULT_LATENCY 125
#define SRT_DEFAULT_KEY_LENGTH 0
#define SRT_DEFAULT_MSG_SIZE 1316
#define SRT_DEFAULT_MAX_MSGS_PER_READ 10
#define SRT_DEFAULT_POLL_TIMEOUT 100
#define SRT_DEFAULT_MAX_CONNECT_RETRIES 0

G_BEGIN_DECLS

typedef enum
{
  GST_SRT_NO_CONNECTION = 0,
  GST_SRT_CALLER_CONNECTION = 1,
  GST_SRT_LISTENER_CONNECTION = 2,
  GST_SRT_RENDEZVOUS_CONNECTION = 3
} GstSRTConnectionMode;

typedef enum
{
  GST_SRT_NO_KEY = 0,
  GST_SRT_KEY_128_BITS = 16,
  GST_SRT_KEY_192_BITS = 24,
  GST_SRT_KEY_256_BITS = 32
} GstSRTKeyLength;

struct _GsrtSRTParams {
  GstSRTConnectionMode conn_mode;
  gboolean sender;
  gchar *local_address;
  guint16 local_port;
  gchar *remote_host;
  guint16 remote_port;
  gint latency;
  gchar *passphrase;
  GstSRTKeyLength key_length;
  gint connect_timeout;
  gint mss;
  gint srt_send_buf_sz;
  gint srt_recv_buf_sz;
  gint udp_send_buf_sz;
  gint udp_recv_buf_sz;
  gint too_late_pkt_drop;
  gboolean nak_report;
  gint64 input_rate;
  gint overhead_bw;
  gint64 max_bw;
  gint iptos;
  gint ipttl;
};

typedef struct _GsrtSRTParams GstSRTParams;

void gst_srt_default_params (GstSRTParams * params, gboolean sender);

gboolean gst_srt_init_params_from_uri(const GstElement * elem,
    GstSRTParams * params, const GstUri * uri);
gboolean gst_srt_validate_params (const GstElement * elem,
    const GstSRTParams * params);

void gst_srt_install_properties(GObjectClass * gobject_class, gint offset);
gboolean gst_srt_get_property(const GstSRTParams * params,
    const GObject * object, guint prop_id, GValue * value, gint offset);
gboolean gst_srt_set_property (GstSRTParams * params,
    const GObject * object, guint prop_id, const GValue * value, gint offset);
SRTSOCKET gst_srt_start_socket (const GstElement * elem,
    const GstSRTParams * params);

GstStructure *
gst_srt_get_stats (GSocketAddress * sockaddr, SRTSOCKET sock);

G_END_DECLS


#endif /* __GST_SRT_H__ */
