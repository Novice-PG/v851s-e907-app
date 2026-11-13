/*
 * Linkr Bee BLE peripheral for the V851S A7 (BlueZ 5.54, GATT server).
 *
 * Exposes the three services the Linkr Bee clients expect:
 *   - Nordic UART Service (NUS)              6e400001-... (notify/write)
 *   - Linkr Management Service v1            4c4b0001-... (read/write/indicate)
 *   - Linkr Reliable UART Service v1         4c4b0010-... (write/indicate/read)
 *
 * Serial bytes are relayed to/from the Python daemon over a Unix socket
 * (/var/run/linkr-bee.sock), which owns the single rpmsg endpoint connected
 * to the E907.  Management commands are relayed the same way; the daemon
 * implements the Linkr session protocol and the read-only queries.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define _GNU_SOURCE

#include <errno.h>
#include <gio/gio.h>
#include <glib-unix.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define APP_PATH "/com/v851s/linkr"
#define ADV_PATH APP_PATH "/advertisement"
#define AGENT_PATH APP_PATH "/agent"
#define SOCK_PATH "/var/run/linkr-bee.sock"

#define NUS_SVC APP_PATH "/s0"
#define NUS_RX NUS_SVC "/c0"
#define NUS_TX NUS_SVC "/c1"
#define MGMT_SVC APP_PATH "/s1"
#define MGMT_PROTO MGMT_SVC "/c0"
#define MGMT_ID MGMT_SVC "/c1"
#define MGMT_CMD MGMT_SVC "/c2"
#define MGMT_RSP MGMT_SVC "/c3"
#define REL_SVC APP_PATH "/s2"
#define REL_RX REL_SVC "/c0"
#define REL_TX REL_SVC "/c1"
#define REL_STATE REL_SVC "/c2"

#define UUID_NUS "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define UUID_NUS_RX "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
#define UUID_NUS_TX "6e400003-b5a3-f393-e0a9-e50e24dcca9e"
#define UUID_MGMT "4c4b0001-9a7e-4f4e-8b8a-3d6f12a0c001"
#define UUID_MGMT_PROTO "4c4b0002-9a7e-4f4e-8b8a-3d6f12a0c001"
#define UUID_MGMT_ID "4c4b0003-9a7e-4f4e-8b8a-3d6f12a0c001"
#define UUID_MGMT_CMD "4c4b0004-9a7e-4f4e-8b8a-3d6f12a0c001"
#define UUID_MGMT_RSP "4c4b0005-9a7e-4f4e-8b8a-3d6f12a0c001"
#define UUID_REL "4c4b0010-9a7e-4f4e-8b8a-3d6f12a0c001"
#define UUID_REL_RX "4c4b0011-9a7e-4f4e-8b8a-3d6f12a0c001"
#define UUID_REL_TX "4c4b0012-9a7e-4f4e-8b8a-3d6f12a0c001"
#define UUID_REL_STATE "4c4b0013-9a7e-4f4e-8b8a-3d6f12a0c001"

#define ADV_NAME "Linkr Bee V851S"

/* Management capabilities advertised on the protocol-info characteristic:
 * wifi(0) | webdav(1) | websocket(2) | device id(3) | async events(4) |
 * reliable uart(5) | multi session(7). */
#define MGMT_CAPS 0xBFu
#define MGMT_MAX_PAYLOAD 512

/* Unix socket frames: type(1) len(2 LE) payload */
#define FRAME_DATA 0x01
#define FRAME_CTL_REQ 0x02
#define FRAME_CTL_RSP 0x03
#define FRAME_HELLO 0x10
#define FRAME_NAME 0x11
/* Async management event: reqid(4 LE) flags(2 LE) text.  Sent for WiFi
 * operation completion and scan results, which are not tied to the reply of
 * the request that started them. */
#define FRAME_EVENT 0x12

/* Custom advertised name, persisted by the daemon on UDISK. */
#define NAME_FILE "/mnt/UDISK/persist/ble-name"
#define BLE_NAME_MAX 29

#define ATT_CHUNK 20

enum ckind {
	CK_NUS_RX,
	CK_NUS_TX,
	CK_MGMT_PROTO,
	CK_MGMT_ID,
	CK_MGMT_CMD,
	CK_MGMT_RSP,
	CK_REL_RX,
	CK_REL_TX,
	CK_REL_STATE,
	CK_COUNT
};

struct chr_def {
	const char *path;
	const char *uuid;
	const char *svc;
	const char *flags[4];
	enum ckind kind;
};

static const struct chr_def chars[CK_COUNT] = {
	[CK_NUS_RX] = { NUS_RX, UUID_NUS_RX, NUS_SVC,
			{ "write", "write-without-response", NULL }, CK_NUS_RX },
	[CK_NUS_TX] = { NUS_TX, UUID_NUS_TX, NUS_SVC,
			{ "notify", NULL }, CK_NUS_TX },
	[CK_MGMT_PROTO] = { MGMT_PROTO, UUID_MGMT_PROTO, MGMT_SVC,
			    { "read", NULL }, CK_MGMT_PROTO },
	[CK_MGMT_ID] = { MGMT_ID, UUID_MGMT_ID, MGMT_SVC,
			 { "read", NULL }, CK_MGMT_ID },
	[CK_MGMT_CMD] = { MGMT_CMD, UUID_MGMT_CMD, MGMT_SVC,
			  { "write", NULL }, CK_MGMT_CMD },
	[CK_MGMT_RSP] = { MGMT_RSP, UUID_MGMT_RSP, MGMT_SVC,
			  { "indicate", NULL }, CK_MGMT_RSP },
	[CK_REL_RX] = { REL_RX, UUID_REL_RX, REL_SVC,
			{ "write", NULL }, CK_REL_RX },
	[CK_REL_TX] = { REL_TX, UUID_REL_TX, REL_SVC,
			{ "indicate", NULL }, CK_REL_TX },
	[CK_REL_STATE] = { REL_STATE, UUID_REL_STATE, REL_SVC,
			   { "read", NULL }, CK_REL_STATE },
};

struct svc_def {
	const char *path;
	const char *uuid;
};

static const struct svc_def services[3] = {
	{ NUS_SVC, UUID_NUS },
	{ MGMT_SVC, UUID_MGMT },
	{ REL_SVC, UUID_REL },
};

static GDBusConnection *bus;
static GMainLoop *loop;
static gboolean notifying[CK_COUNT];

/* Advertised name at runtime. Starts from ADV_NAME, may be replaced by a
 * custom name loaded from UDISK or pushed by the daemon via FRAME_NAME. */
