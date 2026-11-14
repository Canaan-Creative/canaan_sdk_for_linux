// SPDX-License-Identifier: MIT
/*
 * Copyright (c) 2026, Canaan Creative Co., Ltd.
 *
 * For the licensing terms, see the LICENSE file (MIT License).
 *
 * NO WARRANTY. The Software is provided "AS IS", without warranty of any kind, express or
 * implied, including but not limited to the warranties of merchantability, fitness for a
 * particular purpose and non-infringement. In no event shall the authors or copyright
 * holders be liable for any claim, damages or other liability arising from, out of or in
 * connection with the Software or the use or other dealings in the Software.
 */

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>

#include <gio/gio.h>

#include "ble_prov.h"
#include "ble_wr.h"
#include "net_log.h"
#include "net_util.h"

#define UUID_BASE "-0000-1000-8000-00805f9b34fb" /* Bluetooth base UUID suffix */
#define UUID_SVC  "0000ffff" UUID_BASE           /* provisioning GATT service UUID */
#define UUID_RD   "0000ffe1" UUID_BASE
#define UUID_WR   "0000ffe2" UUID_BASE
#define UUID_STAT "0000ffe3" UUID_BASE

/* GATT application (ObjectManager) object path */
#define APP_PATH  "/com/canaan/miner/gatt"
#define SVC_PATH  APP_PATH "/service0"
#define RD_PATH   SVC_PATH "/char0"
#define WR_PATH   SVC_PATH "/char1"
#define STAT_PATH SVC_PATH "/char2"
#define ADV_PATH  "/com/canaan/miner/advertisement0"

#define BLUEZ_INIT       "/etc/init.d/bluetooth"
#define BLUEZ_RESTART_US (3 * G_USEC_PER_SEC)  /* wait before starting a stopped bluetoothd */
#define BLUEZ_RETRY_US   (10 * G_USEC_PER_SEC) /* retry period while it stays stopped */
#define BLUEZ_START_MS   5000                  /* deadline of the init script, ms */

struct ble_device {
    atomic_int            is_run;                           /* 1 while the BLE server thread runs */
    char                  name[BLE_NAME_LEN];               /* advertised name and adapter alias */
    char                  data_status[BLE_DATA_STATUS_LEN]; /* status characteristic value */
    char                  data_rd[BLE_DATA_RD_LEN];         /* read characteristic value */
    ble_data_process_func data_process_cb;                  /* handler for a reassembled command */
    pthread_mutex_t       mutex;                            /* guards data_status and data_rd */
};

enum {
    KIND_RD,
    KIND_WR,
    KIND_STAT,
};

struct gatt_char {
    const char  *uuid;
    char *const *flags;                   /* GATT flags, NULL-terminated */
    int          kind;                    /* KIND_* */
    char         frozen[BLE_DATA_RD_LEN]; /* value snapshot taken at a zero-offset read */
    size_t       frozen_len;              /* bytes of frozen served, incl. NUL */
};

static void      char_method(GDBusConnection *c, const gchar *sender, const gchar *path,
             const gchar *iface, const gchar *method, GVariant *params, GDBusMethodInvocation *inv,
             gpointer ud);
static GVariant *char_get_prop(GDBusConnection *c, const gchar *sender, const gchar *path,
        const gchar *iface, const gchar *prop, GError **err, gpointer ud);
static GVariant *svc_get_prop(GDBusConnection *c, const gchar *sender, const gchar *path,
        const gchar *iface, const gchar *prop, GError **err, gpointer ud);
static void      om_method(GDBusConnection *c, const gchar *sender, const gchar *path,
             const gchar *iface, const gchar *method, GVariant *params, GDBusMethodInvocation *inv,
             gpointer ud);
static void      adv_method(GDBusConnection *c, const gchar *sender, const gchar *path,
             const gchar *iface, const gchar *method, GVariant *params, GDBusMethodInvocation *inv,
             gpointer ud);
static GVariant *adv_get_prop(GDBusConnection *c, const gchar *sender, const gchar *path,
        const gchar *iface, const gchar *prop, GError **err, gpointer ud);

static struct ble_device ble_dev;

static GDBusConnection *ble_conn;        /* system bus connection */
static GMainContext    *ble_ctx;         /* main context of the BLE server thread */
static char             g_adapter[128];  /* BlueZ adapter object path, empty when none */
static bool             have_adapter;    /* g_adapter holds a live adapter */
static bool app_registered, app_pending; /* GATT application registered / registration in flight */
static bool adv_registered, adv_pending; /* advertisement registered / registration in flight */
static bool adv_failing;                 /* last registration failed; its error is logged */
static int  adapter_open = -1;
static bool need_rediscover;         /* bluetoothd restarted; look the adapter up again */
static gint64        bluez_lost_at;  /* monotonic time org.bluez lost its owner, 0 while owned */
static gint64        bluez_retry_at; /* earliest time of the next bluetoothd start */
static struct ble_wr g_wr;

