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

#include "nm-default.h"

#include "nm-dbus-client.h"

#include "c-list/src/c-list.h"
#include "nm-glib-aux/nm-dbus-aux.h"
#include "nm-glib-aux/nm-ref-string.h"
#include "nm-dbus-interface.h"

/*****************************************************************************/

typedef void (*EventHandleFcn) (NMDBusClient *self,
                                gpointer user_data);

typedef struct {
	CList event_lst;
	EventHandleFcn handle_fcn;
	gpointer user_data;
} EventData;

struct _NMDBusClient {

	const NMDBusClientCallbackTable *callback_table;
	gpointer callback_user_data;

	GMutex event_lock;

	CList event_lst_head;

	GMainContext *caller_context;
	GMainContext *intern_context;

	GDBusConnection *dbus_connection;

	GCancellable *i_name_owner_get_cancellable;

	char *i_name_owner;

	guint name_owner_changed_id;
};

/*****************************************************************************/

#define _LOGT(...) do { } while (0)

static gboolean
NM_IS_DBUS_CLIENT (NMDBusClient *self)
{
	nm_assert (!self || G_IS_DBUS_CONNECTION (self->dbus_connection));

	return !!self;
}

/*****************************************************************************/

static void
_event_queue (NMDBusClient *self,
              EventHandleFcn handle_fcn,
              gpointer user_data)
{
	EventData *e;

	e = g_slice_new (EventData);
	e->handle_fcn = handle_fcn;
	e->user_data = user_data;

	g_mutex_lock (&self->event_lock);
	c_list_link_tail (&self->event_lst_head, &e->event_lst);
	g_mutex_unlock (&self->event_lock);
}

static gboolean
_event_dequeue_and_handle (NMDBusClient *self)
{
	EventData *e;

	g_mutex_lock (&self->event_lock);
	e = c_list_first_entry (&self->event_lst_head, EventData, event_lst);
	if (e)
		c_list_unlink (&e->event_lst);
	g_mutex_unlock (&self->event_lock);

	if (!e)
		return FALSE;

	e->handle_fcn (self, e->user_data);
	nm_g_slice_free (e);
	return TRUE;
}

/*****************************************************************************/

static void
i_name_owner_changed (NMDBusClient *self,
                      const char *name_owner,
                      gboolean from_get_callback)
{
	if (G_UNLIKELY (self->i_name_owner_get_cancellable))
		g_clear_object (&self->i_name_owner_get_cancellable);
	else {
		if (nm_streq0 (self->i_name_owner, name_owner))
			return;
	}

	g_free (self->i_name_owner);
	self->i_name_owner = g_strdup (name_owner);

	if (!self->i_name_owner)
		_LOGT ("D-Bus name for %s has no owner", NM_DBUS_SERVICE);
	else
		_LOGT ("D-Bus name for %s has owner %s", NM_DBUS_SERVICE, i_name_owner);

	//XXX: initialize parts.

	//XXX: notify caller about name owner changed.
}

static void
i_name_owner_changed_cb (GDBusConnection *connection,
                         const char *sender_name,
                         const char *object_path,
                         const char *interface_name,
                         const char *signal_name,
                         GVariant *parameters,
                         gpointer user_data)
{
	const char *new_owner;

	if (g_variant_is_of_type (parameters, G_VARIANT_TYPE ("(sss)"))) {
		g_variant_get (parameters,
		               "(&s&s&s)",
		               NULL,
		               NULL,
		               &new_owner);
		i_name_owner_changed (user_data, nm_str_not_empty (new_owner), FALSE);
	}
}

static void
i_name_owner_get_cb (const char *name_owner,
                     GError *error,
                     gpointer user_data)
{
	if (   name_owner
	    || !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
		i_name_owner_changed (user_data, name_owner, TRUE);
}

/*****************************************************************************/

NMDBusClient *
nm_dbus_client_new (GDBusConnection *dbus_connection,
                    const NMDBusClientCallbackTable *callback_table,
                    gpointer callback_user_data)
{
	NMDBusClient *self;

	g_return_val_if_fail (G_IS_DBUS_CONNECTION (dbus_connection), NULL);
	g_return_val_if_fail (callback_table, NULL);

	self = g_slice_new (NMDBusClient);

	*self = (NMDBusClient) {
		.callback_table     = callback_table,
		.callback_user_data = callback_user_data,
		.dbus_connection    = g_object_ref (dbus_connection),
		.caller_context     = g_main_context_ref_thread_default (),
		.intern_context     = g_main_context_new (),
		.event_lst_head     = C_LIST_INIT (self->event_lst_head),
	};

	g_mutex_init (&self->event_lock);

	g_main_context_push_thread_default (self->intern_context);

	self->name_owner_changed_id = nm_dbus_connection_signal_subscribe_name_owner_changed (self->dbus_connection,
	                                                                                      NM_DBUS_SERVICE,
	                                                                                      i_name_owner_changed_cb,
	                                                                                      self,
	                                                                                      NULL);

	self->i_name_owner_get_cancellable = g_cancellable_new ();
	nm_dbus_connection_call_get_name_owner (self->dbus_connection,
	                                        NM_DBUS_SERVICE,
	                                        -1,
	                                        self->i_name_owner_get_cancellable,
	                                        i_name_owner_get_cb,
	                                        self);

	g_main_context_pop_thread_default (self->intern_context);

	return self;
}

void
nm_dbus_client_free (NMDBusClient *self)
{
	g_return_if_fail (NM_IS_DBUS_CLIENT (self));

	//XXX: handle pending events in event_lst_head
	nm_assert (c_list_is_empty (&self->event_lst_head));

	nm_clear_g_cancellable (&self->i_name_owner_get_cancellable);

	nm_clear_g_dbus_connection_signal (self->dbus_connection,
	                                   &self->name_owner_changed_id);

	g_object_unref (self->dbus_connection);

	g_main_context_unref (self->caller_context);

	//XXX: ensure there are no pending jobs (or we handle them correctly).
	g_main_context_unref (self->intern_context);

	g_mutex_clear (&self->event_lock);

	nm_g_slice_free (self);
}