static char adv_name[BLE_NAME_MAX + 1] = ADV_NAME;

static guint8 device_id[16];
static guint8 proto_info[10];

/* Reliable UART state */
static guint32 rel_rx_expected = 1;
static guint32 rel_tx_next = 1;
static guint32 rel_tx_acked;

/* Reliable RX reassembly */
static guint8 rel_rx_buf[12 + 232];
static gsize rel_rx_len;
static gsize rel_rx_got;
static guint16 rel_rx_need;
static guint32 rel_rx_seq;

/* Management RX reassembly */
static guint8 mgmt_rx_buf[12 + MGMT_MAX_PAYLOAD];
static gsize mgmt_rx_len;
static gsize mgmt_rx_got;
static guint16 mgmt_rx_need;
static guint32 mgmt_rx_reqid;

/* Management responses to send (indications) */
struct mgmt_rsp {
	guint32 reqid;
	guint16 flags;
	guint8 type;
	GByteArray *payload;
};
static GQueue mgmt_q = G_QUEUE_INIT;
static GByteArray *mgmt_cur;
static gsize mgmt_off;
static gint64 mgmt_next_us;

/* Reliable TX frame in progress */
static GByteArray *rel_cur;
static gsize rel_off;
static gint64 rel_next_us;

/* Serial data received from the daemon, waiting for a BLE transport */
static GByteArray *serial_in;
static gint64 nus_next_us;

/* Unix socket */
static int sock_fd = -1;
static guint sock_watch;
static GByteArray *sock_rx;

static const char *char_iface = "org.bluez.GattCharacteristic1";

static void logmsg(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

/* ------------------------------------------------------------------ */
/* D-Bus helpers                                                       */
/* ------------------------------------------------------------------ */

static void emit_value(const char *path, const guint8 *data, gsize len)
{
	GVariantBuilder b;
	GVariant *value;

	value = g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, data, len, 1);
	g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
	g_variant_builder_add(&b, "{sv}", "Value", value);
	g_dbus_connection_emit_signal(bus, NULL, path,
				      "org.freedesktop.DBus.Properties",
				      "PropertiesChanged",
				      g_variant_new("(sa{sv}as)", char_iface, &b,
						    NULL),
				      NULL);
}

static void set_adapter_prop(const char *name, GVariant *value)
{
	GError *err = NULL;
	GVariant *r;

	r = g_dbus_connection_call_sync(
		bus, "org.bluez", "/org/bluez/hci0",
		"org.freedesktop.DBus.Properties", "Set",
		g_variant_new("(ssv)", "org.bluez.Adapter1", name, value),
		NULL, G_DBUS_CALL_FLAGS_NONE, 3000, NULL, &err);
	if (!r) {
		logmsg("adapter set %s failed: %s", name, err->message);
		g_error_free(err);
		return;
	}
	g_variant_unref(r);
}

/* ------------------------------------------------------------------ */
/* GATT service/characteristic objects                                 */
/* ------------------------------------------------------------------ */

static const char svc_xml[] =
	"<node><interface name=\"org.bluez.GattService1\">"
	"<property name=\"UUID\" type=\"s\" access=\"read\"/>"
	"<property name=\"Primary\" type=\"b\" access=\"read\"/>"
	"<property name=\"Includes\" type=\"ao\" access=\"read\"/>"
	"</interface></node>";

static const char chr_xml[] =
	"<node><interface name=\"org.bluez.GattCharacteristic1\">"
	"<method name=\"ReadValue\">"
	"<arg name=\"options\" type=\"a{sv}\" direction=\"in\"/>"
	"<arg name=\"value\" type=\"ay\" direction=\"out\"/></method>"
	"<method name=\"WriteValue\">"
	"<arg name=\"value\" type=\"ay\" direction=\"in\"/>"
	"<arg name=\"options\" type=\"a{sv}\" direction=\"in\"/></method>"
	"<method name=\"StartNotify\"/>"
	"<method name=\"StopNotify\"/>"
	"<property name=\"UUID\" type=\"s\" access=\"read\"/>"
	"<property name=\"Service\" type=\"o\" access=\"read\"/>"
	"<property name=\"Flags\" type=\"as\" access=\"read\"/>"
	"<property name=\"Notifying\" type=\"b\" access=\"read\"/>"
	"<property name=\"Value\" type=\"ay\" access=\"read\"/>"
	"</interface></node>";

static const char om_xml[] =
	"<node><interface name=\"org.freedesktop.DBus.ObjectManager\">"
	"<method name=\"GetManagedObjects\">"
	"<arg name=\"objects\" type=\"a{oa{sa{sv}}}\" direction=\"out\"/>"
	"</method></interface></node>";

static const char adv_xml[] =
	"<node><interface name=\"org.bluez.LEAdvertisement1\">"
	"<method name=\"Release\"/>"
	"<property name=\"Type\" type=\"s\" access=\"read\"/>"
	"<property name=\"ServiceUUIDs\" type=\"as\" access=\"read\"/>"
	"<property name=\"LocalName\" type=\"s\" access=\"read\"/>"
	"<property name=\"Includes\" type=\"as\" access=\"read\"/>"
	"<property name=\"ManufacturerData\" type=\"a{qv}\" access=\"read\"/>"
	"</interface></node>";

/* BlueZ pairing agent. With the "NoInputNoOutput" capability BlueZ only
 * calls RequestAuthorization/RequestConfirmation for Just Works pairing;
 * accepting them unconditionally is what lets an unpaired phone or browser
 * complete SMP instead of being rejected. The physical pairing-key gate
 * used by the upstream Zephyr firmware is added in stage 1. */
static const char agent_xml[] =
	"<node><interface name=\"org.bluez.Agent1\">"
	"<method name=\"Release\"/>"
	"<method name=\"RequestPinCode\">"
	"<arg name=\"device\" type=\"o\" direction=\"in\"/>"
	"<arg name=\"pincode\" type=\"s\" direction=\"out\"/></method>"
	"<method name=\"DisplayPinCode\">"
	"<arg name=\"device\" type=\"o\" direction=\"in\"/>"
	"<arg name=\"pincode\" type=\"s\" direction=\"in\"/></method>"
	"<method name=\"RequestPasskey\">"
	"<arg name=\"device\" type=\"o\" direction=\"in\"/>"
	"<arg name=\"passkey\" type=\"u\" direction=\"out\"/></method>"
	"<method name=\"DisplayPasskey\">"
	"<arg name=\"device\" type=\"o\" direction=\"in\"/>"
	"<arg name=\"passkey\" type=\"u\" direction=\"in\"/>"
	"<arg name=\"entered\" type=\"q\" direction=\"in\"/></method>"
	"<method name=\"RequestConfirmation\">"
	"<arg name=\"device\" type=\"o\" direction=\"in\"/>"
	"<arg name=\"passkey\" type=\"u\" direction=\"in\"/></method>"
	"<method name=\"RequestAuthorization\">"
	"<arg name=\"device\" type=\"o\" direction=\"in\"/></method>"
	"<method name=\"AuthorizeService\">"
	"<arg name=\"device\" type=\"o\" direction=\"in\"/>"
	"<arg name=\"uuid\" type=\"s\" direction=\"in\"/></method>"
	"<method name=\"Cancel\"/>"
	"</interface></node>";