static atomic_int g_desired_adv; /* 1 when advertising is wanted */

/* parsed introspection data */
static GDBusNodeInfo *om_node, *svc_node, *char_ro_node, *char_wo_node, *adv_node;

static char *const flags_ro[] = { (char *)"read", NULL };
static char *const flags_wo[] = { (char *)"write", NULL };

static struct gatt_char char_rd   = { .uuid = UUID_RD, .flags = flags_ro, .kind = KIND_RD };
static struct gatt_char char_wr   = { .uuid = UUID_WR, .flags = flags_wo, .kind = KIND_WR };
static struct gatt_char char_stat = { .uuid = UUID_STAT, .flags = flags_ro, .kind = KIND_STAT };

static const char OM_XML[] = "<node><interface name='org.freedesktop.DBus.ObjectManager'>"
                             "<method name='GetManagedObjects'>"
                             "<arg type='a{oa{sa{sv}}}' name='objects' direction='out'/>"
                             "</method></interface></node>";

static const char SVC_XML[] = "<node><interface name='org.bluez.GattService1'>"
                              "<property name='UUID' type='s' access='read'/>"
                              "<property name='Primary' type='b' access='read'/>"
                              "</interface></node>";

static const char CHAR_RO_XML[] = "<node><interface name='org.bluez.GattCharacteristic1'>"
                                  "<method name='ReadValue'>"
                                  "<arg type='a{sv}' name='options' direction='in'/>"
                                  "<arg type='ay' name='value' direction='out'/></method>"
                                  "<property name='UUID' type='s' access='read'/>"
                                  "<property name='Service' type='o' access='read'/>"
                                  "<property name='Flags' type='as' access='read'/>"
                                  "</interface></node>";

static const char CHAR_WO_XML[] = "<node><interface name='org.bluez.GattCharacteristic1'>"
                                  "<method name='WriteValue'>"
                                  "<arg type='ay' name='value' direction='in'/>"
                                  "<arg type='a{sv}' name='options' direction='in'/></method>"
                                  "<property name='UUID' type='s' access='read'/>"
                                  "<property name='Service' type='o' access='read'/>"
                                  "<property name='Flags' type='as' access='read'/>"
                                  "</interface></node>";

static const char ADV_XML[] = "<node><interface name='org.bluez.LEAdvertisement1'>"
                              "<method name='Release'/>"
                              "<property name='Type' type='s' access='read'/>"
                              "<property name='ServiceUUIDs' type='as' access='read'/>"
                              "<property name='LocalName' type='s' access='read'/>"
                              "</interface></node>";

/* GattCharacteristic1 handlers */
static const GDBusInterfaceVTable char_vtable = { char_method, char_get_prop, NULL, { 0 } };

/* GattService1 handlers */
static const GDBusInterfaceVTable svc_vtable = { NULL, svc_get_prop, NULL, { 0 } };

/* ObjectManager handlers */
static const GDBusInterfaceVTable om_vtable = { om_method, NULL, NULL, { 0 } };

/* LEAdvertisement1 handlers */
static const GDBusInterfaceVTable adv_vtable = { adv_method, adv_get_prop, NULL, { 0 } };

static GVariant *strv_variant(char *const *v)
{
    GVariantBuilder b;

    g_variant_builder_init(&b, G_VARIANT_TYPE("as"));
    for (; *v; v++)
        g_variant_builder_add(&b, "s", *v);
    return g_variant_builder_end(&b);
}

static GVariant *empty_opts(void)
{
    return g_variant_new_array(G_VARIANT_TYPE("{sv}"), NULL, 0);
}

static GVariant *char_get_prop(GDBusConnection *c, const gchar *sender, const gchar *path,
        const gchar *iface, const gchar *prop, GError **err, gpointer ud)
{
    struct gatt_char *cc = ud;
    (void)c;
    (void)sender;
    (void)path;
    (void)iface;
    (void)err;

    if (!strcmp(prop, "UUID"))
        return g_variant_new_string(cc->uuid);
    if (!strcmp(prop, "Service"))
        return g_variant_new_object_path(SVC_PATH);
    if (!strcmp(prop, "Flags"))
        return strv_variant(cc->flags);
    return NULL;
}

