/* -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/* NetworkManager -- Network link manager
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Copyright (C) 2008 Novell, Inc.
 * Copyright (C) 2008 Red Hat, Inc.
 */

#include "config.h"
#define ___CONFIG_H__

#include <string.h>
#include <pppd/pppd.h>
#include <pppd/fsm.h>
#include <pppd/ipcp.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <dlfcn.h>

#define INET6
#include <pppd/eui64.h>
#include <pppd/ipv6cp.h>

#include "nm-default.h"
#include "nm-dbus-interface.h"
#include "nm-pppd-plugin.h"
#include "nm-ppp-status.h"

int plugin_init (void);

char pppd_version[] = VERSION;

static GDBusProxy *proxy = NULL;

static void
nm_phasechange (void *data, int arg)
{
	NMPPPStatus ppp_status = NM_PPP_STATUS_UNKNOWN;
	char *ppp_phase;

	g_return_if_fail (G_IS_DBUS_PROXY (proxy));

	switch (arg) {
	case PHASE_DEAD:
		ppp_status = NM_PPP_STATUS_DEAD;
		ppp_phase = "dead";
		break;
	case PHASE_INITIALIZE:
		ppp_status = NM_PPP_STATUS_INITIALIZE;
		ppp_phase = "initialize";
		break;
	case PHASE_SERIALCONN:
		ppp_status = NM_PPP_STATUS_SERIALCONN;
		ppp_phase = "serial connection";
		break;
	case PHASE_DORMANT:
		ppp_status = NM_PPP_STATUS_DORMANT;
		ppp_phase = "dormant";
		break;
	case PHASE_ESTABLISH:
		ppp_status = NM_PPP_STATUS_ESTABLISH;
		ppp_phase = "establish";
		break;
	case PHASE_AUTHENTICATE:
		ppp_status = NM_PPP_STATUS_AUTHENTICATE;
		ppp_phase = "authenticate";
		break;
	case PHASE_CALLBACK:
		ppp_status = NM_PPP_STATUS_CALLBACK;
		ppp_phase = "callback";
		break;
	case PHASE_NETWORK:
		ppp_status = NM_PPP_STATUS_NETWORK;
		ppp_phase = "network";
		break;
	case PHASE_RUNNING:
		ppp_status = NM_PPP_STATUS_RUNNING;
		ppp_phase = "running";
		break;
	case PHASE_TERMINATE:
		ppp_status = NM_PPP_STATUS_TERMINATE;
		ppp_phase = "terminate";
		break;
	case PHASE_DISCONNECT:
		ppp_status = NM_PPP_STATUS_DISCONNECT;
		ppp_phase = "disconnect";
		break;
	case PHASE_HOLDOFF:
		ppp_status = NM_PPP_STATUS_HOLDOFF;
		ppp_phase = "holdoff";
		break;
	case PHASE_MASTER:
		ppp_status = NM_PPP_STATUS_MASTER;
		ppp_phase = "master";
		break;

	default:
		ppp_phase = "unknown";
		break;
	}

	g_message ("nm-ppp-plugin: (%s): status %d / phase '%s'",
	           __func__,
	           ppp_status,
	           ppp_phase);

	if (ppp_status != NM_PPP_STATUS_UNKNOWN) {
		g_dbus_proxy_call (proxy,
		                   "SetState",
		                   g_variant_new ("(u)", ppp_status),
		                   G_DBUS_CALL_FLAGS_NONE, -1,
		                   NULL,
		                   NULL, NULL);
	}
}