static GVariant *svc_get_property(GDBusConnection *conn, const char *sender,
				  const char *path, const char *iface,
				  const char *prop, GError **err,
				  gpointer user_data)
{
	const struct svc_def *svc = user_data;

	(void)conn;
	(void)sender;
	(void)iface;
	(void)err;

	if (!strcmp(prop, "UUID")) {
		return g_variant_new_string(svc->uuid);
	}
	if (!strcmp(prop, "Primary")) {
		return g_variant_new_boolean(TRUE);
	}
	if (!strcmp(prop, "Includes")) {
		return g_variant_new_strv(NULL, 0);
	}
	return NULL;
}

static void append_mgmt_msg(uint8_t type, guint32 reqid, guint16 flags,
			    const char *text)
{
	struct mgmt_rsp *rsp;
	gsize len = strlen(text);

	rsp = g_new0(struct mgmt_rsp, 1);
	rsp->reqid = reqid;
	rsp->flags = flags;
	rsp->type = type;
	rsp->payload = g_byte_array_new();
	g_byte_array_append(rsp->payload, (const guint8 *)text, len);
	g_queue_push_tail(&mgmt_q, rsp);
}

static void append_mgmt_rsp(guint32 reqid, const char *text)
{
	guint16 flags = 0x0001; /* FINAL */

	if (!strncmp(text, "ERR ", 4)) {
		flags |= 0x0002; /* ERROR */
	}
	append_mgmt_msg(2, reqid, flags, text);
}

static void append_mgmt_event(guint32 reqid, guint16 flags, const char *text)
{
	append_mgmt_msg(3, reqid, flags | 0x0004 /* ASYNC */, text);
}

static gboolean write_all(int fd, const void *buf, gsize len)
{
	const guint8 *p = buf;
	gsize off = 0;

	while (off < len) {
		ssize_t n = write(fd, p + off, len - off);

		if (n < 0 && errno == EINTR) {
			continue;
		}
		if (n <= 0) {
			return FALSE;
		}
		off += n;
	}
	return TRUE;
}

static void send_frame(int fd, guint8 type, const void *data, gsize len)
{
	guint8 hdr[3];

	if (len > 0xffff) {
		logmsg("frame too large (%zu), dropped", len);
		return;
	}
	hdr[0] = type;
	hdr[1] = len & 0xff;
	hdr[2] = (len >> 8) & 0xff;
	if (!write_all(fd, hdr, sizeof(hdr))) {
		return;
	}
	(void)write_all(fd, data, len);
}

static void mgmt_dispatch(const guint8 *payload, gsize len)
{
	/* Forward the command text to the daemon; it answers asynchronously. */
	if (sock_fd < 0) {
		logmsg("mgmt command dropped: daemon not connected");
		return;
	}
	guint8 *buf = g_malloc(4 + len);
	buf[0] = mgmt_rx_reqid & 0xff;
	buf[1] = (mgmt_rx_reqid >> 8) & 0xff;
	buf[2] = (mgmt_rx_reqid >> 16) & 0xff;
	buf[3] = (mgmt_rx_reqid >> 24) & 0xff;
	memcpy(buf + 4, payload, len);
	send_frame(sock_fd, FRAME_CTL_REQ, buf, 4 + len);
	g_free(buf);
}

static void mgmt_write(const guint8 *data, gsize len)
{
	while (len > 0) {
		gsize take;

		if (mgmt_rx_len == 0) {
			/* The first fragment must carry the complete 12-byte
			 * header; BLE writes are at least 20 bytes, so this is
			 * not a practical limitation. */
			if (len < 12) {
				return;
			}
			if (data[0] != 'L' || data[1] != 'K' || data[2] != 1 ||
			    data[3] != 1) {
				return;
			}
			mgmt_rx_reqid = data[4] | (data[5] << 8) |
					(data[6] << 16) | (data[7] << 24);
			mgmt_rx_need = data[8] | (data[9] << 8);
			if (mgmt_rx_need == 0 ||
			    mgmt_rx_need > MGMT_MAX_PAYLOAD ||
			    mgmt_rx_reqid == 0) {
				return;
			}
			mgmt_rx_got = 0;
			mgmt_rx_len = 12;
			memcpy(mgmt_rx_buf, data, 12);
			data += 12;
			len -= 12;
			continue;
		}

		take = MIN(len, (gsize)(mgmt_rx_need - mgmt_rx_got));
		if (mgmt_rx_len + take > sizeof(mgmt_rx_buf)) {
			mgmt_rx_len = 0;
			mgmt_rx_got = 0;
			return;
		}
		memcpy(mgmt_rx_buf + mgmt_rx_len, data, take);
		mgmt_rx_len += take;
		mgmt_rx_got += take;
		data += take;
		len -= take;
		if (mgmt_rx_got >= mgmt_rx_need) {
			mgmt_dispatch(mgmt_rx_buf + 12, mgmt_rx_need);
			mgmt_rx_len = 0;
			mgmt_rx_got = 0;
		}
	}
}

static void rel_rx_write(const guint8 *data, gsize len)
{
	while (len > 0) {
		gsize take;

		if (rel_rx_len == 0) {
			if (len < 12) {
				return;
			}
			if (data[0] != 'L' || data[1] != 'R' ||
			    data[2] != 1) {
				return;
			}
			rel_rx_seq = data[4] | (data[5] << 8) |
				     (data[6] << 16) | (data[7] << 24);
			rel_rx_need = data[8] | (data[9] << 8);
			if (rel_rx_need == 0 || rel_rx_need > 232) {
				return;
			}
			rel_rx_got = 0;
			rel_rx_len = 12;
			memcpy(rel_rx_buf, data, 12);
			data += 12;
			len -= 12;
			continue;
		}

		take = MIN(len, (gsize)(rel_rx_need - rel_rx_got));
		if (rel_rx_len + take > sizeof(rel_rx_buf)) {
			rel_rx_len = 0;
			rel_rx_got = 0;
			return;
		}
		memcpy(rel_rx_buf + rel_rx_len, data, take);
		rel_rx_len += take;
		rel_rx_got += take;
		data += take;
		len -= take;
		if (rel_rx_got >= rel_rx_need) {
			guint32 prev = rel_rx_expected - 1;

			if (rel_rx_seq == rel_rx_expected) {
				if (sock_fd >= 0) {
					send_frame(sock_fd, FRAME_DATA,
						   rel_rx_buf + 12, rel_rx_need);
				}
				rel_rx_expected++;
				if (rel_rx_expected == 0) {
					rel_rx_expected = 1;
				}
			} else if (rel_rx_seq != prev) {
				logmsg("reliable RX sequence gap: expected %u "
				       "got %u", rel_rx_expected, rel_rx_seq);
			}
			rel_rx_len = 0;
			rel_rx_got = 0;
		}
	}
}