static void char_read(struct gatt_char *cc, GVariant *params, GDBusMethodInvocation *inv)
{
    guint16   offset = 0;
    GVariant *opts   = NULL, *arr;

    g_variant_get(params, "(@a{sv})", &opts);
    if (opts) {
        g_variant_lookup(opts, "offset", "q", &offset);
        g_variant_unref(opts);
    }

    if (offset == 0) {
        pthread_mutex_lock(&ble_dev.mutex);
        if (cc->kind == KIND_STAT)
            memcpy(cc->frozen, ble_dev.data_status, sizeof(cc->frozen));
        else
            memcpy(cc->frozen, ble_dev.data_rd, sizeof(cc->frozen));
        pthread_mutex_unlock(&ble_dev.mutex);
        cc->frozen[sizeof(cc->frozen) - 1] = '\0';
        cc->frozen_len                     = strlen(cc->frozen) + 1;
    }

    if (offset > cc->frozen_len) {
        g_dbus_method_invocation_return_dbus_error(inv, "org.bluez.Error.InvalidOffset",
                "invalid offset");
        return;
    }

    arr = g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, cc->frozen + offset,
            cc->frozen_len - offset, 1);
    g_dbus_method_invocation_return_value(inv, g_variant_new("(@ay)", arr));
}

static void char_write(GVariant *params, GDBusMethodInvocation *inv)
{
    GVariant   *val = NULL, *opts = NULL;
    const void *data;
    gsize       len    = 0;
    guint16     offset = 0;
    int         cbrc   = 0, rc;

    g_variant_get(params, "(@ay@a{sv})", &val, &opts);
    if (opts) {
        g_variant_lookup(opts, "offset", "q", &offset);
        g_variant_unref(opts);
    }
    data = g_variant_get_fixed_array(val, &len, 1);

    if (!(offset + len)) {
        g_variant_unref(val);
        g_dbus_method_invocation_return_dbus_error(inv, "org.bluez.Error.InvalidValueLength",
                "zero length");
        return;
    }

    rc = ble_wr_feed(&g_wr, data, len);
    g_variant_unref(val);
    if (BLE_WR_OVERFLOW == rc) {
        if (ble_dev.data_process_cb)
            ble_dev.data_process_cb(NULL);
        g_dbus_method_invocation_return_dbus_error(inv, "org.bluez.Error.InvalidValueLength",
                "overflow");
        return;
    }

    if (BLE_WR_LINE == rc) {
        if (ble_dev.data_process_cb)
            cbrc = ble_dev.data_process_cb(g_wr.buf);
        ble_wr_reset(&g_wr);
        if (cbrc) {
            g_dbus_method_invocation_return_dbus_error(inv, "org.bluez.Error.NotSupported",
                    "rejected");
            return;
        }
    }

    g_dbus_method_invocation_return_value(inv, NULL);
}

static void char_method(GDBusConnection *c, const gchar *sender, const gchar *path,
        const gchar *iface, const gchar *method, GVariant *params, GDBusMethodInvocation *inv,
        gpointer ud)
{
    (void)c;
    (void)sender;
    (void)path;
    (void)iface;

    if (!strcmp(method, "ReadValue"))
        char_read(ud, params, inv);
    else if (!strcmp(method, "WriteValue"))
        char_write(params, inv);
    else
        g_dbus_method_invocation_return_dbus_error(inv, "org.bluez.Error.NotSupported",
                "no method");
}

static GVariant *svc_get_prop(GDBusConnection *c, const gchar *sender, const gchar *path,
        const gchar *iface, const gchar *prop, GError **err, gpointer ud)
{
    (void)c;
    (void)sender;
    (void)path;
    (void)iface;
    (void)err;
    (void)ud;

    if (!strcmp(prop, "UUID"))
        return g_variant_new_string(UUID_SVC);
    if (!strcmp(prop, "Primary"))
        return g_variant_new_boolean(TRUE);
    return NULL;
}

static GVariant *char_props(const char *uuid, char *const *flags)
{
    GVariantBuilder p;

    g_variant_builder_init(&p, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&p, "{sv}", "UUID", g_variant_new_string(uuid));
    g_variant_builder_add(&p, "{sv}", "Service", g_variant_new_object_path(SVC_PATH));
    g_variant_builder_add(&p, "{sv}", "Flags", strv_variant(flags));
    return g_variant_builder_end(&p);
}

static void add_object(GVariantBuilder *objs, const char *path, const char *iface, GVariant *props)
{
    GVariantBuilder ifaces;

    g_variant_builder_init(&ifaces, G_VARIANT_TYPE("a{sa{sv}}"));
    g_variant_builder_add(&ifaces, "{s@a{sv}}", iface, props);
    g_variant_builder_add(objs, "{o@a{sa{sv}}}", path, g_variant_builder_end(&ifaces));
}

