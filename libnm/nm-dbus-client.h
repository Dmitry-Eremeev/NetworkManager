/*
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301 USA.
 *
 * Copyright 2019 Red Hat, Inc.
 */

#ifndef __NM_DBUS_CLIENT_H__
#define __NM_DBUS_CLIENT_H__

typedef struct _NMDBusClient NMDBusClient;

typedef struct {
	void (*name_owner_changed) (NMDBusClient *self,
	                            const char *new_owner,
	                            gpointer user_data);
} NMDBusClientCallbackTable;

NMDBusClient *nm_dbus_client_new (GDBusConnection *dbus_connection,
                                  const NMDBusClientCallbackTable *callback_table,
                                  gpointer callback_user_data);

void nm_dbus_client_free (NMDBusClient *self);

#endif /* __NM_DBUS_CLIENT_H__ */