static GVariant *char_read(enum ckind kind, gsize *len)
{
	guint8 state[16];

	switch (kind) {
	case CK_MGMT_PROTO:
		*len = sizeof(proto_info);
		return g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE,
						 proto_info, sizeof(proto_info),
						 1);
	case CK_MGMT_ID:
		*len = sizeof(device_id);
		return g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE,
						 device_id, sizeof(device_id), 1);
	case CK_REL_STATE:
		state[0] = 1;
		state[1] = 1; /* reliable selected */
		state[2] = 232 & 0xff;
		state[3] = 232 >> 8;
		memcpy(state + 4, &rel_rx_expected, 4);
		memcpy(state + 8, &rel_tx_next, 4);
		memcpy(state + 12, &rel_tx_acked, 4);
		*len = sizeof(state);
		return g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, state,
						 sizeof(state), 1);
	default:
		*len = 0;
		return g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, NULL, 0,
						 1);
	}
}

static void char_method_call(GDBusConnection *conn, const char *sender,
			     const char *path, const char *iface,
			     const char *method, GVariant *params,
			     GDBusMethodInvocation *inv, gpointer user_data)
{
	const struct chr_def *chr = user_data;

	(void)conn;
	(void)sender;
	(void)path;
	(void)iface;

	if (!strcmp(method, "ReadValue")) {
		gsize len;
		GVariant *value = char_read(chr->kind, &len);

		g_dbus_method_invocation_return_value(
			inv, g_variant_new("(@ay)", value));
		return;
	}
	if (!strcmp(method, "WriteValue")) {
		GVariant *v = g_variant_get_child_value(params, 0);
		gsize len = 0;
		const guint8 *data = g_variant_get_fixed_array(v, &len, 1);

		if (data && len) {
			switch (chr->kind) {
			case CK_NUS_RX:
				if (sock_fd >= 0) {
					send_frame(sock_fd, FRAME_DATA, data,
						   len);
				}
				break;
			case CK_MGMT_CMD:
				mgmt_write(data, len);
				break;
			case CK_REL_RX:
				rel_rx_write(data, len);
				break;
			default:
				break;
			}
		}
		g_variant_unref(v);
		g_dbus_method_invocation_return_value(inv, NULL);
		return;
	}
	if (!strcmp(method, "StartNotify")) {
		notifying[chr->kind] = TRUE;
		/* A new NUS subscriber wants the live console, not whatever was
		 * received while nobody was listening.  Without this, connecting
		 * a phone or bee-cli dumps up to 32 KiB of stale serial text. */
		if (chr->kind == CK_NUS_TX && serial_in->len) {
			logmsg("dropping %zu stale serial bytes on NUS subscribe",
			       serial_in->len);
			g_byte_array_set_size(serial_in, 0);
		}
		logmsg("StartNotify %s", chr->path);
		g_dbus_method_invocation_return_value(inv, NULL);
		return;
	}
	if (!strcmp(method, "StopNotify")) {
		notifying[chr->kind] = FALSE;
		logmsg("StopNotify %s", chr->path);
		g_dbus_method_invocation_return_value(inv, NULL);
		return;
	}
	g_dbus_method_invocation_return_dbus_error(inv,
						   "org.bluez.Error.NotSupported",
						   "unsupported method");
}

static GVariant *char_get_property(GDBusConnection *conn, const char *sender,
				   const char *path, const char *iface,
				   const char *prop, GError **err,
				   gpointer user_data)
{
	const struct chr_def *chr = user_data;

	(void)conn;
	(void)sender;
	(void)path;
	(void)iface;
	(void)err;

	if (!strcmp(prop, "UUID")) {
		return g_variant_new_string(chr->uuid);
	}
	if (!strcmp(prop, "Service")) {
		return g_variant_new_object_path(chr->svc);
	}
	if (!strcmp(prop, "Flags")) {
		return g_variant_new_strv(chr->flags, -1);
	}
	if (!strcmp(prop, "Notifying")) {
		return g_variant_new_boolean(notifying[chr->kind]);
	}
	if (!strcmp(prop, "Value")) {
		gsize len;
		GVariant *v = char_read(chr->kind, &len);

		(void)len;
		return v;
	}
	return NULL;
}

static const GDBusInterfaceVTable svc_vtable = {
	.method_call = NULL,
	.get_property = svc_get_property,
	.set_property = NULL,
};

static const GDBusInterfaceVTable chr_vtable = {
	.method_call = char_method_call,
	.get_property = char_get_property,
	.set_property = NULL,
};

static void om_method_call(GDBusConnection *conn, const char *sender,
			   const char *path, const char *iface,
			   const char *method, GVariant *params,
			   GDBusMethodInvocation *inv, gpointer user_data)
{
	GVariantBuilder objects, ifaces, props;
	int i;

	(void)conn;
	(void)sender;
	(void)path;
	(void)iface;
	(void)params;
	(void)user_data;

	if (strcmp(method, "GetManagedObjects")) {
		g_dbus_method_invocation_return_dbus_error(
			inv, "org.freedesktop.DBus.Error.UnknownMethod",
			"unknown method");
		return;
	}

	g_variant_builder_init(&objects, G_VARIANT_TYPE("a{oa{sa{sv}}}"));
	for (i = 0; i < 3; i++) {
		g_variant_builder_init(&ifaces, G_VARIANT_TYPE("a{sa{sv}}"));
		g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
		g_variant_builder_add(&props, "{sv}", "UUID",
				      g_variant_new_string(services[i].uuid));
		g_variant_builder_add(&props, "{sv}", "Primary",
				      g_variant_new_boolean(TRUE));
		g_variant_builder_add(&ifaces, "{sa{sv}}",
				      "org.bluez.GattService1", &props);
		g_variant_builder_add(&objects, "{oa{sa{sv}}}",
				      services[i].path, &ifaces);
	}
	for (i = 0; i < CK_COUNT; i++) {
		g_variant_builder_init(&ifaces, G_VARIANT_TYPE("a{sa{sv}}"));
		g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
		g_variant_builder_add(&props, "{sv}", "UUID",
				      g_variant_new_string(chars[i].uuid));
		g_variant_builder_add(&props, "{sv}", "Service",
				      g_variant_new_object_path(chars[i].svc));
		g_variant_builder_add(&props, "{sv}", "Flags",
				      g_variant_new_strv(chars[i].flags, -1));
		g_variant_builder_add(&ifaces, "{sa{sv}}",
				      "org.bluez.GattCharacteristic1", &props);
		g_variant_builder_add(&objects, "{oa{sa{sv}}}", chars[i].path,
				      &ifaces);
	}
	g_dbus_method_invocation_return_value(
		inv, g_variant_new("(a{oa{sa{sv}}})", &objects));
}