static void om_method(GDBusConnection *c, const gchar *sender, const gchar *path,
        const gchar *iface, const gchar *method, GVariant *params, GDBusMethodInvocation *inv,
        gpointer ud)
{
    GVariantBuilder objs, svc_props;

    (void)c;
    (void)sender;
    (void)path;
    (void)iface;
    (void)params;
    (void)ud;

    if (strcmp(method, "GetManagedObjects")) {
        g_dbus_method_invocation_return_dbus_error(inv, "org.freedesktop.DBus.Error.UnknownMethod",
                "no method");
        return;
    }

    g_variant_builder_init(&objs, G_VARIANT_TYPE("a{oa{sa{sv}}}"));

    g_variant_builder_init(&svc_props, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&svc_props, "{sv}", "UUID", g_variant_new_string(UUID_SVC));
    g_variant_builder_add(&svc_props, "{sv}", "Primary", g_variant_new_boolean(TRUE));
    add_object(&objs, SVC_PATH, "org.bluez.GattService1", g_variant_builder_end(&svc_props));

    add_object(&objs, RD_PATH, "org.bluez.GattCharacteristic1", char_props(UUID_RD, flags_ro));
    add_object(&objs, WR_PATH, "org.bluez.GattCharacteristic1", char_props(UUID_WR, flags_wo));
    add_object(&objs, STAT_PATH, "org.bluez.GattCharacteristic1", char_props(UUID_STAT, flags_ro));

    g_dbus_method_invocation_return_value(inv, g_variant_new("(a{oa{sa{sv}}})", &objs));
}

static void adv_method(GDBusConnection *c, const gchar *sender, const gchar *path,
        const gchar *iface, const gchar *method, GVariant *params, GDBusMethodInvocation *inv,
        gpointer ud)
{
    (void)c;
    (void)sender;
    (void)path;
    (void)iface;
    (void)params;
    (void)ud;

    if (!strcmp(method, "Release"))
        adv_registered = false;
    g_dbus_method_invocation_return_value(inv, NULL);
}

static GVariant *adv_get_prop(GDBusConnection *c, const gchar *sender, const gchar *path,
        const gchar *iface, const gchar *prop, GError **err, gpointer ud)
{
    char *const uuids[] = { (char *)UUID_SVC, NULL };
    (void)c;
    (void)sender;
    (void)path;
    (void)iface;
    (void)err;
    (void)ud;

    if (!strcmp(prop, "Type"))
        return g_variant_new_string("peripheral");
    if (!strcmp(prop, "ServiceUUIDs"))
        return strv_variant(uuids);
    if (!strcmp(prop, "LocalName"))
        return g_variant_new_string(ble_dev.name);
    return NULL;
}

static void on_generic_done(GObject *src, GAsyncResult *res, gpointer ud)
{
    GError   *err = NULL;
    GVariant *r   = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
    if (!r) {
        applog(LOG_ERR, "Bluetooth: %s failed: %s", (const char *)ud, err->message);
        g_error_free(err);
        return;
    }
    g_variant_unref(r);
}

static bool err_already_exists(const GError *err)
{
    gchar *rem    = g_dbus_error_get_remote_error(err);
    bool   exists = rem && !strcmp(rem, "org.bluez.Error.AlreadyExists");

    g_free(rem);
    return exists;
}

static void on_reg_app_done(GObject *src, GAsyncResult *res, gpointer ud)
{
    GError   *err = NULL;
    GVariant *r;

    (void)ud;
    app_pending = false;
    r           = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
    if (!r) {
        if (err_already_exists(err))
            app_registered = true;
        else
            applog(LOG_ERR, "Bluetooth: provisioning GATT service not registered: %s",
                    err->message);
        g_error_free(err);
        return;
    }
    g_variant_unref(r);
    app_registered = true;
    applog(LOG_INFO, "Bluetooth: provisioning GATT service registered");
}

static void on_reg_adv_done(GObject *src, GAsyncResult *res, gpointer ud)
{
    GError   *err = NULL;
    GVariant *r;

    (void)ud;
    adv_pending = false;
    r           = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
    if (!r) {
        if (err_already_exists(err)) {
            adv_registered = true;
            adv_failing    = false;
        } else if (!adv_failing) {
            adv_failing = true;
            applog(LOG_ERR, "Bluetooth: advertising could not start: %s", err->message);
        }
        g_error_free(err);
        return;
    }
    g_variant_unref(r);
    adv_registered = true;
    adv_failing    = false;
    applog(LOG_INFO, "Bluetooth: advertising as '%s'", ble_dev.name);
}