static void
nm_ip_up (void *data, int arg)
{
	ipcp_options opts = ipcp_gotoptions[0];
	ipcp_options peer_opts = ipcp_hisoptions[0];
	GVariantBuilder builder;
	guint32 pppd_made_up_address = htonl (0x0a404040 + ifunit);

	g_return_if_fail (G_IS_DBUS_PROXY (proxy));

	g_message ("nm-ppp-plugin: (%s): ip-up event", __func__);

	if (!opts.ouraddr) {
		g_warning ("nm-ppp-plugin: (%s): didn't receive an internal IP from pppd!", __func__);
		nm_phasechange (NULL, PHASE_DEAD);
		return;
	}

	g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);

	g_variant_builder_add (&builder, "{sv}",
	                       NM_PPP_IP4_CONFIG_INTERFACE,
	                       g_variant_new_string (ifname));

	g_variant_builder_add (&builder, "{sv}",
	                       NM_PPP_IP4_CONFIG_ADDRESS,
	                       g_variant_new_uint32 (opts.ouraddr));

	/* Prefer the peer options remote address first, _unless_ pppd made the
	 * address up, at which point prefer the local options remote address,
	 * and if that's not right, use the made-up address as a last resort.
	 */
	if (peer_opts.hisaddr && (peer_opts.hisaddr != pppd_made_up_address)) {
		g_variant_builder_add (&builder, "{sv}",
		                       NM_PPP_IP4_CONFIG_GATEWAY,
		                       g_variant_new_uint32 (peer_opts.hisaddr));
	} else if (opts.hisaddr) {
		g_variant_builder_add (&builder, "{sv}",
		                       NM_PPP_IP4_CONFIG_GATEWAY,
		                       g_variant_new_uint32 (opts.hisaddr));
	} else if (peer_opts.hisaddr == pppd_made_up_address) {
		/* As a last resort, use the made-up address */
		g_variant_builder_add (&builder, "{sv}",
		                       NM_PPP_IP4_CONFIG_GATEWAY,
		                       g_variant_new_uint32 (peer_opts.ouraddr));
	}

	g_variant_builder_add (&builder, "{sv}",
	                       NM_PPP_IP4_CONFIG_PREFIX,
	                       g_variant_new_uint32 (32));

	if (opts.dnsaddr[0] || opts.dnsaddr[1]) {
		guint32 dns[2];
		int len = 0;

		if (opts.dnsaddr[0])
			dns[len++] = opts.dnsaddr[0];
		if (opts.dnsaddr[1])
			dns[len++] = opts.dnsaddr[1];

		g_variant_builder_add (&builder, "{sv}",
		                       NM_PPP_IP4_CONFIG_DNS,
		                       g_variant_new_fixed_array (G_VARIANT_TYPE_UINT32,
		                                                  dns, len, sizeof (guint32)));
	}

	if (opts.winsaddr[0] || opts.winsaddr[1]) {
		guint32 wins[2];
		int len = 0;

		if (opts.winsaddr[0])
			wins[len++] = opts.winsaddr[0];
		if (opts.winsaddr[1])
			wins[len++] = opts.winsaddr[1];

		g_variant_builder_add (&builder, "{sv}",
		                       NM_PPP_IP4_CONFIG_WINS,
		                       g_variant_new_fixed_array (G_VARIANT_TYPE_UINT32,
		                                                  wins, len, sizeof (guint32)));
	}

	g_message ("nm-ppp-plugin: (%s): sending IPv4 config to NetworkManager...", __func__);

	g_dbus_proxy_call (proxy,
	                   "SetIp4Config",
	                   g_variant_new ("(a{sv})", &builder),
	                   G_DBUS_CALL_FLAGS_NONE, -1,
	                   NULL,
	                   NULL, NULL);
}

static GVariant *
eui64_to_variant (eui64_t eui)
{
	guint64 iid;

	G_STATIC_ASSERT (sizeof (iid) == sizeof (eui));

	memcpy (&iid, &eui, sizeof (eui));
	return g_variant_new_uint64 (iid);
}

static void
nm_ip6_up (void *data, int arg)
{
	ipv6cp_options *ho = &ipv6cp_hisoptions[0];
	ipv6cp_options *go = &ipv6cp_gotoptions[0];
	GVariantBuilder builder;

	g_return_if_fail (G_IS_DBUS_PROXY (proxy));

	g_message ("nm-ppp-plugin: (%s): ip6-up event", __func__);

	g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
	g_variant_builder_add (&builder, "{sv}",
	                       NM_PPP_IP6_CONFIG_INTERFACE,
	                       g_variant_new_string (ifname));
	g_variant_builder_add (&builder, "{sv}",
	                       NM_PPP_IP6_CONFIG_OUR_IID,
	                       eui64_to_variant (go->ourid));
	g_variant_builder_add (&builder, "{sv}",
	                       NM_PPP_IP6_CONFIG_PEER_IID,
	                       eui64_to_variant (ho->hisid));

	/* DNS is done via DHCPv6 or router advertisements */

	g_message ("nm-ppp-plugin: (%s): sending IPv6 config to NetworkManager...", __func__);

	g_dbus_proxy_call (proxy,
	                   "SetIp6Config",
	                   g_variant_new ("(a{sv})", &builder),
	                   G_DBUS_CALL_FLAGS_NONE, -1,
	                   NULL,
	                   NULL, NULL);
}