static const GDBusInterfaceVTable om_vtable = {
	.method_call = om_method_call,
	.get_property = NULL,
	.set_property = NULL,
};

/* ------------------------------------------------------------------ */
/* Advertisement                                                       */
/* ------------------------------------------------------------------ */

static void adv_method_call(GDBusConnection *conn, const char *sender,
			    const char *path, const char *iface,
			    const char *method, GVariant *params,
			    GDBusMethodInvocation *inv, gpointer user_data)
{
	(void)conn;
	(void)sender;
	(void)path;
	(void)iface;
	(void)params;
	(void)user_data;

	if (!strcmp(method, "Release")) {
		g_dbus_method_invocation_return_value(inv, NULL);
		return;
	}
	g_dbus_method_invocation_return_dbus_error(inv,
						   "org.bluez.Error.NotSupported",
						   "unsupported method");
}

static GVariant *adv_get_property(GDBusConnection *conn, const char *sender,
				  const char *path, const char *iface,
				  const char *prop, GError **err,
				  gpointer user_data)
{
	(void)conn;
	(void)sender;
	(void)path;
	(void)iface;
	(void)err;
	(void)user_data;

	if (!strcmp(prop, "Type")) {
		return g_variant_new_string("peripheral");
	}
	if (!strcmp(prop, "ServiceUUIDs")) {
		const char *uuids[] = { UUID_MGMT, NULL };

		return g_variant_new_strv(uuids, -1);
	}
	if (!strcmp(prop, "LocalName")) {
		return g_variant_new_string(adv_name);
	}
	if (!strcmp(prop, "Includes")) {
		/* LocalName already adds MGMT_ADV_FLAG_LOCAL_NAME; asking for
		 * "local-name" here makes BlueZ reject the advertisement. */
		return g_variant_new_strv(NULL, 0);
	}
	if (!strcmp(prop, "ManufacturerData")) {
		GVariantBuilder b;

		g_variant_builder_init(&b, G_VARIANT_TYPE("a{qv}"));
		return g_variant_builder_end(&b);
	}
	return NULL;
}

static const GDBusInterfaceVTable adv_vtable = {
	.method_call = adv_method_call,
	.get_property = adv_get_property,
	.set_property = NULL,
};

static void agent_method_call(GDBusConnection *conn, const char *sender,
			      const char *path, const char *iface,
			      const char *method, GVariant *params,
			      GDBusMethodInvocation *inv, gpointer user_data)
{
	(void)conn;
	(void)sender;
	(void)path;
	(void)iface;
	(void)params;
	(void)user_data;

	/* NoInputNoOutput: Just Works. Accept the pairing requests BlueZ
	 * raises, and supply dummy values for the two methods that have an
	 * out-argument (BlueZ will not call them for this capability, but a
	 * well-formed reply keeps the interface complete). */
	if (!strcmp(method, "RequestPinCode")) {
		g_dbus_method_invocation_return_value(inv,
			g_variant_new("(s)", "000000"));
		return;
	}
	if (!strcmp(method, "RequestPasskey")) {
		g_dbus_method_invocation_return_value(inv,
			g_variant_new("(u)", 0u));
		return;
	}
	/* Release, DisplayPinCode, DisplayPasskey, RequestConfirmation,
	 * RequestAuthorization, AuthorizeService, Cancel: no reply value. */
	g_dbus_method_invocation_return_value(inv, NULL);
}

static const GDBusInterfaceVTable agent_vtable = {
	.method_call = agent_method_call,
	.get_property = NULL,
	.set_property = NULL,
};

/* ------------------------------------------------------------------ */
/* Unix socket to the Python daemon                                    */
/* ------------------------------------------------------------------ */

static gboolean sock_connect_cb(gpointer data);
static void schedule_reconnect(void);
static void apply_new_name(const char *name, gsize len);

static gboolean sock_read_cb(gint fd, GIOCondition cond, gpointer data)
{
	guint8 buf[4096];
	ssize_t n;

	(void)data;

	if (cond & (G_IO_HUP | G_IO_ERR)) {
		goto drop;
	}
	n = read(fd, buf, sizeof(buf));
	if (n <= 0) {
		goto drop;
	}
	g_byte_array_append(sock_rx, buf, n);

	while (sock_rx->len >= 3) {
		guint8 type = sock_rx->data[0];
		gsize len = sock_rx->data[1] | (sock_rx->data[2] << 8);

		if (sock_rx->len < 3 + len) {
			break;
		}
		if (type == FRAME_DATA) {
			if (serial_in->len + len <= 32768) {
				g_byte_array_append(serial_in, sock_rx->data + 3,
						    len);
			} else {
				logmsg("serial queue full, dropping %zu bytes",
				       len);
			}
		} else if (type == FRAME_NAME) {
			apply_new_name((const char *)sock_rx->data + 3, len);
		} else if (type == FRAME_CTL_RSP && len >= 4) {
			guint32 reqid = sock_rx->data[3] |
					(sock_rx->data[4] << 8) |
					(sock_rx->data[5] << 16) |
					(sock_rx->data[6] << 24);
			gsize tlen = len - 4;
			char *text = g_malloc(tlen + 1);

			memcpy(text, sock_rx->data + 7, tlen);
			text[tlen] = 0;
			if (reqid != 0) {
				append_mgmt_rsp(reqid, text);
			}
			g_free(text);
		} else if (type == FRAME_EVENT && len >= 6) {
			guint32 reqid = sock_rx->data[3] |
					(sock_rx->data[4] << 8) |
					(sock_rx->data[5] << 16) |
					(sock_rx->data[6] << 24);
			guint16 flags = sock_rx->data[7] |
					(sock_rx->data[8] << 8);
			gsize tlen = len - 6;
			char *text = g_malloc(tlen + 1);

			memcpy(text, sock_rx->data + 9, tlen);
			text[tlen] = 0;
			append_mgmt_event(reqid, flags, text);
			g_free(text);
		}
		g_byte_array_remove_range(sock_rx, 0, 3 + len);
	}
	return G_SOURCE_CONTINUE;

drop:
	if (sock_watch) {
		g_source_remove(sock_watch);
		sock_watch = 0;
	}
	if (sock_fd >= 0) {
		close(sock_fd);
		sock_fd = -1;
	}
	logmsg("daemon socket closed, reconnecting");
	schedule_reconnect();
	return G_SOURCE_REMOVE;
}