static void call_reg_obj(const char *iface, const char *member, const char *objpath,
        GAsyncReadyCallback cb, gpointer ud)
{
    g_dbus_connection_call(ble_conn, "org.bluez", g_adapter, iface, member,
            g_variant_new("(o@a{sv})", objpath, empty_opts()), NULL, G_DBUS_CALL_FLAGS_NONE, -1,
            NULL, cb, ud);
}

static void set_adapter_prop(const char *prop, GVariant *val, const char *what)
{
    g_dbus_connection_call(ble_conn, "org.bluez", g_adapter, "org.freedesktop.DBus.Properties",
            "Set", g_variant_new("(ssv)", "org.bluez.Adapter1", prop, val), NULL,
            G_DBUS_CALL_FLAGS_NONE, -1, NULL, on_generic_done, (gpointer)what);
}

static void unregister_obj(const char *iface, const char *member, const char *objpath)
{
    g_dbus_connection_call(ble_conn, "org.bluez", g_adapter, iface, member,
            g_variant_new("(o)", objpath), NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, on_generic_done,
            (gpointer)member);
}

static void drop_devices(bool live);

static void reconcile(void)
{
    int want;

    if (!ble_conn || !have_adapter)
        return;

    want = g_desired_adv ? 1 : 0;
    if (want != adapter_open) {
        adapter_open = want;
        adv_failing  = false;
        if (!want) {
            set_adapter_prop("Discoverable", g_variant_new_boolean(FALSE), "SetDiscoverable");
            /*
             * The controller cannot advertise connectable while it
             * holds a peripheral link, so a phone left connected
             * would block the next provisioning start.
             */
            drop_devices(true);
        }
    }

    if (want) {
        if (!app_registered && !app_pending) {
            app_pending = true;
            call_reg_obj("org.bluez.GattManager1", "RegisterApplication", APP_PATH, on_reg_app_done,
                    NULL);
        }
        if (app_registered && !adv_registered && !adv_pending) {
            adv_pending = true;
            call_reg_obj("org.bluez.LEAdvertisingManager1", "RegisterAdvertisement", ADV_PATH,
                    on_reg_adv_done, NULL);
        }
    } else {
        if (adv_registered && !adv_pending) {
            adv_registered = false;
            unregister_obj("org.bluez.LEAdvertisingManager1", "UnregisterAdvertisement", ADV_PATH);
        }
        if (app_registered && !app_pending) {
            app_registered = false;
            unregister_obj("org.bluez.GattManager1", "UnregisterApplication", APP_PATH);
        }
    }
}

