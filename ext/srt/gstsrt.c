/* GStreamer
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstsrt.h"
#include "gstsrtsrc.h"
#include "gstsrtsink.h"

#define GST_CAT_DEFAULT gst_debug_srt
GST_DEBUG_CATEGORY (GST_CAT_DEFAULT);

#if !GLIB_CHECK_VERSION(2, 54, 0)
/* gboolean g_ascii_string_to_signed() and g_ascii_string_to_unsigned()
 * have been borrowed from glib 2.54 as-is minus the formatting
 */
static gboolean
str_has_sign (const gchar * str)
{
  return str[0] == '-' || str[0] == '+';
}

static gboolean
str_has_hex_prefix (const gchar * str)
{
  return str[0] == '0' && g_ascii_tolower (str[1]) == 'x';
}

typedef enum
{
  G_NUMBER_PARSER_ERROR_INVALID,
  G_NUMBER_PARSER_ERROR_OUT_OF_BOUNDS,
} GNumberParserError;

#define G_NUMBER_PARSER_ERROR (g_number_parser_error_quark ())
static GQuark
g_number_parser_error_quark (void)
{
  return g_quark_from_static_string ("number-parser-error-quark");
}

static gboolean
g_ascii_string_to_signed (const gchar * str, guint base,
    gint64 min, gint64 max, gint64 * out_num, GError ** error)
{
  gint64 number;
  const gchar *end_ptr = NULL;
  gint saved_errno = 0;

  g_return_val_if_fail (str != NULL, FALSE);
  g_return_val_if_fail (base >= 2 && base <= 36, FALSE);
  g_return_val_if_fail (min <= max, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (str[0] == '\0') {
    g_set_error_literal (error,
        G_NUMBER_PARSER_ERROR, G_NUMBER_PARSER_ERROR_INVALID,
        "Empty string is not a number");
    return FALSE;
  }

  errno = 0;
  number = g_ascii_strtoll (str, (gchar **) & end_ptr, base);
  saved_errno = errno;

  /* We do not allow leading whitespace, but g_ascii_strtoll
   * accepts it and just skips it, so we need to check for it
   * ourselves.
   *
   * We don't support hexadecimal numbers prefixed with 0x or
   * 0X.
   */
  if (g_ascii_isspace (str[0]) ||
      (base == 16 &&
          (str_has_sign (str) ? str_has_hex_prefix (str +
                  1) : str_has_hex_prefix (str))) || (saved_errno != 0
          && saved_errno != ERANGE) || end_ptr == NULL || *end_ptr != '\0') {
    g_set_error (error, G_NUMBER_PARSER_ERROR, G_NUMBER_PARSER_ERROR_INVALID,
        "Not a signed number");
    return FALSE;
  }
  if (saved_errno == ERANGE || number < min || number > max) {
    gchar *min_str = g_strdup_printf ("%" G_GINT64_FORMAT, min);
    gchar *max_str = g_strdup_printf ("%" G_GINT64_FORMAT, max);

    g_set_error (error,
        G_NUMBER_PARSER_ERROR, G_NUMBER_PARSER_ERROR_OUT_OF_BOUNDS,
        "Out of bounds");
    g_free (min_str);
    g_free (max_str);
    return FALSE;
  }
  if (out_num != NULL)
    *out_num = number;
  return TRUE;
}

static gboolean
g_ascii_string_to_unsigned (const gchar * str, guint base,
    guint64 min, guint64 max, guint64 * out_num, GError ** error)
{
  guint64 number;
  const gchar *end_ptr = NULL;
  gint saved_errno = 0;

  g_return_val_if_fail (str != NULL, FALSE);
  g_return_val_if_fail (base >= 2 && base <= 36, FALSE);
  g_return_val_if_fail (min <= max, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (str[0] == '\0') {
    g_set_error_literal (error,
        G_NUMBER_PARSER_ERROR, G_NUMBER_PARSER_ERROR_INVALID,
        "Empty string is not a number");
    return FALSE;
  }

  errno = 0;
  number = g_ascii_strtoull (str, (gchar **) & end_ptr, base);
  saved_errno = errno;

  /* We do not allow leading whitespace, but g_ascii_strtoull
   * accepts it and just skips it, so we need to check for it
   * ourselves.
   *
   * Unsigned number should have no sign.
   *
   * We don't support hexadecimal numbers prefixed with 0x or
   * 0X.
   */
  if (g_ascii_isspace (str[0]) ||
      str_has_sign (str) ||
      (base == 16 && str_has_hex_prefix (str)) ||
      (saved_errno != 0 && saved_errno != ERANGE) ||
      end_ptr == NULL || *end_ptr != '\0') {
    g_set_error (error,
        G_NUMBER_PARSER_ERROR, G_NUMBER_PARSER_ERROR_INVALID,
        "Not an unsigned number");
    return FALSE;
  }
  if (saved_errno == ERANGE || number < min || number > max) {
    gchar *min_str = g_strdup_printf ("%" G_GUINT64_FORMAT, min);
    gchar *max_str = g_strdup_printf ("%" G_GUINT64_FORMAT, max);

    g_set_error (error,
        G_NUMBER_PARSER_ERROR, G_NUMBER_PARSER_ERROR_OUT_OF_BOUNDS,
        "Number is out of bounds");
    g_free (min_str);
    g_free (max_str);
    return FALSE;
  }
  if (out_num != NULL)
    *out_num = number;
  return TRUE;
}
#endif

// will be shifted with offset
enum
{
  PROP_MODE = 1,
  PROP_LATENCY,
  PROP_PASSPHRASE,
  PROP_KEY_LENGTH,
  PROP_MSS,
  PROP_SRT_SEND_BUF_SZ,
  PROP_SRT_RECV_BUF_SZ,
  PROP_UDP_SEND_BUF_SZ,
  PROP_UDP_RECV_BUF_SZ,
  PROP_TOO_LATE_PKT_DROP,
  PROP_INPUT_RATE,
  PROP_OVERHEAD_BW,
  PROP_MAXBW_BW,
  PROP_IPTOS,
  PROP_IPTTL,
  PROP_REMOTE_HOST,
  PROP_REMOTE_PORT,
  PROP_LOCAL_ADDRESS,
  PROP_LOCAL_PORT,

  /*< private > */
  PROP_LAST
};

#define GST_TYPE_SRT_CONNECTION_MODE (gst_srt_connection_mode_get_type ())
static GType
gst_srt_connection_mode_get_type (void)
{
  static GType srt_connection_mode_type = 0;

  if (!srt_connection_mode_type) {
    static GEnumValue pattern_types[] = {
      {GST_SRT_NO_CONNECTION, "None", "none"},
      {GST_SRT_CALLER_CONNECTION, "Caller Mode", "caller"},
      {GST_SRT_LISTENER_CONNECTION, "Listener Mode)", "listener"},
      {GST_SRT_RENDEZVOUS_CONNECTION, "Rendezvous Mode", "rendezvous"},
      {0, NULL, NULL}
    };

    srt_connection_mode_type = g_enum_register_static ("GstSRTConnectionMode",
        pattern_types);
  }

  return srt_connection_mode_type;
}

#define GST_TYPE_SRT_KEY_LENGTH (gst_srt_key_length_get_type ())
static GType
gst_srt_key_length_get_type (void)
{
  static GType gst_srt_key_length_type = 0;

  if (!gst_srt_key_length_type) {
    static GEnumValue pattern_types[] = {
      {GST_SRT_NO_KEY, "no key", "0"},
      {GST_SRT_KEY_128_BITS, "128 bits", "128"},
      {GST_SRT_KEY_192_BITS, "192 bits", "192"},
      {GST_SRT_KEY_256_BITS, "256 bits", "256"},
      {0, NULL, NULL}
    };

    gst_srt_key_length_type = g_enum_register_static ("GstSRTKeyLength",
        pattern_types);
  }

  return gst_srt_key_length_type;
}

void
gst_srt_default_params (GstSRTParams * params, gboolean sender)
{
  g_assert (params != NULL);

  g_free ((gpointer) (params->local_address));
  g_free ((gpointer) (params->remote_host));
  g_free ((gpointer) (params->passphrase));

  memset ((void *) params, 0, sizeof (GstSRTParams));

  params->sender = sender;
  params->conn_mode = GST_SRT_NO_CONNECTION;
  params->latency = SRT_DEFAULT_LATENCY;
  params->key_length = SRT_DEFAULT_KEY_LENGTH;
  params->connect_timeout = -1;
  params->too_late_pkt_drop = -1;
  params->iptos = -1;
  params->nak_report = TRUE;
}

gboolean
gst_srt_validate_params (const GstElement * elem, const GstSRTParams * params)
{
  gboolean ret = TRUE;

  g_assert (elem != NULL);
  g_assert (params != NULL);

  switch (params->conn_mode) {
    case GST_SRT_LISTENER_CONNECTION:
      if (params->local_port == 0) {
        GST_ELEMENT_ERROR (elem, RESOURCE, SETTINGS,
            ("SRT Params specify server connection mode"
                " but local port is not set."), (NULL));
        ret = FALSE;
      }
      break;

    case GST_SRT_RENDEZVOUS_CONNECTION:
    case GST_SRT_CALLER_CONNECTION:
      if (params->remote_port == 0) {
        GST_ELEMENT_ERROR (elem, RESOURCE, SETTINGS,
            ("SRT Params specify client connection mode"
                " but remote port is not set."), (NULL));
        ret = FALSE;
      }
      if (params->remote_host == NULL || strlen (params->remote_host) == 0) {
        GST_ELEMENT_ERROR (elem, RESOURCE, SETTINGS,
            ("SRT Params specify client connection mode"
                " but remote host is not set."), (NULL));
        ret = FALSE;
      }
      break;
    default:
      ret = FALSE;
      break;
  }

  if (params->passphrase != NULL) {
    size_t len = strlen (params->passphrase);
    if (len < 10 || len > 79) {
      GST_ELEMENT_ERROR (elem, RESOURCE, SETTINGS,
          ("SRT passphrase must be between"
              " 10 and 79 characters inclusive."), (NULL));
      ret = FALSE;
    }
  }

  switch (params->key_length) {
    case GST_SRT_NO_KEY:
    case GST_SRT_KEY_128_BITS:
    case GST_SRT_KEY_192_BITS:
    case GST_SRT_KEY_256_BITS:
      break;
    default:
      GST_ELEMENT_ERROR (elem, RESOURCE, SETTINGS,
          ("SRT Params specify invalid key length"), (NULL));
      ret = FALSE;
      break;
  }

  if (params->mss > 0 && params->mss < 76) {
    GST_ELEMENT_ERROR (elem, RESOURCE, SETTINGS,
        ("SRT socket MSS parameter must be"
            " greater than 76 if set."), (NULL));
    ret = FALSE;
  }

  if (params->iptos > 255) {
    GST_ELEMENT_ERROR (elem, RESOURCE, SETTINGS,
        ("SRT IP type of service must be between 0 and 255"
            " (0xFF) inclusive."), (NULL));
    ret = FALSE;
  }
  if (params->overhead_bw > 0 &&
      (params->overhead_bw < 5 || params->overhead_bw > 100)) {
    GST_ELEMENT_ERROR (elem, RESOURCE, SETTINGS,
        ("SRT overhead bandwidth must be between 5%% and"
            " 100%% inclusive."), (NULL));
    ret = FALSE;
  }
  return ret;
}

gboolean
gst_srt_init_params_from_uri (const GstElement * elem,
    GstSRTParams * params, const GstUri * uri)
{
  gboolean ret = FALSE;

  guint16 port;
  const gchar *host;
  GHashTable *qtable = NULL;
  GHashTableIter qtable_it;
  gpointer key, value;
  gint64 value64;
  guint64 uvalue64;
  GError *error = NULL;

  g_assert (params != NULL);
  g_assert (elem != NULL);
  g_assert (uri != NULL);

  gst_srt_default_params (params, params->sender);

  if (g_strcmp0 (gst_uri_get_scheme (uri), SRT_URI_SCHEME) != 0) {
    GST_ELEMENT_ERROR (elem, RESOURCE, SETTINGS,
        ("Invalid SRT URI scheme"), (NULL));
    goto out;
  }

  if (gst_uri_get_userinfo (uri) != NULL) {
    GST_ELEMENT_ERROR (elem, RESOURCE, SETTINGS,
        ("SRT URI doesn't support user/password"), (NULL));
    goto out;
  }

  port = gst_uri_get_port (uri);
  if (port == GST_URI_NO_PORT) {
    GST_ELEMENT_ERROR (elem, RESOURCE, SETTINGS,
        ("SRT URI has missing or invalid port number"), (NULL));
    goto out;
  }

  host = gst_uri_get_host (uri);
  if (host == NULL || !g_strcmp0 (host, "0.0.0.0"))
    params->conn_mode = GST_SRT_LISTENER_CONNECTION;
  else
    params->conn_mode = GST_SRT_CALLER_CONNECTION;

  qtable = gst_uri_get_query_table (uri);
  if (qtable != NULL) {
    g_hash_table_iter_init (&qtable_it, qtable);
    while (g_hash_table_iter_next (&qtable_it, &key, &value)) {
      if (!g_strcmp0 ((const gchar *) key, "mode")) {
        if (!g_strcmp0 ((const gchar *) value, "caller"))
          params->conn_mode = GST_SRT_CALLER_CONNECTION;
        else if (!g_strcmp0 ((const gchar *) value, "listener"))
          params->conn_mode = GST_SRT_LISTENER_CONNECTION;
        else if (!g_strcmp0 ((const gchar *) value, "rendezvous"))
          params->conn_mode = GST_SRT_RENDEZVOUS_CONNECTION;
        else {
          GST_ELEMENT_ERROR (elem, RESOURCE, SETTINGS,
              ("Unrecognized SRT connection mode"), (NULL));
          goto out;
        }
      } else if (!g_strcmp0 ((const gchar *) key, "latency")) {
        if (g_ascii_string_to_signed ((const gchar *) value, 10, 0, INT_MAX,
                &value64, &error)) {
          params->latency = (gint) value64;
        }
      } else if (key && g_str_has_prefix ((const gchar *) key, "pass")) {
        params->passphrase = g_strdup ((const gchar *) value);
        // also default the key length to lowest possible if not set yet
        if (params->key_length == GST_SRT_NO_KEY)
          params->key_length = GST_SRT_KEY_128_BITS;
      } else if (key && g_str_has_prefix ((const gchar *) key, "key")) {
        if (!g_strcmp0 ((const gchar *) value, "0"))
          params->key_length = GST_SRT_NO_KEY;
        else if (!g_strcmp0 ((const gchar *) value, "128"))
          params->key_length = GST_SRT_KEY_128_BITS;
        else if (!g_strcmp0 ((const gchar *) value, "192"))
          params->key_length = GST_SRT_KEY_192_BITS;
        else if (!g_strcmp0 ((const gchar *) value, "256"))
          params->key_length = GST_SRT_KEY_256_BITS;
        else {
          GST_ELEMENT_ERROR (elem, RESOURCE, SETTINGS,
              ("SRT URI key-length missing or invalid value"), (NULL));
          goto out;
        }
      } else if (!g_strcmp0 ((const gchar *) key, "mss")) {
        if (g_ascii_string_to_signed ((const gchar *) value, 10, -1, INT_MAX,
                &value64, &error)) {
          params->mss = (gint) value64;
        }
      } else if (!g_strcmp0 ((const gchar *) key, "srt-send")) {
        if (g_ascii_string_to_signed ((const gchar *) value, 10, 0, INT_MAX,
                &value64, &error)) {
          params->srt_send_buf_sz = (gint) value64;
        }
      } else if (!g_strcmp0 ((const gchar *) key, "srt-recv")) {
        if (g_ascii_string_to_signed ((const gchar *) value, 10, 0, INT_MAX,
                &value64, &error)) {
          params->srt_recv_buf_sz = (gint) value64;
        }
      } else if (!g_strcmp0 ((const gchar *) key, "udp-send")) {
        if (g_ascii_string_to_signed ((const gchar *) value, 10, 0, INT_MAX,
                &value64, &error)) {
          params->udp_send_buf_sz = (gint) value64;
        }
      } else if (!g_strcmp0 ((const gchar *) key, "udp-recv")) {
        if (g_ascii_string_to_signed ((const gchar *) value, 10, 0, INT_MAX,
                &value64, &error)) {
          params->udp_recv_buf_sz = (gint) value64;
        }
      } else if (!g_strcmp0 ((const gchar *) key, "too-late")) {
        if (g_ascii_string_to_signed ((const gchar *) value, 10, -1, 1,
                &value64, &error)) {
          params->too_late_pkt_drop = (gint) value64;
        }
      } else if (!g_strcmp0 ((const gchar *) key, "input-rate")) {
        if (g_ascii_string_to_signed ((const gchar *) value, 10, -1, LONG_MAX,
                &value64, &error)) {
          params->input_rate = value64;
        }
      } else if (!g_strcmp0 ((const gchar *) key, "overhead")) {
        if (g_ascii_string_to_signed ((const gchar *) value, 10, 5, 100,
                &value64, &error)) {
          params->overhead_bw = (gint) value64;
        }
      } else if (!g_strcmp0 ((const gchar *) key, "maxbw")) {
        if (g_ascii_string_to_signed ((const gchar *) value, 10, -1, LONG_MAX,
                &value64, &error)) {
          params->max_bw = value64;
        }
      } else if (!g_strcmp0 ((const gchar *) key, "iptos")) {
        if (g_ascii_string_to_signed ((const gchar *) value, 10, -1, 255,
                &value64, &error)) {
          params->iptos = (gint) value64;
        }
      } else if (!g_strcmp0 ((const gchar *) key, "ipttl")) {
        if (g_ascii_string_to_signed ((const gchar *) value, 10, -1, 255,
                &value64, &error)) {
          params->ipttl = (gint) value64;
        }
      } else if (!g_strcmp0 ((const gchar *) key, "remotehost")) {
        params->remote_host = g_strdup ((const gchar *) value);
      } else if (!g_strcmp0 ((const gchar *) key, "remoteport")) {
        if (g_ascii_string_to_unsigned ((const gchar *) value, 10, 1, USHRT_MAX,
                &uvalue64, &error)) {
          params->remote_port = (guint16) uvalue64;
        }
      } else if (!g_strcmp0 ((const gchar *) key, "localaddress")) {
        params->local_address = g_strdup ((const gchar *) value);
      } else if (!g_strcmp0 ((const gchar *) key, "localport")) {
        if (g_ascii_string_to_unsigned ((const gchar *) value, 10, 1, USHRT_MAX,
                &uvalue64, &error)) {
          params->local_port = (guint16) uvalue64;
        }
      } else if (key) {
        GST_ELEMENT_WARNING (elem, RESOURCE, SETTINGS,
            ("Failed to parse SRT URI parameter: %s", (const gchar *) key),
            ("%s", (error != NULL) ? error->message : ""));
      }
      if (error != NULL) {
        g_error_free (error);
        error = NULL;
      }
    }
  }

  switch (params->conn_mode) {
    case GST_SRT_LISTENER_CONNECTION:
      /// For listener mode we always use the address and port specified
      /// int the URI. And localport and localaddress keys are ignored.
      params->local_port = port;
      g_free (params->local_address);
      params->local_address = g_strdup (host);
      break;

    case GST_SRT_CALLER_CONNECTION:
    case GST_SRT_RENDEZVOUS_CONNECTION:
      // for rendezvous and caller connections URI specifies the remote
      params->remote_port = port;
      params->remote_host = g_strdup (host);
      break;

    default:
      g_free ((gpointer) host);
      GST_ELEMENT_ERROR (elem, RESOURCE, SETTINGS,
          ("SRT URI connection mode is not set"), (NULL));
      goto out;
      break;
  }

  ret = gst_srt_validate_params (elem, params);

out:
  if (qtable != NULL)
    g_hash_table_unref (qtable);
  if (!ret)
    gst_srt_default_params (params, params->sender);

  return ret;
}

void
gst_srt_install_properties (GObjectClass * gobject_class, gint offset)
{
  g_assert (gobject_class != NULL);

  g_object_class_install_property (gobject_class, PROP_MODE + offset,
      g_param_spec_enum ("mode", "Mode",
          "Connection mode {caller,listener,rendezvous}",
          GST_TYPE_SRT_CONNECTION_MODE,
          GST_SRT_NO_CONNECTION, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_LATENCY + offset,
      g_param_spec_int ("latency", "latency",
          "Minimum latency (milliseconds)",
          -1, G_MAXINT32, SRT_DEFAULT_LATENCY,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_PASSPHRASE + offset,
      g_param_spec_string ("passphrase", "Passphrase",
          "The password for the encrypted transmission", NULL,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_KEY_LENGTH + offset,
      g_param_spec_enum ("key-length", "key length",
          "Crypto key length in bits {0,128,192,256}", GST_TYPE_SRT_KEY_LENGTH,
          GST_SRT_NO_KEY, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_MSS + offset,
      g_param_spec_int ("mss", "MSS",
          "Maximum Segment Size",
          0, G_MAXINT32, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_SRT_SEND_BUF_SZ + offset,
      g_param_spec_int ("srt-send", "SRT send buf",
          "SRT Send buffer size",
          0, G_MAXINT32, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_SRT_RECV_BUF_SZ + offset,
      g_param_spec_int ("srt-recv", "SRT receive buf",
          "SRT Receive buffer size",
          0, G_MAXINT32, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_UDP_SEND_BUF_SZ + offset,
      g_param_spec_int ("udp-send", "UDP send buf",
          "UDP Send buffer size",
          0, G_MAXINT32, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_UDP_RECV_BUF_SZ + offset,
      g_param_spec_int ("udp-recv", "UDP receive buf",
          "UDP Receive buffer size",
          0, G_MAXINT32, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
      PROP_TOO_LATE_PKT_DROP + offset,
      g_param_spec_int ("too-late", "Too-late packet drop",
          "Drop packets that are too late",
          -1, 1, -1, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_INPUT_RATE + offset,
      g_param_spec_uint64 ("input-rate", "Input rate",
          "Maximum BW with possible overhead",
          -1, G_MAXUINT64, -1, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_OVERHEAD_BW + offset,
      g_param_spec_int ("overhead", "Overhead bw",
          "Overhead BW (used only if input-rate is used and maxbw == 0)",
          -1, 100, -1, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_MAXBW_BW + offset,
      g_param_spec_int64 ("maxbw", "Maximum bandwidth",
          "Maximum bandwidth",
          -2, G_MAXINT64, -2, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_IPTOS + offset,
      g_param_spec_int ("iptos", "IP TOS",
          "IP type of service",
          -1, 255, -1, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_IPTTL + offset,
      g_param_spec_int ("ipttl", "IP TTL",
          "IP time to live",
          -1, 255, -1, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_LOCAL_ADDRESS + offset,
      g_param_spec_string ("localaddress", "Local Address",
          "Address to bind socket to", NULL,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_LOCAL_PORT + offset,
      g_param_spec_int ("localport", "Local Port",
          "Port to bind socket to (Ignored in rendez-vous mode)",
          0, G_MAXUINT16, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
}

gboolean
gst_srt_get_property (const GstSRTParams * params,
    const GObject * object, guint prop_id, GValue * value, gint offset)
{
  gboolean res = TRUE;

  g_assert (params != NULL);
  g_assert (object != NULL);
  g_assert (value != NULL);

  switch (prop_id - offset) {
    case PROP_MODE:
      g_value_set_enum (value, params->conn_mode);
      break;
    case PROP_LATENCY:
      g_value_set_int (value, params->latency);
      break;
    case PROP_PASSPHRASE:
      g_value_set_string (value, params->passphrase);
      break;
    case PROP_KEY_LENGTH:
      g_value_set_enum (value, params->key_length);
      break;
    case PROP_MSS:
      g_value_set_int (value, params->mss);
      break;
    case PROP_SRT_SEND_BUF_SZ:
      g_value_set_int (value, params->srt_send_buf_sz);
      break;
    case PROP_SRT_RECV_BUF_SZ:
      g_value_set_int (value, params->srt_recv_buf_sz);
      break;
    case PROP_UDP_SEND_BUF_SZ:
      g_value_set_int (value, params->udp_send_buf_sz);
      break;
    case PROP_UDP_RECV_BUF_SZ:
      g_value_set_int (value, params->udp_recv_buf_sz);
      break;
    case PROP_TOO_LATE_PKT_DROP:
      g_value_set_int (value, params->too_late_pkt_drop);
      break;
    case PROP_INPUT_RATE:
      g_value_set_int64 (value, params->input_rate);
      break;
    case PROP_OVERHEAD_BW:
      g_value_set_int (value, params->overhead_bw);
      break;
    case PROP_MAXBW_BW:
      g_value_set_int64 (value, params->max_bw);
      break;
    case PROP_IPTOS:
      g_value_set_int (value, params->iptos);
      break;
    case PROP_IPTTL:
      g_value_set_int (value, params->ipttl);
      break;
    case PROP_REMOTE_HOST:
      g_value_set_string (value, params->remote_host);
      break;
    case PROP_REMOTE_PORT:
      g_value_set_int (value, params->remote_port);
      break;
    case PROP_LOCAL_ADDRESS:
      g_value_set_string (value, params->local_address);
      break;
    case PROP_LOCAL_PORT:
      g_value_set_int (value, params->local_port);
      break;
    default:
      res = FALSE;
      break;
  }

  return res;
}

gboolean
gst_srt_set_property (GstSRTParams * params,
    const GObject * object, guint prop_id, const GValue * value, gint offset)
{
  gboolean res = TRUE;

  g_assert (params != NULL);
  g_assert (object != NULL);
  g_assert (value != NULL);

  switch (prop_id - offset) {
    case PROP_MODE:
      params->conn_mode = g_value_get_enum (value);
      break;
    case PROP_LATENCY:
      params->latency = g_value_get_int (value);
      break;
    case PROP_PASSPHRASE:
      g_free (params->passphrase);
      params->passphrase = g_value_dup_string (value);
      // also default the key length to lowest possible if not set yet
      if (params->key_length == GST_SRT_NO_KEY)
        params->key_length = GST_SRT_KEY_128_BITS;
      break;
    case PROP_KEY_LENGTH:
      params->key_length = g_value_get_enum (value);
      break;
    case PROP_MSS:
      params->mss = g_value_get_int (value);
      break;
    case PROP_SRT_SEND_BUF_SZ:
      params->srt_send_buf_sz = g_value_get_int (value);
      break;
    case PROP_SRT_RECV_BUF_SZ:
      params->srt_recv_buf_sz = g_value_get_int (value);
      break;
    case PROP_UDP_SEND_BUF_SZ:
      params->udp_send_buf_sz = g_value_get_int (value);
      break;
    case PROP_UDP_RECV_BUF_SZ:
      params->udp_recv_buf_sz = g_value_get_int (value);
      break;
    case PROP_TOO_LATE_PKT_DROP:
      params->too_late_pkt_drop = g_value_get_int (value);
      break;
    case PROP_INPUT_RATE:
      params->input_rate = g_value_get_int64 (value);
      break;
    case PROP_OVERHEAD_BW:
      params->overhead_bw = g_value_get_int (value);
      break;
    case PROP_MAXBW_BW:
      params->max_bw = g_value_get_int64 (value);
      break;
    case PROP_IPTOS:
      params->iptos = g_value_get_int (value);
      break;
    case PROP_IPTTL:
      params->ipttl = g_value_get_int (value);
      break;
    case PROP_REMOTE_HOST:
      g_free (params->remote_host);
      params->remote_host = g_value_dup_string (value);
      break;
    case PROP_REMOTE_PORT:
      params->remote_port = (guint16) g_value_get_int (value);
      break;
    case PROP_LOCAL_ADDRESS:
      g_free (params->local_address);
      params->local_address = g_value_dup_string (value);
      break;
    case PROP_LOCAL_PORT:
      params->local_port = (guint16) g_value_get_int (value);
      break;
    default:
      res = FALSE;
      break;
  }

  return res;
}

static SRTSOCKET
gst_srt_create_socket (const GstElement * elem, const GstSRTParams * params)
{
  SRTSOCKET sock = SRT_INVALID_SOCK;
  gint ival, mss;

  if (!gst_srt_validate_params (elem, params))
    goto failed;

  // SRT only supports IPv4 and datagram sockets.
  sock = srt_socket (AF_INET, SOCK_DGRAM, 0);
  if (sock == SRT_INVALID_SOCK) {
    GST_ELEMENT_ERROR (elem, LIBRARY, INIT,
        ("SRT socket create failed"),
        ("failed to create SRT socket (reason: %s)", srt_getlasterror_str ()));
    goto failed;
  }
  // Use non-blocking mode
  ival = 0;
  if (srt_setsockopt (sock, 0, SRTO_SNDSYN, &ival, sizeof (ival))) {
    GST_ELEMENT_ERROR (elem, LIBRARY, INIT,
        ("SRT setsockopt failed"),
        ("failed to set SRTO_SNDSYN (reason: %s)", srt_getlasterror_str ()));
    goto failed;
  }

  if (srt_setsockopt (sock, 0, SRTO_RCVSYN, &ival, sizeof (ival))) {
    GST_ELEMENT_ERROR (elem, LIBRARY, INIT,
        ("SRT setsockopt failed"),
        ("failed to set SRTO_RCVSYN (reason: %s)", srt_getlasterror_str ()));
    goto failed;
  }
  // For RENDEZVOUS_CONNECTION, the appropriate socket option must be set.
  ival = (params->conn_mode == GST_SRT_RENDEZVOUS_CONNECTION) ? 1 : 0;
  if (srt_setsockopt (sock, 0, SRTO_RENDEZVOUS, &ival, sizeof (ival))) {
    GST_ELEMENT_ERROR (elem, LIBRARY, INIT,
        ("SRT setsockopt failed"),
        ("failed to set SRTO_RENDEZVOUS (reason: %s)",
            srt_getlasterror_str ()));
    goto failed;
  }
  // disable lingering
  ival = 0;
  if (srt_setsockopt (sock, 0, SRTO_LINGER, &ival, sizeof (ival))) {
    GST_ELEMENT_ERROR (elem, LIBRARY, INIT,
        ("SRT setsockopt failed"),
        ("failed to set SRTO_LINGER (reason: %s)", srt_getlasterror_str ()));
    goto failed;
  }
  // Timestamp-based Packet Delivery mode must be enabled
  ival = 1;
  if (srt_setsockopt (sock, 0, SRTO_TSBPDMODE, &ival, sizeof (ival))) {
    GST_ELEMENT_ERROR (elem, LIBRARY, INIT,
        ("SRT setsockopt failed"),
        ("failed to set SRTO_TSBPDMODE (reason: %s)", srt_getlasterror_str ()));
    goto failed;
  }

  ival = params->sender;
  if (srt_setsockopt (sock, 0, SRTO_SENDER, &ival, sizeof (ival))) {
    GST_ELEMENT_ERROR (elem, LIBRARY, INIT,
        ("SRT setsockopt failed"),
        ("failed to set SRTO_SENDER (reason: %s)", srt_getlasterror_str ()));
    goto failed;
  }

  ival = (params->latency < 0) ? SRT_DEFAULT_LATENCY : params->latency;
  if (srt_setsockopt (sock, 0, SRTO_TSBPDDELAY, &ival, sizeof (ival))) {
    GST_ELEMENT_ERROR (elem, LIBRARY, INIT,
        ("SRT setsockopt failed"),
        ("failed to set SRTO_TSBPDDELAY (reason: %s)",
            srt_getlasterror_str ()));
    goto failed;
  }

  if (params->passphrase != NULL) {
    if (srt_setsockopt (sock, 0, SRTO_PASSPHRASE,
            params->passphrase, strlen (params->passphrase) + 1)) {
      GST_ELEMENT_ERROR (elem, LIBRARY, INIT,
          ("SRT setsockopt failed"),
          ("failed to set SRTO_PASSPHRASE (reason: %s)",
              srt_getlasterror_str ()));
      goto failed;
    }
  }

  ival = params->key_length;
  if (srt_setsockopt (sock, 0, SRTO_PBKEYLEN, &ival, sizeof (ival))) {
    GST_ELEMENT_ERROR (elem, LIBRARY, INIT,
        ("SRT setsockopt failed"),
        ("failed to set SRTO_PBKEYLEN (reason: %s)", srt_getlasterror_str ()));
    goto failed;
  }

  ival = (params->connect_timeout < 0) ? 8000 : params->connect_timeout;
  if (srt_setsockopt (sock, 0, SRTO_CONNTIMEO, &ival, sizeof (ival))) {
    GST_ELEMENT_ERROR (elem, LIBRARY, INIT,
        ("SRT setsockopt failed"),
        ("failed to set SRTO_CONNTIMEO (reason: %s)", srt_getlasterror_str ()));
    goto failed;
  }

  mss = (params->mss <= 0) ? 1500 : params->mss;
  if (srt_setsockopt (sock, 0, SRTO_MSS, &mss, sizeof (ival))) {
    GST_ELEMENT_ERROR (elem, LIBRARY, INIT,
        ("SRT setsockopt failed"),
        ("failed to set SRTO_MSS (reason: %s)", srt_getlasterror_str ()));
    goto failed;
  }

  ival = (params->srt_send_buf_sz <= 0) ?
      8192 * (mss - 28) : params->srt_send_buf_sz;
  if (srt_setsockopt (sock, 0, SRTO_SNDBUF, &ival, sizeof (ival))) {
    GST_ELEMENT_ERROR (elem, LIBRARY, INIT,
        ("SRT setsockopt failed"),
        ("failed to set SRTO_SNDBUF (reason: %s)", srt_getlasterror_str ()));
    goto failed;
  }

  ival = (params->srt_recv_buf_sz <= 0) ?
      8192 * (mss - 28) : params->srt_recv_buf_sz;
  if (srt_setsockopt (sock, 0, SRTO_RCVBUF, &ival, sizeof (ival))) {
    GST_ELEMENT_ERROR (elem, LIBRARY, INIT,
        ("SRT setsockopt failed"),
        ("failed to set SRTO_RCVBUF (reason: %s)", srt_getlasterror_str ()));
    goto failed;
  }

  ival = (params->udp_send_buf_sz <= 0) ? 1024 * 1024 : params->udp_send_buf_sz;
  if (srt_setsockopt (sock, 0, SRTO_UDP_SNDBUF, &ival, sizeof (ival))) {
    GST_ELEMENT_ERROR (elem, LIBRARY, INIT,
        ("SRT setsockopt failed"),
        ("failed to set SRTO_UDP_SNDBUF (reason: %s)",
            srt_getlasterror_str ()));
    goto failed;
  }

  ival = (params->udp_recv_buf_sz <= 0) ? 8192 * mss : params->udp_recv_buf_sz;
  if (srt_setsockopt (sock, 0, SRTO_UDP_RCVBUF, &ival, sizeof (ival))) {
    GST_ELEMENT_ERROR (elem, LIBRARY, INIT,
        ("SRT setsockopt failed"),
        ("failed to set SRTO_UDP_RCVBUF (reason: %s)",
            srt_getlasterror_str ()));
    goto failed;
  }

  ival = (params->too_late_pkt_drop < 0) ?
      (params->sender ? 0 : 1) : params->too_late_pkt_drop;
  if (srt_setsockopt (sock, 0, SRTO_TLPKTDROP, &ival, sizeof (ival))) {
    GST_ELEMENT_ERROR (elem, LIBRARY, INIT,
        ("SRT setsockopt failed"),
        ("failed to set SRTO_TLPKTDROP (reason: %s)", srt_getlasterror_str ()));
    goto failed;
  }

  ival = (params->nak_report) ? 1 : 0;
  if (srt_setsockopt (sock, 0, SRTO_NAKREPORT, &ival, sizeof (ival))) {
    GST_ELEMENT_ERROR (elem, LIBRARY, INIT,
        ("SRT setsockopt failed"),
        ("failed to set SRTO_NAKREPORT (reason: %s)", srt_getlasterror_str ()));
    goto failed;
  }

  if (params->sender) {
    ival = (params->input_rate < 0) ? 0 : params->input_rate;
    if (srt_setsockopt (sock, 0, SRTO_INPUTBW, &ival, sizeof (ival))) {
      GST_ELEMENT_ERROR (elem, LIBRARY, INIT,
          ("SRT setsockopt failed"),
          ("failed to set SRTO_INPUTBW (reason: %s)", srt_getlasterror_str ()));
      goto failed;
    }

    ival = (params->overhead_bw <= 0) ? 25 : params->overhead_bw;
    if (srt_setsockopt (sock, 0, SRTO_OHEADBW, &ival, sizeof (ival))) {
      GST_ELEMENT_ERROR (elem, LIBRARY, INIT,
          ("SRT setsockopt failed"),
          ("failed to set SRTO_OHEADBW (reason: %s)", srt_getlasterror_str ()));
      goto failed;
    }

    ival = (params->max_bw < -1) ? 0 : params->max_bw;
    if (srt_setsockopt (sock, 0, SRTO_MAXBW, &ival, sizeof (ival))) {
      GST_ELEMENT_ERROR (elem, LIBRARY, INIT,
          ("SRT setsockopt failed"),
          ("failed to set SRTO_MAXBW (reason: %s)", srt_getlasterror_str ()));
      goto failed;
    }

    ival = (params->iptos < 0) ? 0xB8 : params->iptos;
    if (srt_setsockopt (sock, 0, SRTO_IPTOS, &ival, sizeof (ival))) {
      GST_ELEMENT_ERROR (elem, LIBRARY, INIT,
          ("SRT setsockopt failed"),
          ("failed to set SRTO_IPTOS (reason: %s)", srt_getlasterror_str ()));
      goto failed;
    }

    ival = (params->ipttl <= 0) ? 64 : params->ipttl;
    if (srt_setsockopt (sock, 0, SRTO_IPTTL, &ival, sizeof (ival))) {
      GST_ELEMENT_ERROR (elem, LIBRARY, INIT,
          ("SRT setsockopt failed"),
          ("failed to set SRTO_IPTTL (reason: %s)", srt_getlasterror_str ()));
      goto failed;
    }
  }

  return sock;

failed:
  if (sock != SRT_INVALID_SOCK)
    srt_close (sock);

  return SRT_INVALID_SOCK;
}

static gboolean
gst_srt_activate_socket (const GstElement * elem,
    const SRTSOCKET sock, const GstSRTParams * params)
{
  guint16 local_port;
  GError *error = NULL;
  const char *local_address =
      (params->local_address != NULL) ? params->local_address : "0.0.0.0";

  if (!gst_srt_validate_params (elem, params))
    goto failed;

  local_port = params->local_port;
  if (params->conn_mode == GST_SRT_RENDEZVOUS_CONNECTION && local_port == 0) {
    // for rendezvous mode bind localy to the same port as the remote
    // unless specified otherwise
    local_port = params->remote_port;
  }
  // For all modes, bind local port and address if specified
  if (local_port != 0) {
    GSocketAddress *gsock_addr;
    gpointer sock_addr;
    gsize sock_addr_len;

    gsock_addr =
        g_inet_socket_address_new_from_string (local_address, local_port);

    if (gsock_addr == NULL) {
      GST_ELEMENT_ERROR (elem, RESOURCE, OPEN_READ_WRITE,
          ("Invalid local host"), ("Failed to resolve local host"));
      goto failed;
    }

    if (g_socket_address_get_family (gsock_addr) != G_SOCKET_FAMILY_IPV4) {
      GST_ELEMENT_ERROR (elem, RESOURCE, OPEN_READ_WRITE,
          ("Invalid local address"), ("SRT only supports IPv4 addresses"));
      g_clear_object (&gsock_addr);
      goto failed;
    }

    sock_addr_len = g_socket_address_get_native_size (gsock_addr);
    sock_addr = g_alloca (sock_addr_len);
    if (!g_socket_address_to_native (gsock_addr, sock_addr,
            sock_addr_len, &error)) {
      GST_ELEMENT_ERROR (elem, RESOURCE, OPEN_READ_WRITE,
          ("to native sockaddr failed"),
          ("Can't parse local address to sockaddr: %s", error->message));
      g_clear_object (&gsock_addr);
      g_error_free (error);
      goto failed;
    }
    g_clear_object (&gsock_addr);

    if (srt_bind (sock, sock_addr, sock_addr_len) == SRT_ERROR) {
      GST_ELEMENT_ERROR (elem, RESOURCE, OPEN_READ_WRITE,
          ("Can't bind to address"),
          ("Can't bind to %s:%d (reason: %s)",
              local_address, local_port, srt_getlasterror_str ()));
      goto failed;
    }
  }

  if (params->conn_mode == GST_SRT_RENDEZVOUS_CONNECTION
      || params->conn_mode == GST_SRT_CALLER_CONNECTION) {
    GSocketAddress *gsock_addr;
    gpointer sock_addr;
    gsize sock_addr_len;

    gsock_addr =
        g_inet_socket_address_new_from_string (params->remote_host,
        params->remote_port);

    if (gsock_addr == NULL) {
      GST_ELEMENT_ERROR (elem, RESOURCE, OPEN_READ_WRITE,
          ("Invalid remote host"), ("Failed to resolve remote host"));
      goto failed;
    }

    if (g_socket_address_get_family (gsock_addr) != G_SOCKET_FAMILY_IPV4) {
      GST_ELEMENT_ERROR (elem, RESOURCE, OPEN_READ_WRITE,
          ("Invalid remote address"), ("SRT only supports IPv4 addresses"));
      g_clear_object (&gsock_addr);
      goto failed;
    }

    sock_addr_len = g_socket_address_get_native_size (gsock_addr);
    sock_addr = g_alloca (sock_addr_len);
    if (!g_socket_address_to_native (gsock_addr, sock_addr, sock_addr_len,
            &error)) {
      GST_ELEMENT_ERROR (elem, RESOURCE, OPEN_READ,
          ("to native sockaddr failed"),
          ("Can't parse remote address to sockaddr: %s", error->message));
      g_clear_object (&gsock_addr);
      g_error_free (error);
      goto failed;
    }

    if (srt_connect (sock, sock_addr, sock_addr_len) == SRT_ERROR) {
      GST_ELEMENT_ERROR (elem, RESOURCE, OPEN_READ_WRITE,
          ("Connect failed"),
          ("Could't schedule connect to %s:%d (reason: %s)",
              params->remote_host, params->remote_port,
              srt_getlasterror_str ()));
      g_clear_object (&gsock_addr);
      goto failed;
    }
    g_clear_object (&gsock_addr);

    GST_LOG_OBJECT (elem,
        "Scheduled connect to remote SRT endpoint %s:%d",
        params->remote_host, params->remote_port);
  } else {                      // GST_SRT_LISTENER_CONNECTION
    if (srt_listen (sock, 1) == SRT_ERROR) {
      GST_ELEMENT_ERROR (elem, RESOURCE, OPEN_READ_WRITE,
          ("Listen failed"),
          ("Could't starting listen on %s:%d (reason: %s)",
              local_address, local_port, srt_getlasterror_str ()));
    }

    GST_LOG_OBJECT (elem,
        "Listening on SRT endpoint %s:%d", local_address, local_port);
  }

  return TRUE;

failed:
  return FALSE;
}

SRTSOCKET
gst_srt_start_socket (const GstElement * elem, const GstSRTParams * params)
{
  SRTSOCKET sock = gst_srt_create_socket (elem, params);
  if (sock != SRT_INVALID_SOCK) {
    if (!gst_srt_activate_socket (elem, sock, params)) {
      srt_close (sock);
      sock = SRT_INVALID_SOCK;
    }
  }
  return sock;
}

gsize
gst_srt_send (GstElement * elem, const SRTSOCKET sock,
    const guint8 * buffer, gsize length)
{
  const guint8 *msg = buffer;

  if (sock == SRT_INVALID_SOCK)
    return 0;

  if (srt_getsockstate (sock) != SRTS_CONNECTED)
    return 0;

  while (msg < (buffer + length)) {
    gsize msglen;
    gint rc;

    msglen = MIN (length - (msg - buffer), SRT_DEFAULT_MSG_SIZE);

    rc = srt_sendmsg (sock, (const char *) msg, msglen, -1, TRUE);
    if (rc <= 0) {
      GST_WARNING_OBJECT (elem,
          "Error sending data on SRT socket: %s", srt_getlasterror_str ());

      break;
    }
    msg += rc;
  }

  return (gsize) (msg - buffer);
}

GstStructure *
gst_srt_get_stats (GSocketAddress * sockaddr, SRTSOCKET sock)
{
  SRT_TRACEBSTATS stats;
  int ret;
  GValue v = G_VALUE_INIT;
  GstStructure *s;

  if (sock == SRT_INVALID_SOCK || sockaddr == NULL)
    return gst_structure_new_empty ("application/x-srt-statistics");

  s = gst_structure_new ("application/x-srt-statistics",
      "sockaddr", G_TYPE_SOCKET_ADDRESS, sockaddr, NULL);

  ret = srt_bstats (sock, &stats, 0);
  if (ret >= 0) {
    gst_structure_set (s,
        /* number of sent data packets, including retransmissions */
        "packets-sent", G_TYPE_INT64, stats.pktSent,
        /* number of lost packets (sender side) */
        "packets-sent-lost", G_TYPE_INT, stats.pktSndLoss,
        /* number of retransmitted packets */
        "packets-retransmitted", G_TYPE_INT, stats.pktRetrans,
        /* number of received ACK packets */
        "packet-ack-received", G_TYPE_INT, stats.pktRecvACK,
        /* number of received NAK packets */
        "packet-nack-received", G_TYPE_INT, stats.pktRecvNAK,
        /* time duration when UDT is sending data (idle time exclusive) */
        "send-duration-us", G_TYPE_INT64, stats.usSndDuration,
        /* number of sent data bytes, including retransmissions */
        "bytes-sent", G_TYPE_UINT64, stats.byteSent,
        /* number of retransmitted bytes */
        "bytes-retransmitted", G_TYPE_UINT64, stats.byteRetrans,
        /* number of too-late-to-send dropped bytes */
        "bytes-sent-dropped", G_TYPE_UINT64, stats.byteSndDrop,
        /* number of too-late-to-send dropped packets */
        "packets-sent-dropped", G_TYPE_INT, stats.pktSndDrop,
        /* sending rate in Mb/s */
        "send-rate-mbps", G_TYPE_DOUBLE, stats.msRTT,
        /* estimated bandwidth, in Mb/s */
        "bandwidth-mbps", G_TYPE_DOUBLE, stats.mbpsBandwidth,
        /* busy sending time (i.e., idle time exclusive) */
        "send-duration-us", G_TYPE_UINT64, stats.usSndDuration,
        "rtt-ms", G_TYPE_DOUBLE, stats.msRTT,
        "negotiated-latency-ms", G_TYPE_INT, stats.msSndTsbPdDelay, NULL);
  }

  g_value_init (&v, G_TYPE_STRING);
  g_value_take_string (&v,
      g_socket_connectable_to_string (G_SOCKET_CONNECTABLE (sockaddr)));
  gst_structure_take_value (s, "sockaddr-str", &v);

  return s;
}

static gboolean
plugin_init (GstPlugin * plugin)
{
  GST_DEBUG_CATEGORY_INIT (GST_CAT_DEFAULT, "srt", 0, "SRT Common code");

  if (!gst_element_register (plugin, "srtsrc", GST_RANK_PRIMARY,
          GST_TYPE_SRT_SRC))
    return FALSE;
  if (!gst_element_register (plugin, "srtsink", GST_RANK_PRIMARY,
          GST_TYPE_SRT_SINK))
    return FALSE;

  return TRUE;
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    srt,
    "transfer data via SRT",
    plugin_init, VERSION, GST_LICENSE, GST_PACKAGE_NAME, GST_PACKAGE_ORIGIN);