static void schedule_reconnect(void)
{
	g_timeout_add_seconds(1, sock_connect_cb, NULL);
}

static gboolean sock_connect_cb(gpointer data)
{
	struct sockaddr_un sa;
	int fd;

	(void)data;

	if (sock_fd >= 0) {
		return G_SOURCE_REMOVE;
	}
	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		schedule_reconnect();
		return G_SOURCE_REMOVE;
	}
	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	strncpy(sa.sun_path, SOCK_PATH, sizeof(sa.sun_path) - 1);
	if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		close(fd);
		schedule_reconnect();
		return G_SOURCE_REMOVE;
	}
	sock_fd = fd;
	g_byte_array_set_size(sock_rx, 0);
	send_frame(fd, FRAME_HELLO, "linkr-ble", 9);
	sock_watch = g_unix_fd_add(fd, G_IO_IN | G_IO_HUP | G_IO_ERR,
				   sock_read_cb, NULL);
	logmsg("connected to %s", SOCK_PATH);
	return G_SOURCE_REMOVE;
}

/* ------------------------------------------------------------------ */
/* TX pacing                                                           */
/* ------------------------------------------------------------------ */

static void mgmt_send_tick(gint64 now)
{
	if (!notifying[CK_MGMT_RSP] || now < mgmt_next_us) {
		return;
	}
	if (!mgmt_cur) {
		struct mgmt_rsp *rsp;

		if (g_queue_is_empty(&mgmt_q)) {
			return;
		}
		rsp = g_queue_pop_head(&mgmt_q);
		mgmt_cur = g_byte_array_new();
		guint8 hdr[12] = { 'L', 'K', 1, rsp->type,
				   rsp->reqid & 0xff, (rsp->reqid >> 8) & 0xff,
				   (rsp->reqid >> 16) & 0xff,
				   (rsp->reqid >> 24) & 0xff,
				   rsp->payload->len & 0xff,
				   (rsp->payload->len >> 8) & 0xff,
				   rsp->flags & 0xff, (rsp->flags >> 8) & 0xff };
		g_byte_array_append(mgmt_cur, hdr, 12);
		g_byte_array_append(mgmt_cur, rsp->payload->data,
				    rsp->payload->len);
		g_byte_array_unref(rsp->payload);
		g_free(rsp);
		mgmt_off = 0;
	}
	gsize n = MIN((gsize)ATT_CHUNK, mgmt_cur->len - mgmt_off);

	emit_value(MGMT_RSP, mgmt_cur->data + mgmt_off, n);
	mgmt_off += n;
	if (mgmt_off >= mgmt_cur->len) {
		g_byte_array_unref(mgmt_cur);
		mgmt_cur = NULL;
		mgmt_next_us = now + 40000;
	} else {
		mgmt_next_us = now + 15000;
	}
}

static void rel_send_tick(gint64 now)
{
	if (!notifying[CK_REL_TX] || now < rel_next_us) {
		return;
	}
	if (!rel_cur) {
		if (!serial_in->len) {
			return;
		}
		gsize n = MIN((gsize)64, serial_in->len);
		guint8 hdr[12] = { 'L', 'R', 1, 0,
				   rel_tx_next & 0xff,
				   (rel_tx_next >> 8) & 0xff,
				   (rel_tx_next >> 16) & 0xff,
				   (rel_tx_next >> 24) & 0xff,
				   n & 0xff, (n >> 8) & 0xff, 0, 0 };

		rel_cur = g_byte_array_new();
		g_byte_array_append(rel_cur, hdr, 12);
		g_byte_array_append(rel_cur, serial_in->data, n);
		g_byte_array_remove_range(serial_in, 0, n);
		rel_off = 0;
	}
	gsize n = MIN((gsize)ATT_CHUNK, rel_cur->len - rel_off);

	emit_value(REL_TX, rel_cur->data + rel_off, n);
	rel_off += n;
	if (rel_off >= rel_cur->len) {
		g_byte_array_unref(rel_cur);
		rel_cur = NULL;
		rel_tx_acked = rel_tx_next;
		rel_tx_next++;
		if (rel_tx_next == 0) {
			rel_tx_next = 1;
		}
		rel_next_us = now + 30000;
	} else {
		rel_next_us = now + 10000;
	}
}

static void nus_send_tick(gint64 now)
{
	int i;

	if (notifying[CK_REL_TX] || !notifying[CK_NUS_TX] ||
	    now < nus_next_us) {
		return;
	}
	for (i = 0; i < 4 && serial_in->len; i++) {
		gsize n = MIN((gsize)ATT_CHUNK, serial_in->len);

		emit_value(NUS_TX, serial_in->data, n);
		g_byte_array_remove_range(serial_in, 0, n);
	}
	nus_next_us = now + 5000;
}