static int
get_chap_check (void)
{
	return 1;
}

static int
get_pap_check (void)
{
	return 1;
}

static int
get_credentials (char *username, char *password)
{
	const char *my_username = NULL;
	const char *my_password = NULL;
	size_t len;
	GVariant *ret;
	GError *err = NULL;

	if (!password) {
		/* pppd is checking pap support; return 1 for supported */
		g_return_val_if_fail (username, -1);
		return 1;
	}

	g_return_val_if_fail (username, -1);
	g_return_val_if_fail (G_IS_DBUS_PROXY (proxy), -1);

	g_message ("nm-ppp-plugin: (%s): passwd-hook, requesting credentials...", __func__);

	ret = g_dbus_proxy_call_sync (proxy,
	                              "NeedSecrets",
	                              NULL,
	                              G_DBUS_CALL_FLAGS_NONE, -1,
	                              NULL, &err);
	if (!ret) {
		g_warning ("nm-ppp-plugin: (%s): could not get secrets: (%d) %s",
		           __func__,
		           err->code,
		           err->message);
		g_error_free (err);
		return -1;
	}

	g_message ("nm-ppp-plugin: (%s): got credentials from NetworkManager", __func__);

	g_variant_get (ret, "(&s&s)", &my_username, &my_password);

	if (my_username) {
		len = strlen (my_username) + 1;
		len = len < MAXNAMELEN ? len : MAXNAMELEN;

		strncpy (username, my_username, len);
		username[len - 1] = '\0';
	}

	if (my_password) {
		len = strlen (my_password) + 1;
		len = len < MAXSECRETLEN ? len : MAXSECRETLEN;

		strncpy (password, my_password, len);
		password[len - 1] = '\0';
	}

	g_variant_unref (ret);

	return 1;
}

static void
nm_exit_notify (void *data, int arg)
{
	g_return_if_fail (G_IS_DBUS_PROXY (proxy));

	g_message ("nm-ppp-plugin: (%s): cleaning up", __func__);

	g_object_unref (proxy);
	proxy = NULL;
}

static void
add_ip6_notifier (void)
{
	static struct notifier **notifier = NULL;
	static gsize load_once = 0;

	if (g_once_init_enter (&load_once)) {
		void *handle = dlopen(NULL, RTLD_NOW | RTLD_GLOBAL);

		if (handle) {
			notifier = dlsym (handle, "ipv6_up_notifier");
			dlclose (handle);
		}
		g_once_init_leave (&load_once, 1);
	}
	if (notifier)
		add_notifier (notifier, nm_ip6_up, NULL);
	else
		g_message ("nm-ppp-plugin: no IPV6CP notifier support; IPv6 not available");
}

int
plugin_init (void)
{
	GDBusConnection *bus;
	GError *err = NULL;

	nm_g_type_init ();

	g_message ("nm-ppp-plugin: (%s): initializing", __func__);

	bus = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, &err);
	if (!bus) {
		g_warning ("nm-pppd-plugin: (%s): couldn't connect to system bus: %s",
		           __func__, err->message);
		g_error_free (err);
		return -1;
	}

	/* NM passes in the object path of the corresponding PPPManager
	 * object as the 'ipparam' argument to pppd.
	 */
	proxy = g_dbus_proxy_new_sync (bus,
	                               G_DBUS_PROXY_FLAGS_NONE,
	                               NULL,
	                               NM_DBUS_SERVICE,
	                               ipparam,
	                               NM_DBUS_INTERFACE_PPP,
	                               NULL, &err);
	g_object_unref (bus);

	if (!proxy) {
		g_warning ("nm-pppd-plugin: (%s): couldn't create D-Bus proxy: %s",
		           __func__, err->message);
		g_error_free (err);
		return -1;
	}

	chap_passwd_hook = get_credentials;
	chap_check_hook = get_chap_check;
	pap_passwd_hook = get_credentials;
	pap_check_hook = get_pap_check;

	add_notifier (&phasechange, nm_phasechange, NULL);
	add_notifier (&ip_up_notifier, nm_ip_up, NULL);
	add_notifier (&exitnotify, nm_exit_notify, proxy);
	add_ip6_notifier ();

	return 0;
}