static GVariant *get_managed_objects(GError **err)
{
    return g_dbus_connection_call_sync(ble_conn, "org.bluez", "/",
            "org.freedesktop.DBus.ObjectManager", "GetManagedObjects", NULL,
            G_VARIANT_TYPE("(a{oa{sa{sv}}})"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, err);
}

static void drop_devices(bool live)
{
    GError       *err = NULL;
    GVariant     *r, *ifaces, *dev;
    GVariantIter *it;
    const char   *opath;
    size_t        alen = strlen(g_adapter);

    r = get_managed_objects(&err);
    if (!r) {
        applog(LOG_ERR, "Bluetooth: could not list devices: %s", err->message);
        g_error_free(err);
        return;
    }

    g_variant_get(r, "(a{oa{sa{sv}}})", &it);
    while (g_variant_iter_loop(it, "{&o@a{sa{sv}}}", &opath, &ifaces)) {
        gboolean conn = FALSE;

        if (strncmp(opath, g_adapter, alen) || opath[alen] != '/')
            continue;
        dev = g_variant_lookup_value(ifaces, "org.bluez.Device1", NULL);
        if (!dev)
            continue;
        g_variant_lookup(dev, "Connected", "b", &conn);
        g_variant_unref(dev);
        if (conn != live)
            continue;

        if (live) {
            applog(LOG_INFO, "Bluetooth: disconnecting %s", opath);
            g_dbus_connection_call(ble_conn, "org.bluez", opath, "org.bluez.Device1", "Disconnect",
                    NULL, NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, on_generic_done,
                    (gpointer) "Disconnect");
            continue;
        }
        applog(LOG_INFO, "Bluetooth: forgetting stale pairing %s", opath);
        g_dbus_connection_call(ble_conn, "org.bluez", g_adapter, "org.bluez.Adapter1",
                "RemoveDevice", g_variant_new("(o)", opath), NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                on_generic_done, (gpointer) "RemoveDevice");
    }
    g_variant_iter_free(it);
    g_variant_unref(r);
}

static void adapter_found(const char *path)
{
    if (have_adapter)
        return;
    snprintf(g_adapter, sizeof(g_adapter), "%s", path);
    have_adapter   = true;
    app_registered = app_pending = false;
    adv_registered = adv_pending = false;
    applog(LOG_INFO, "Bluetooth: adapter %s found", g_adapter);
    drop_devices(false);
    set_adapter_prop("Powered", g_variant_new_boolean(TRUE), "SetPowered");
    set_adapter_prop("Alias", g_variant_new_string(ble_dev.name), "SetAlias");
    set_adapter_prop("Pairable", g_variant_new_boolean(FALSE), "SetPairable");
    adapter_open = -1;
    reconcile();
}

static void adapter_lost(void)
{
    have_adapter   = false;
    g_adapter[0]   = '\0';
    app_registered = app_pending = false;
    adv_registered = adv_pending = false;
}

static void find_adapter(void)
{
    GError       *err = NULL;
    GVariant     *r, *ifaces;
    GVariantIter *it;
    const char   *opath;

    if (have_adapter || !ble_conn)
        return;

    r = get_managed_objects(&err);
    if (!r) {
        applog(LOG_DEBUG, "Bluetooth: adapter query failed: %s", err->message);
        g_error_free(err);
        return;
    }

    g_variant_get(r, "(a{oa{sa{sv}}})", &it);
    while (g_variant_iter_loop(it, "{&o@a{sa{sv}}}", &opath, &ifaces)) {
        GVariant *ad = g_variant_lookup_value(ifaces, "org.bluez.Adapter1", NULL);
        if (ad) {
            g_variant_unref(ad);
            adapter_found(opath);
        }
    }
    g_variant_iter_free(it);
    g_variant_unref(r);
}

static void on_ifaces_added(GDBusConnection *c, const gchar *sender, const gchar *path,
        const gchar *iface, const gchar *signal, GVariant *params, gpointer ud)
{
    const char *opath;
    GVariant   *ifaces, *ad;

    (void)c;
    (void)sender;
    (void)path;
    (void)iface;
    (void)signal;
    (void)ud;
    g_variant_get(params, "(&o@a{sa{sv}})", &opath, &ifaces);
    ad = g_variant_lookup_value(ifaces, "org.bluez.Adapter1", NULL);
    if (ad) {
        g_variant_unref(ad);
        adapter_found(opath);
    }
    g_variant_unref(ifaces);
}

static void on_ifaces_removed(GDBusConnection *c, const gchar *sender, const gchar *path,
        const gchar *iface, const gchar *signal, GVariant *params, gpointer ud)
{
    const char   *opath, *s;
    GVariantIter *it;
    bool          adapter_gone = false, device_gone = false;

    (void)c;
    (void)sender;
    (void)path;
    (void)iface;
    (void)signal;
    (void)ud;
    g_variant_get(params, "(&oas)", &opath, &it);
    while (g_variant_iter_loop(it, "&s", &s)) {
        if (!strcmp(s, "org.bluez.Adapter1"))
            adapter_gone = true;
        else if (!strcmp(s, "org.bluez.Device1"))
            device_gone = true;
    }
    g_variant_iter_free(it);

    if (adapter_gone && have_adapter && !strcmp(opath, g_adapter)) {
        applog(LOG_WARNING, "Bluetooth: adapter removed; waiting for it to return");
        adapter_lost();
    } else if (device_gone) {
        ble_wr_reset(&g_wr);
        if (g_desired_adv)
            adv_registered = false;
    }
}

static void on_props_changed(GDBusConnection *c, const gchar *sender, const gchar *path,
        const gchar *iface, const gchar *signal, GVariant *params, gpointer ud)
{
    const char *ch_iface;
    GVariant   *changed, *inval;
    gboolean    conn;

    (void)c;
    (void)sender;
    (void)path;
    (void)iface;
    (void)signal;
    (void)ud;
    g_variant_get(params, "(&s@a{sv}@as)", &ch_iface, &changed, &inval);
    g_variant_unref(inval);

    if (!strcmp(ch_iface, "org.bluez.Device1") &&
            g_variant_lookup(changed, "Connected", "b", &conn)) {
        ble_wr_reset(&g_wr);
        if (!conn && g_desired_adv)
            adv_registered = false;
    }
    g_variant_unref(changed);
}

static void on_name_owner_changed(GDBusConnection *c, const gchar *sender, const gchar *path,
        const gchar *iface, const gchar *signal, GVariant *params, gpointer ud)
{
    const char *name, *old, *neu;

    (void)c;
    (void)sender;
    (void)path;
    (void)iface;
    (void)signal;
    (void)ud;
    g_variant_get(params, "(&s&s&s)", &name, &old, &neu);
    if (strcmp(name, "org.bluez"))
        return;

    adapter_lost();
    if (neu && neu[0]) {
        applog(LOG_INFO, "Bluetooth: service (bluetoothd) started; registering again");
        need_rediscover = true;
        bluez_lost_at   = 0;
    } else {
        applog(LOG_WARNING, "Bluetooth: service (bluetoothd) stopped; provisioning paused");
        bluez_lost_at = g_get_monotonic_time();
    }
}

static bool bluez_has_owner(void)
{
    GVariant *r;
    gboolean  owned = FALSE;

    r = g_dbus_connection_call_sync(ble_conn, "org.freedesktop.DBus", "/org/freedesktop/DBus",
            "org.freedesktop.DBus", "NameHasOwner", g_variant_new("(s)", "org.bluez"),
            G_VARIANT_TYPE("(b)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);
    if (!r)
        return true;
    g_variant_get(r, "(b)", &owned);
    g_variant_unref(r);
    return owned;
}

static void bluez_restart(gint64 now)
{
    const char *const argv[] = { BLUEZ_INIT, "start", NULL };
    char              out[128];

    if (!bluez_lost_at || now - bluez_lost_at < BLUEZ_RESTART_US || now < bluez_retry_at)
        return;
    bluez_retry_at = now + BLUEZ_RETRY_US;
    applog(LOG_WARNING, "Bluetooth: service (bluetoothd) not running; restarting it");
    if (exec_argv_timeout(argv, out, sizeof out, BLUEZ_START_MS) != 0)
        applog(LOG_ERR, "Bluetooth: service (bluetoothd) start failed");
}

static int register_object(const char *path, GDBusNodeInfo *node, const GDBusInterfaceVTable *vt,
        gpointer ud)
{
    GError *err = NULL;
    guint id = g_dbus_connection_register_object(ble_conn, path, node->interfaces[0], vt, ud, NULL,
            &err);
    if (!id) {
        applog(LOG_ERR, "Bluetooth: D-Bus object %s not registered: %s", path, err->message);
        g_error_free(err);
        return -1;
    }
    return 0;
}

static int bus_setup(void)
{
    GError *err = NULL;
    gchar  *addr;
    int     retry;

    ble_ctx = g_main_context_new();
    g_main_context_push_thread_default(ble_ctx);

    addr = g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SYSTEM, NULL, &err);
    if (!addr) {
        applog(LOG_ERR, "Bluetooth unavailable: D-Bus system bus not found: %s", err->message);
        g_error_free(err);
        return -1;
    }

    for (retry = 0; retry < 20; retry++) {
        ble_conn = g_dbus_connection_new_for_address_sync(addr,
                G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                        G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
                NULL, NULL, &err);
        if (ble_conn)
            break;
        g_clear_error(&err);
        usleep(250000);
    }
    g_free(addr);
    if (!ble_conn) {
        applog(LOG_ERR, "Bluetooth unavailable: cannot connect to the D-Bus system bus");
        return -1;
    }
    g_dbus_connection_set_exit_on_close(ble_conn, FALSE);

    om_node      = g_dbus_node_info_new_for_xml(OM_XML, NULL);
    svc_node     = g_dbus_node_info_new_for_xml(SVC_XML, NULL);
    char_ro_node = g_dbus_node_info_new_for_xml(CHAR_RO_XML, NULL);
    char_wo_node = g_dbus_node_info_new_for_xml(CHAR_WO_XML, NULL);
    adv_node     = g_dbus_node_info_new_for_xml(ADV_XML, NULL);
    if (!om_node || !svc_node || !char_ro_node || !char_wo_node || !adv_node) {
        applog(LOG_ERR, "Bluetooth unavailable: internal D-Bus description is invalid");
        return -1;
    }

    if (register_object(APP_PATH, om_node, &om_vtable, NULL) < 0)
        return -1;
    if (register_object(SVC_PATH, svc_node, &svc_vtable, NULL) < 0)
        return -1;
    if (register_object(RD_PATH, char_ro_node, &char_vtable, &char_rd) < 0)
        return -1;
    if (register_object(WR_PATH, char_wo_node, &char_vtable, &char_wr) < 0)
        return -1;
    if (register_object(STAT_PATH, char_ro_node, &char_vtable, &char_stat) < 0)
        return -1;
    if (register_object(ADV_PATH, adv_node, &adv_vtable, NULL) < 0)
        return -1;

    g_dbus_connection_signal_subscribe(ble_conn, "org.bluez", "org.freedesktop.DBus.ObjectManager",
            "InterfacesAdded", NULL, NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_ifaces_added, NULL, NULL);
    g_dbus_connection_signal_subscribe(ble_conn, "org.bluez", "org.freedesktop.DBus.ObjectManager",
            "InterfacesRemoved", NULL, NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_ifaces_removed, NULL,
            NULL);
    g_dbus_connection_signal_subscribe(ble_conn, "org.bluez", "org.freedesktop.DBus.Properties",
            "PropertiesChanged", NULL, NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_props_changed, NULL,
            NULL);
    g_dbus_connection_signal_subscribe(ble_conn, "org.freedesktop.DBus", "org.freedesktop.DBus",
            "NameOwnerChanged", "/org/freedesktop/DBus", "org.bluez", G_DBUS_SIGNAL_FLAGS_NONE,
            on_name_owner_changed, NULL, NULL);
    if (!bluez_has_owner())
        bluez_lost_at = g_get_monotonic_time();

    return 0;
}

static void bus_teardown(void)
{
    if (ble_conn) {
        g_dbus_connection_close_sync(ble_conn, NULL, NULL);
        g_object_unref(ble_conn);
        ble_conn = NULL;
    }
    g_clear_pointer(&om_node, g_dbus_node_info_unref);
    g_clear_pointer(&svc_node, g_dbus_node_info_unref);
    g_clear_pointer(&char_ro_node, g_dbus_node_info_unref);
    g_clear_pointer(&char_wo_node, g_dbus_node_info_unref);
    g_clear_pointer(&adv_node, g_dbus_node_info_unref);
    if (ble_ctx) {
        g_main_context_pop_thread_default(ble_ctx);
        g_main_context_unref(ble_ctx);
        ble_ctx = NULL;
    }
    have_adapter = false;
}

static gboolean on_tick(gpointer u)
{
    static unsigned ticks;

    (void)u;
    bluez_restart(g_get_monotonic_time());
    if (need_rediscover) {
        need_rediscover = false;
        find_adapter();
    }
    if (!have_adapter && (ticks % 10) == 0)
        find_adapter();
    ticks++;
    reconcile();
    return G_SOURCE_CONTINUE;
}

static void *ble_server_thread(void *arg)
{
    GSource   *tick;
    GMainLoop *loop;

    (void)arg;
    applog(LOG_INFO, "Bluetooth provisioning service started");

    if (bus_setup() < 0) {
        bus_teardown();
        atomic_store(&ble_dev.is_run, 0);
        return NULL;
    }

    find_adapter();

    loop = g_main_loop_new(ble_ctx, FALSE);
    tick = g_timeout_source_new(500);
    g_source_set_callback(tick, on_tick, NULL, NULL);
    g_source_attach(tick, ble_ctx);
    g_source_unref(tick);

    g_main_loop_run(loop);
    return NULL;
}

void ble_dev_init(char *name, ble_data_process_func data_process_cb)
{
    memset(&ble_dev, 0, sizeof(ble_dev));
    pthread_mutex_init(&ble_dev.mutex, NULL);
    strncpy(ble_dev.name, name, sizeof(ble_dev.name) - 1);
    ble_dev.data_process_cb = data_process_cb;
}

int ble_dev_startup(void)
{
    pthread_t tid;
    int       expected = 0;

    g_desired_adv = 1;
    if (!atomic_compare_exchange_strong(&ble_dev.is_run, &expected, 1))
        return 0;
    if (pthread_create(&tid, NULL, ble_server_thread, &ble_dev)) {
        atomic_store(&ble_dev.is_run, 0);
        applog(LOG_ERR, "Bluetooth provisioning unavailable: thread could not be started");
        return -1;
    }
    pthread_detach(tid);
    return 0;
}

int ble_dev_is_run(void)
{
    return atomic_load(&ble_dev.is_run);
}

void ble_dev_set_power(int onoff)
{
    g_desired_adv = onoff ? 1 : 0;
}

void ble_set_data_status(char *sta)
{
    pthread_mutex_lock(&ble_dev.mutex);
    strncpy(ble_dev.data_status, sta, sizeof(ble_dev.data_status) - 1);
    pthread_mutex_unlock(&ble_dev.mutex);
}

void ble_set_data(char *data)
{
    pthread_mutex_lock(&ble_dev.mutex);
    strncpy(ble_dev.data_rd, data, sizeof(ble_dev.data_rd) - 1);
    pthread_mutex_unlock(&ble_dev.mutex);
}