static gboolean tick_cb(gpointer data)
{
	gint64 now = g_get_monotonic_time();

	(void)data;

	mgmt_send_tick(now);
	rel_send_tick(now);
	nus_send_tick(now);
	return G_SOURCE_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* Setup                                                               */
/* ------------------------------------------------------------------ */

static void load_device_id(void)
{
	GError *err = NULL;
	GVariant *v, *inner;
	const char *addr = NULL;
	unsigned int mac[6] = { 0 };
	static const guint8 ns[10] = { 'L', 'I', 'N', 'K', 'R',
				       'B', 'L', 'E', 1, 0 };

	v = g_dbus_connection_call_sync(
		bus, "org.bluez", "/org/bluez/hci0",
		"org.freedesktop.DBus.Properties", "Get",
		g_variant_new("(ss)", "org.bluez.Adapter1", "Address"),
		G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 3000, NULL,
		&err);
	if (v) {
		g_variant_get(v, "(v)", &inner);
		addr = g_variant_get_string(inner, NULL);
		sscanf(addr, "%x:%x:%x:%x:%x:%x", &mac[0], &mac[1], &mac[2],
		       &mac[3], &mac[4], &mac[5]);
		g_variant_unref(inner);
		g_variant_unref(v);
	} else {
		logmsg("adapter address failed: %s", err->message);
		g_error_free(err);
	}
	memcpy(device_id, ns, sizeof(ns));
	for (int i = 0; i < 6; i++) {
		device_id[10 + i] = mac[5 - i];
	}
}

static gboolean gatt_registered;
static gboolean adv_registered;
static gboolean reg_in_flight;

static gboolean register_gatt(gpointer data);
static void register_adv(void);

static gboolean register_adv_timeout(gpointer data)
{
	(void)data;
	register_adv();
	return G_SOURCE_REMOVE;
}

/* The Allwinner 4.9 kernel comments out HCI_OP_LE_SET_ADV_DATA and
 * HCI_OP_LE_SET_SCAN_RSP_DATA in net/bluetooth/hci_request.c, so BlueZ's
 * advertising payload (service UUID + name) never reaches the XR829
 * controller: it transmits an empty LE advertisement. A Web Bluetooth
 * chooser filtering on the Management Service UUID then finds nothing.
 *
 * Work around it by writing the AD / scan-response data straight to the
 * controller with /usr/bin/ble-adv-data.py after every advertisement
 * registration (boot and name changes both land here). */
static void push_adv_data_to_controller(void)
{
	const char *cmd = "/usr/bin/python3 /usr/bin/ble-adv-data.py";
	GError *err = NULL;

	if (!g_spawn_command_line_async(cmd, &err)) {
		logmsg("adv data push failed: %s", err->message);
		g_error_free(err);
	}
}

static void register_adv_cb(GObject *source, GAsyncResult *res,
			    gpointer user_data)
{
	GError *err = NULL;
	GVariant *r;

	(void)user_data;
	r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &err);
	if (!r) {
		logmsg("RegisterAdvertisement failed: %s (retrying)",
		       err->message);
		if (strstr(err->message, "Already")) {
			adv_registered = TRUE;
			push_adv_data_to_controller();
		}
		g_error_free(err);
		if (!adv_registered) {
			g_timeout_add_seconds(2, register_adv_timeout, NULL);
		}
		return;
	}
	g_variant_unref(r);
	adv_registered = TRUE;
	push_adv_data_to_controller();
	logmsg("advertising as \"%s\"", adv_name);
}

static void register_adv(void)
{
	if (adv_registered) {
		return;
	}
	g_dbus_connection_call(
		bus, "org.bluez", "/org/bluez/hci0",
		"org.bluez.LEAdvertisingManager1", "RegisterAdvertisement",
		g_variant_new("(oa{sv})", ADV_PATH, NULL), NULL,
		G_DBUS_CALL_FLAGS_NONE, 5000, NULL, register_adv_cb, NULL);
}

/* ------------------------------------------------------------------ */
/* Custom advertised name                                              */
/* ------------------------------------------------------------------ */

/* Load the custom name persisted by the daemon. The daemon already
 * validates it; here we only reject NUL/control bytes and enforce the
 * 29-byte limit so a truncated UDISK write cannot inject a bad name. */
static void load_adv_name(void)
{
	FILE *f = fopen(NAME_FILE, "rb");
	char buf[BLE_NAME_MAX + 2];
	size_t n;

	if (!f) {
		return;
	}
	n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = 0;
	if (n == 0 || n > BLE_NAME_MAX) {
		return;
	}
	for (size_t i = 0; i < n; i++) {
		if ((unsigned char)buf[i] < 0x20 ||
		    (unsigned char)buf[i] == 0x7f) {
			return;
		}
	}
	memcpy(adv_name, buf, n + 1);
	logmsg("using custom advertised name \"%s\"", adv_name);
}

/* BlueZ caches LocalName when the advertisement is registered, so a name
 * change needs UnregisterAdvertisement + RegisterAdvertisement. Do both
 * asynchronously and only flip adv_registered after the new one is in. */
static void unregister_adv_done(GObject *source, GAsyncResult *res,
				gpointer user_data)
{
	GError *err = NULL;
	GVariant *r;

	(void)user_data;
	r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &err);
	if (!r) {
		logmsg("UnregisterAdvertisement failed: %s", err->message);
		g_error_free(err);
	} else {
		g_variant_unref(r);
	}
	register_adv();
}

static void apply_new_name(const char *name, gsize len)
{
	if (len == 0 || len > BLE_NAME_MAX) {
		logmsg("ignoring invalid advertised name length %zu", len);
		return;
	}
	for (gsize i = 0; i < len; i++) {
		if ((unsigned char)name[i] < 0x20 ||
		    (unsigned char)name[i] == 0x7f) {
			logmsg("ignoring advertised name with control byte");
			return;
		}
	}
	memcpy(adv_name, name, len);
	adv_name[len] = 0;

	set_adapter_prop("Alias", g_variant_new_string(adv_name));
	logmsg("advertised name changed to \"%s\"", adv_name);

	if (!adv_registered) {
		register_adv();
		return;
	}
	adv_registered = FALSE;
	g_dbus_connection_call(
		bus, "org.bluez", "/org/bluez/hci0",
		"org.bluez.LEAdvertisingManager1", "UnregisterAdvertisement",
		g_variant_new("(o)", ADV_PATH), NULL,
		G_DBUS_CALL_FLAGS_NONE, 5000, NULL, unregister_adv_done, NULL);
}

/* ------------------------------------------------------------------ */
/* Pairing agent registration                                          */
/* ------------------------------------------------------------------ */

static gboolean agent_registered;
static gboolean agent_reg_in_flight;

static void register_agent(void);
static void request_default_agent(void);

static gboolean register_agent_timeout(gpointer data)
{
	(void)data;
	register_agent();
	return G_SOURCE_REMOVE;
}

static void register_agent_cb(GObject *source, GAsyncResult *res,
			      gpointer user_data)
{
	GError *err = NULL;
	GVariant *r;

	(void)user_data;
	r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &err);
	agent_reg_in_flight = FALSE;
	if (!r) {
		logmsg("RegisterAgent failed: %s (retrying)", err->message);
		g_error_free(err);
		g_timeout_add_seconds(2, register_agent_timeout, NULL);
		return;
	}
	g_variant_unref(r);
	agent_registered = TRUE;
	logmsg("pairing agent registered (NoInputNoOutput)");
	/* Only now that the agent object is known to BlueZ, make it the
	 * default; requesting earlier races the async RegisterAgent. */
	request_default_agent();
}

static void register_agent(void)
{
	if (agent_registered || agent_reg_in_flight) {
		return;
	}
	agent_reg_in_flight = TRUE;
	/* Asynchronous: bluetoothd may call back into this process while the
	 * registration is pending. Capability NoInputNoOutput selects Just
	 * Works pairing, which is what the unmodified Linkr Bee clients use. */
	g_dbus_connection_call(
		bus, "org.bluez", "/org/bluez",
		"org.bluez.AgentManager1", "RegisterAgent",
		g_variant_new("(os)", AGENT_PATH, "NoInputNoOutput"), NULL,
		G_DBUS_CALL_FLAGS_NONE, 5000, NULL, register_agent_cb, NULL);
}

static void request_default_agent(void)
{
	GError *err = NULL;
	GVariant *r;

	r = g_dbus_connection_call_sync(
		bus, "org.bluez", "/org/bluez",
		"org.bluez.AgentManager1", "RequestDefaultAgent",
		g_variant_new("(o)", AGENT_PATH), NULL,
		G_DBUS_CALL_FLAGS_NONE, 3000, NULL, &err);
	if (!r) {
		logmsg("RequestDefaultAgent failed: %s", err->message);
		g_error_free(err);
		return;
	}
	g_variant_unref(r);
}

static void register_app_cb(GObject *source, GAsyncResult *res,
			    gpointer user_data)
{
	GError *err = NULL;
	GVariant *r;

	(void)user_data;
	reg_in_flight = FALSE;
	r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &err);
	if (!r) {
		logmsg("RegisterApplication failed: %s (retrying)",
		       err->message);
		if (strstr(err->message, "Already")) {
			gatt_registered = TRUE;
		}
		g_error_free(err);
		if (!gatt_registered) {
			g_timeout_add_seconds(2, register_gatt, NULL);
			return;
		}
	} else {
		g_variant_unref(r);
		gatt_registered = TRUE;
		logmsg("GATT application registered");
	}
	register_adv();
}

static gboolean register_gatt(gpointer data)
{
	(void)data;

	if (gatt_registered) {
		register_adv();
		return G_SOURCE_REMOVE;
	}
	if (reg_in_flight) {
		return G_SOURCE_REMOVE;
	}
	reg_in_flight = TRUE;
	/* Must be asynchronous: BlueZ calls GetManagedObjects back into this
	 * process while RegisterApplication is still pending. */
	g_dbus_connection_call(
		bus, "org.bluez", "/org/bluez/hci0",
		"org.bluez.GattManager1", "RegisterApplication",
		g_variant_new("(oa{sv})", APP_PATH, NULL), NULL,
		G_DBUS_CALL_FLAGS_NONE, 5000, NULL, register_app_cb, NULL);
	return G_SOURCE_REMOVE;
}

static void register_objects(void)
{
	GDBusNodeInfo *info;
	GError *err = NULL;

	info = g_dbus_node_info_new_for_xml(om_xml, &err);
	g_dbus_connection_register_object(bus, APP_PATH, info->interfaces[0],
					  &om_vtable, NULL, NULL, &err);
	g_dbus_node_info_unref(info);

	info = g_dbus_node_info_new_for_xml(svc_xml, &err);
	for (int i = 0; i < 3; i++) {
		g_dbus_connection_register_object(
			bus, services[i].path, info->interfaces[0],
			&svc_vtable, (gpointer)&services[i], NULL, &err);
	}
	g_dbus_node_info_unref(info);

	info = g_dbus_node_info_new_for_xml(chr_xml, &err);
	for (int i = 0; i < CK_COUNT; i++) {
		g_dbus_connection_register_object(
			bus, chars[i].path, info->interfaces[0], &chr_vtable,
			(gpointer)&chars[i], NULL, &err);
	}
	g_dbus_node_info_unref(info);

	info = g_dbus_node_info_new_for_xml(adv_xml, &err);
	g_dbus_connection_register_object(bus, ADV_PATH, info->interfaces[0],
					  &adv_vtable, NULL, NULL, &err);
	g_dbus_node_info_unref(info);

	info = g_dbus_node_info_new_for_xml(agent_xml, &err);
	g_dbus_connection_register_object(bus, AGENT_PATH, info->interfaces[0],
					  &agent_vtable, NULL, NULL, &err);
	g_dbus_node_info_unref(info);
}

int main(int argc, char **argv)
{
	GError *err = NULL;

	(void)argc;
	(void)argv;

	/* A client disappearing between two writes must not terminate us. */
	signal(SIGPIPE, SIG_IGN);

	proto_info[0] = 1;
	proto_info[1] = 0;
	proto_info[2] = MGMT_MAX_PAYLOAD & 0xff;
	proto_info[3] = MGMT_MAX_PAYLOAD >> 8;
	proto_info[4] = MGMT_CAPS & 0xff;
	proto_info[5] = (MGMT_CAPS >> 8) & 0xff;
	proto_info[6] = (MGMT_CAPS >> 16) & 0xff;
	proto_info[7] = (MGMT_CAPS >> 24) & 0xff;

	serial_in = g_byte_array_new();
	sock_rx = g_byte_array_new();

	bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &err);
	if (!bus) {
		logmsg("system bus: %s", err->message);
		return 1;
	}
	load_device_id();
	load_adv_name();
	register_objects();

	set_adapter_prop("Alias", g_variant_new_string(adv_name));
	set_adapter_prop("Powered", g_variant_new_boolean(TRUE));
	set_adapter_prop("Discoverable", g_variant_new_boolean(TRUE));
	/* Keep the adapter pairable/discoverable indefinitely. Without these,
	 * BlueZ 5.54 defaults DiscoverableTimeout to 180 s and Pairable to no,
	 * so after three minutes an unpaired host can see the advertisement but
	 * the SMP pairing it starts is rejected -- exactly the "visible but
	 * cannot connect" symptom. A USB/portable controller also needs the
	 * pairing agent registered below to answer BlueZ's auth callbacks. */
	set_adapter_prop("Pairable", g_variant_new_boolean(TRUE));
	set_adapter_prop("DiscoverableTimeout", g_variant_new_uint32(0));
	set_adapter_prop("PairableTimeout", g_variant_new_uint32(0));

	register_agent();

	/* bluetoothd may not have the adapter ready at boot; register_gatt()
	 * retries on its own until the GattManager1 interface exists. */
	register_gatt(NULL);

	sock_connect_cb(NULL);
	if (sock_fd < 0) {
		schedule_reconnect();
	}

	g_timeout_add(5, tick_cb, NULL);
	loop = g_main_loop_new(NULL, FALSE);
	g_main_loop_run(loop);
	return 0;
}
