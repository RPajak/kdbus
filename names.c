/*
 * Copyright (C) 2013 Kay Sievers
 * Copyright (C) 2013 Greg Kroah-Hartman <gregkh@linuxfoundation.org>
 * Copyright (C) 2013 Daniel Mack <daniel@zonque.org>
 * Copyright (C) 2013 Linux Foundation
 *
 * kdbus is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

#include <linux/module.h>
#include <linux/device.h>
#include <linux/idr.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/mutex.h>
#include <linux/init.h>
#include <linux/hash.h>
#include <linux/uaccess.h>
#include <linux/ctype.h>

#include "names.h"
#include "connection.h"
#include "notify.h"
#include "policy.h"
#include "bus.h"
#include "endpoint.h"

/**
 * struct kdbus_name_queue_item - a queue item for a name
 * @conn:		The associated connection
 * @entry:		Name entry queuing up for
 * @flags:		The queuing flags
 * @entry_entry:	List element for the list in @entry
 * @conn_entry:		List element for the list in @conn
 */
struct kdbus_name_queue_item {
	struct kdbus_conn	*conn;
	struct kdbus_name_entry	*entry;
	u64			flags;
	struct list_head	entry_entry;
	struct list_head	conn_entry;
};

static void kdbus_name_entry_free(struct kdbus_name_entry *e)
{
	kdbus_conn_unref(e->conn);
	kdbus_conn_unref(e->starter);
	hash_del(&e->hentry);
	kfree(e->name);
	kfree(e);
}

static void __kdbus_name_registry_free(struct kref *kref)
{
	struct kdbus_name_registry *reg =
		container_of(kref, struct kdbus_name_registry, kref);
	struct kdbus_name_entry *e;
	struct hlist_node *tmp;
	unsigned int i;

	mutex_lock(&reg->entries_lock);
	hash_for_each_safe(reg->entries_hash, i, tmp, e, hentry)
		kdbus_name_entry_free(e);
	mutex_unlock(&reg->entries_lock);

	kfree(reg);
}

/**
 * kdbus_name_registry_unref() - drop a name reg's reference
 * @reg:		The name registry
 *
 * When the last reference is dropped, the name registry's internal structure
 * is freed.
 */
void kdbus_name_registry_unref(struct kdbus_name_registry *reg)
{
	kref_put(&reg->kref, __kdbus_name_registry_free);
}

/**
 * kdbus_name_registry_new() - create a new name registry
 * @reg:		The returned name registry
 *
 * Returns 0 on success, -ENOMEM if memory allocation failed.
 */
int kdbus_name_registry_new(struct kdbus_name_registry **reg)
{
	struct kdbus_name_registry *r;

	r = kzalloc(sizeof(*r), GFP_KERNEL);
	if (!r)
		return -ENOMEM;

	kref_init(&r->kref);
	hash_init(r->entries_hash);
	mutex_init(&r->entries_lock);

	*reg = r;

	return 0;
}

static struct kdbus_name_entry *
__kdbus_name_lookup(struct kdbus_name_registry *reg,
		    u32 hash, const char *name)
{
	struct kdbus_name_entry *e;

	hash_for_each_possible(reg->entries_hash, e, hentry, hash)
		if (strcmp(e->name, name) == 0)
			return e;

	return NULL;
}

static void kdbus_name_queue_item_free(struct kdbus_name_queue_item *q)
{
	list_del(&q->entry_entry);
	list_del(&q->conn_entry);
	kfree(q);
}

static void kdbus_name_entry_detach(struct kdbus_name_entry *e)
{
	mutex_lock(&e->conn->lock);
	e->conn->names--;
	list_del(&e->conn_entry);
	mutex_unlock(&e->conn->lock);
}

static void kdbus_name_entry_attach(struct kdbus_name_entry *e,
				    struct kdbus_conn *conn)
{
	mutex_lock(&conn->lock);
	e->conn = kdbus_conn_ref(conn);
	list_add_tail(&e->conn_entry, &e->conn->names_list);
	conn->names++;
	mutex_unlock(&conn->lock);
}

static void kdbus_name_entry_release(struct kdbus_name_entry *e,
				     struct list_head *notification_list)
{
	struct kdbus_name_queue_item *q;

	kdbus_name_entry_detach(e);

	/*
	 * When a name is released, check if there is any connection
	 * waiting to take over. If there isn't, check if the name has
	 * had a starter; in this case, hand the name back to it.
	 */
	if (list_empty(&e->queue_list)) {
		if (e->starter) {
			kdbus_notify_name_change(e->conn->ep,
						 KDBUS_ITEM_NAME_CHANGE,
						 e->conn->id, e->starter->id,
						 e->flags, e->name,
						 notification_list);
			kdbus_name_entry_attach(e, e->starter);
		} else {
			kdbus_notify_name_change(e->conn->ep,
						 KDBUS_ITEM_NAME_REMOVE,
						 e->conn->id, 0,
						 e->flags, e->name,
						 notification_list);
			kdbus_name_entry_free(e);
		}
	} else {
		struct kdbus_conn *old_conn = e->conn;

		q = list_first_entry(&e->queue_list,
				     struct kdbus_name_queue_item,
				     entry_entry);
		e->flags = q->flags;
		kdbus_name_entry_attach(e, q->conn);
		kdbus_name_queue_item_free(q);
		kdbus_notify_name_change(old_conn->ep, KDBUS_ITEM_NAME_CHANGE,
					 old_conn->id, e->conn->id,
					 e->flags, e->name, notification_list);
	}
}

static int kdbus_name_release(struct kdbus_name_entry *e,
			      struct kdbus_conn *conn,
			      struct list_head *notification_list)
{
	struct kdbus_name_queue_item *q_tmp, *q;

	/* Is the connection already the real owner of the name? */
	if (e->conn == conn) {
		kdbus_name_entry_release(e, notification_list);
		return 0;
	}

	/*
	 * Otherwise, walk the list of queued entries and search for
	 * items for the connection.
	 */
	list_for_each_entry_safe(q, q_tmp, &e->queue_list, entry_entry) {
		if (q->conn != conn)
			continue;
		kdbus_name_queue_item_free(q);
		return 0;
	}

	/*
	 * If we didn't find the connection in the lists above,
	 * deny permisson to removal.
	 */
	return -EPERM;
}

/**
 * kdbus_name_remove_by_conn() - remove all name entries of a given connection
 * @reg:		The name registry
 * @conn:		The connection which entries to remove
 *
 * This function removes all name entry held by a given connection.
 */
void kdbus_name_remove_by_conn(struct kdbus_name_registry *reg,
			       struct kdbus_conn *conn)
{
	struct kdbus_name_entry *e_tmp, *e;
	struct kdbus_name_queue_item *q_tmp, *q;
	LIST_HEAD(notification_list);
	LIST_HEAD(names_queue_list);
	LIST_HEAD(names_list);

	mutex_lock(&conn->lock);
	list_splice_init(&conn->names_list, &names_list);
	list_splice_init(&conn->names_queue_list, &names_queue_list);
	mutex_unlock(&conn->lock);

	mutex_lock(&reg->entries_lock);
	list_for_each_entry_safe(q, q_tmp, &names_queue_list, conn_entry)
		kdbus_name_queue_item_free(q);
	list_for_each_entry_safe(e, e_tmp, &names_list, conn_entry)
		kdbus_name_entry_release(e, &notification_list);
	mutex_unlock(&reg->entries_lock);

	kdbus_conn_kmsg_list_send(conn->ep, NULL, &notification_list);
}

/**
 * kdbus_name_lookup() - look up a name in a name registry
 * @reg:		The name registry
 * @name:		The name to look up
 *
 * Returns the name entry, if found. Otherwise, NULL is returned.
 */
struct kdbus_name_entry *kdbus_name_lookup(struct kdbus_name_registry *reg,
					   const char *name)
{
	struct kdbus_name_entry *e = NULL;
	u32 hash = kdbus_str_hash(name);

	mutex_lock(&reg->entries_lock);
	e = __kdbus_name_lookup(reg, hash, name);
	mutex_unlock(&reg->entries_lock);

	return e;
}

static int kdbus_name_queue_conn(struct kdbus_conn *conn, u64 *flags,
				 struct kdbus_name_entry *e)
{
	struct kdbus_name_queue_item *q;

	q = kzalloc(sizeof(*q), GFP_KERNEL);
	if (!q)
		return -ENOMEM;

	q->conn = conn;
	q->flags = *flags;

	list_add_tail(&q->entry_entry, &e->queue_list);
	list_add_tail(&q->conn_entry, &conn->names_queue_list);
	*flags |= KDBUS_NAME_IN_QUEUE;

	return 0;
}

/* called with entries_lock held */
static int kdbus_name_handle_conflict(struct kdbus_name_registry *reg,
				      struct kdbus_conn *conn,
				      struct kdbus_name_entry *e, u64 *flags,
				      struct list_head *notification_list)
{
	u64 old_id;
	int ret;

	/*
	 * When the acquisition of an already taken name is requested,
	 * check if replacing the ownership was explicitly allowed by the
	 * owner.
	 */
	if ((*flags & KDBUS_NAME_REPLACE_EXISTING) &&
	    (e->flags & KDBUS_NAME_ALLOW_REPLACEMENT)) {
		if (e->flags & KDBUS_NAME_QUEUE) {
			ret = kdbus_name_queue_conn(e->conn, &e->flags, e);
			if (ret < 0)
				return -ENOMEM;
		}

		old_id = e->conn->id;

		/*
		 * In case the name is owned by a starter connection, take it over.
		 * Move queued messages from the starter to the new connection.
		 */
		if (e->starter) {
			ret = kdbus_conn_move_messages(conn, e->starter);
			if (ret < 0)
				return ret;

			kdbus_conn_unref(e->conn);
			e->conn = kdbus_conn_ref(conn);

			kdbus_conn_unref(e->starter);
			e->starter = NULL;
		}

		/* update the owner of the name */
		kdbus_name_entry_detach(e);
		kdbus_name_entry_attach(e, conn);

		e->flags = *flags;

		return kdbus_notify_name_change(conn->ep,
						KDBUS_ITEM_NAME_CHANGE,
						old_id, conn->id, *flags,
						e->name, notification_list);
	}

	if (*flags & KDBUS_NAME_QUEUE)
		return kdbus_name_queue_conn(conn, flags, e);

	return -EEXIST;
}

/**
 * kdbus_name_is_valid() - check if a name is value
 * @p:			The name to check
 *
 * A name is valid if all of the following criterias are met:
 *
 *  - The name has one or more elements separated by a period ('.') character.
 *    All elements must contain at least one character.
 *  - Each element must only contain the ASCII characters "[A-Z][a-z][0-9]_"
 *    and must not begin with a digit.
 *  - The name must contain at least one '.' (period) character
 *    (and thus at least two elements).
 *  - The name must not begin with a '.' (period) character.
 *  - The name must not exceed KDBUS_NAME_MAX_LEN.
 */
bool kdbus_name_is_valid(const char *p)
{
	const char *q;
	bool dot, found_dot;

	for (dot = true, q = p; *q; q++) {
		if (*q == '.') {
			if (dot)
				return false;

			found_dot = dot = true;
		} else {
			bool good;

			good = isalpha(*q) || (!dot && isdigit(*q)) ||
				*q == '_' || *q == '-';

			if (!good)
				return false;

			dot = false;
		}
	}

	if (q - p > KDBUS_NAME_MAX_LEN)
		return false;

	if (dot)
		return false;

	if (!found_dot)
		return false;

	return true;
}

/**
 * kdbus_name_acquire() - acquire a name
 * @reg:		The name registry
 * @conn:		The connection to pin this entry to
 * @name:		The name to acquire
 * @flags:		Acquisition flags (KDBUS_NAME_*)
 * @entry:		Return pointer for the entry (may be NULL)
 *
 * Returns 0 on success, other values on error.
 */
int kdbus_name_acquire(struct kdbus_name_registry *reg,
		       struct kdbus_conn *conn,
		       const char *name, u64 flags,
		       struct kdbus_name_entry **entry)
{
	struct kdbus_name_entry *e = NULL;
	LIST_HEAD(notification_list);
	int ret = 0;
	u32 hash;

	hash = kdbus_str_hash(name);

	mutex_lock(&reg->entries_lock);
	e = __kdbus_name_lookup(reg, hash, name);
	if (e) {
		if (e->conn == conn) {
			e->flags = flags;
			ret = -EALREADY;
			goto exit_unlock;
		} else {
			ret = kdbus_name_handle_conflict(reg, conn, e, &flags,
							 &notification_list);
			if (ret < 0)
				goto exit_unlock;
		}

		goto exit_unlock;
	}

	e = kzalloc(sizeof(*e), GFP_KERNEL);
	if (!e) {
		ret = -ENOMEM;
		goto exit_unlock;
	}

	e->name = kstrdup(name, GFP_KERNEL);
	if (!e->name) {
		kfree(e);
		ret = -ENOMEM;
		goto exit_unlock;
	}

	if (conn->flags & KDBUS_HELLO_STARTER) {
		e->starter = kdbus_conn_ref(conn);
		flags = KDBUS_NAME_ALLOW_REPLACEMENT;
	}

	e->flags = flags;
	INIT_LIST_HEAD(&e->queue_list);
	INIT_LIST_HEAD(&e->conn_entry);
	hash_add(reg->entries_hash, &e->hentry, hash);
	kdbus_name_entry_attach(e, conn);

	kdbus_notify_name_change(e->conn->ep, KDBUS_ITEM_NAME_ADD, 0,
				 e->conn->id, e->flags, e->name,
				 &notification_list);

	if (entry)
		*entry = e;

exit_unlock:
	mutex_unlock(&reg->entries_lock);
	kdbus_conn_kmsg_list_send(conn->ep, NULL, &notification_list);

	return ret;
}

/**
 * kdbus_name_acquire() - acquire a name from a ioctl command buffer
 * @reg:		The name registry
 * @conn:		The connection to pin this entry to
 * @buf:		The __user buffer as passed in by the ioctl
 *
 * Returns 0 on success, other values on error.
 */
int kdbus_cmd_name_acquire(struct kdbus_name_registry *reg,
			   struct kdbus_conn *conn,
			   void __user *buf)
{
	struct kdbus_name_entry *e = NULL;
	struct kdbus_cmd_name *cmd_name;
	LIST_HEAD(notification_list);
	u64 size;
	u32 hash;
	int ret = 0;

	if (kdbus_size_get_user(&size, buf, struct kdbus_cmd_name))
		return -EFAULT;

	if ((size < sizeof(struct kdbus_cmd_name)) ||
	    (size > (sizeof(struct kdbus_cmd_name) + KDBUS_NAME_MAX_LEN + 1)))
		return -EMSGSIZE;

	if (conn->names > KDBUS_CONN_MAX_NAMES)
		return -E2BIG;

	cmd_name = memdup_user(buf, size);
	if (IS_ERR(cmd_name))
		return PTR_ERR(cmd_name);

	if (!kdbus_name_is_valid(cmd_name->name)) {
		ret = -EINVAL;
		goto exit_free;
	}

	/* privileged users can act on behalf of someone else */
	if (cmd_name->id != 0) {
		struct kdbus_conn *new_conn;
		struct kdbus_bus *bus = conn->ep->bus;

		if (!kdbus_bus_uid_is_privileged(bus)) {
			ret = -EPERM;
			goto exit_free;
		}

		mutex_lock(&bus->lock);
		new_conn = kdbus_bus_find_conn_by_id(bus, cmd_name->id);
		mutex_unlock(&bus->lock);

		if (!new_conn) {
			ret = -ENXIO;
			goto exit_free;
		}

		conn = new_conn;
	} else {
		kdbus_conn_ref(conn);
	}

	cmd_name->flags &= ~KDBUS_NAME_IN_QUEUE;
	hash = kdbus_str_hash(cmd_name->name);

	if (conn->ep->policy_db) {
		if (!kdbus_policy_db_check_own_access(conn->ep->policy_db,
						      conn, cmd_name->name)) {
			ret = -EPERM;
			goto exit_unref_conn;
		}
	}

	ret = kdbus_name_acquire(reg, conn, cmd_name->name,
				 cmd_name->flags, &e);
	if (ret < 0)
		goto exit_unref_conn;

	if (copy_to_user(buf, cmd_name, size)) {
		ret = -EFAULT;
		kdbus_conn_kmsg_list_free(&notification_list);
		kdbus_name_entry_release(e, NULL);
	}

exit_unref_conn:
	kdbus_conn_kmsg_list_send(conn->ep, NULL, &notification_list);
	kdbus_conn_unref(conn);

exit_free:
	kfree(cmd_name);

	return ret;
}

/**
 * kdbus_cmd_name_release() - release a name entry from a ioctl command buffer
 * @reg:		The name registry
 * @conn:		The connection that holds the name
 * @buf:		The __user buffer as passed in by the ioctl
 *
 * Returns 0 on success, other values on error.
 */
int kdbus_cmd_name_release(struct kdbus_name_registry *reg,
			   struct kdbus_conn *conn,
			   void __user *buf)
{
	struct kdbus_name_entry *e;
	struct kdbus_cmd_name *cmd_name;
	LIST_HEAD(notification_list);
	u64 size;
	u32 hash;
	int ret = 0;

	if (kdbus_size_get_user(&size, buf, struct kdbus_cmd_name))
		return -EFAULT;

	if ((size < sizeof(struct kdbus_cmd_name)) ||
	    (size > (sizeof(struct kdbus_cmd_name) + KDBUS_NAME_MAX_LEN + 1)))
		return -EMSGSIZE;

	cmd_name = memdup_user(buf, size);
	if (IS_ERR(cmd_name))
		return PTR_ERR(cmd_name);

	if (!kdbus_name_is_valid(cmd_name->name)) {
		ret = -EINVAL;
		goto exit_free;
	}

	hash = kdbus_str_hash(cmd_name->name);

	mutex_lock(&reg->entries_lock);
	e = __kdbus_name_lookup(reg, hash, cmd_name->name);
	if (!e) {
		ret = -ESRCH;
		goto exit_unlock;
	}

	/* privileged users can act on behalf of someone else */
	if (e->conn != conn) {
		struct kdbus_bus *bus = conn->ep->bus;

		if (!kdbus_bus_uid_is_privileged(bus)) {
			ret = -EPERM;
			goto exit_unlock;
		}

		mutex_lock(&bus->lock);
		conn = kdbus_bus_find_conn_by_id(bus, cmd_name->id);
		mutex_unlock(&bus->lock);

		if (!conn) {
			ret = -EPERM;
			goto exit_unlock;
		}
	} else {
		kdbus_conn_ref(conn);
	}

	ret = kdbus_name_release(e, conn, &notification_list);

exit_unlock:
	mutex_unlock(&reg->entries_lock);

	if (conn)
		kdbus_conn_kmsg_list_send(conn->ep, NULL,
					  &notification_list);

	kdbus_conn_unref(conn);

exit_free:
	kfree(cmd_name);
	return ret;
}

/**
 * kdbus_cmd_name_list() - list names of a connection
 * @reg:		The name registry
 * @conn:		The connection holding the name entries
 * @buf:		The __user buffer as passed in by the ioctl
 *
 * Returns 0 on success, other values on error.
 */
int kdbus_cmd_name_list(struct kdbus_name_registry *reg,
			struct kdbus_conn *conn,
			void __user *buf)
{
	struct kdbus_cmd_name_list *cmd_list;
	struct kdbus_name_list list = {};
	struct kdbus_bus *bus = conn->ep->bus;
	size_t size;
	u64 tmp;
	size_t off, pos;
	int ret = 0;

	cmd_list = memdup_user(buf, sizeof(struct kdbus_cmd_name_list));
	if (IS_ERR(cmd_list))
		return PTR_ERR(cmd_list);

	mutex_lock(&reg->entries_lock);

	/* calculate size of return buffer */
	size = sizeof(struct kdbus_name_list);

	if (cmd_list->flags & KDBUS_NAME_LIST_UNIQUE) {
		struct kdbus_conn *c;
		int i;

		hash_for_each(bus->conn_hash, i, c, hentry) {
			/* skip starters */
			if (!(cmd_list->flags & KDBUS_NAME_LIST_STARTERS) &&
			    (c->flags & KDBUS_HELLO_STARTER))
				continue;

			size += KDBUS_ALIGN8(sizeof(struct kdbus_cmd_name));
		}
	}

	if (cmd_list->flags & KDBUS_NAME_LIST_NAMES) {
		struct kdbus_name_entry *e;

		hash_for_each(reg->entries_hash, tmp, e, hentry) {
			/* skip starters */
			if (!(cmd_list->flags & KDBUS_NAME_LIST_STARTERS) &&
			    (e->starter))
				continue;

			/* skip queued names */
			if (!(cmd_list->flags & KDBUS_NAME_LIST_QUEUED) &&
			    (e->flags & KDBUS_NAME_QUEUE))
				continue;

			size += KDBUS_ALIGN8(sizeof(struct kdbus_cmd_name) +
					     strlen(e->name) + 1);
		}
	}

	ret = kdbus_pool_alloc(conn->pool, size, &off);
	if (ret < 0)
		goto exit_unlock;

	/* copy header */
	pos = off;
	list.size = size;
	ret = kdbus_pool_write(conn->pool, pos,
			       &list, sizeof(struct kdbus_name_list));
	if (ret < 0) {
		kdbus_pool_free(conn->pool, off);
		goto exit_unlock;
	}
	pos += sizeof(struct kdbus_name_list);

	if (cmd_list->flags & KDBUS_NAME_LIST_UNIQUE) {
		struct kdbus_conn *c;
		int i;

		hash_for_each(bus->conn_hash, i, c, hentry) {
			struct kdbus_cmd_name cmd_name = {};

			/* skip starters */
			if (!(cmd_list->flags & KDBUS_NAME_LIST_STARTERS) &&
			    (c->flags & KDBUS_HELLO_STARTER))
				continue;

			cmd_name.size = sizeof(struct kdbus_cmd_name);
			cmd_name.id = c->id;
			cmd_name.conn_flags = c->flags;

			ret = kdbus_pool_write(conn->pool, pos,
					       &cmd_name, sizeof(cmd_name));
			if (ret < 0) {
				kdbus_pool_free(conn->pool, off);
				goto exit_unlock;
			}

			pos += KDBUS_ALIGN8(sizeof(struct kdbus_cmd_name));
		}
	}

	if (cmd_list->flags & KDBUS_NAME_LIST_NAMES) {
		struct kdbus_name_entry *e;

		hash_for_each(reg->entries_hash, tmp, e, hentry) {
			struct kdbus_cmd_name cmd_name = {};
			size_t len;

			/* skip starters */
			if (!(cmd_list->flags & KDBUS_NAME_LIST_STARTERS) &&
			    e->starter)
				continue;

			/* skip queued names */
			if (!(cmd_list->flags & KDBUS_NAME_LIST_QUEUED) &&
			    (e->flags & KDBUS_NAME_QUEUE))
				continue;

			cmd_name.size = sizeof(struct kdbus_cmd_name) +
					strlen(e->name) + 1;
			cmd_name.flags = e->flags;
			cmd_name.id = e->conn->id;
			cmd_name.conn_flags = e->conn->flags;

			ret = kdbus_pool_write(conn->pool, pos,
					       &cmd_name, sizeof(cmd_name));
			if (ret < 0) {
				kdbus_pool_free(conn->pool, off);
				goto exit_unlock;
			}
			pos += sizeof(struct kdbus_cmd_name);

			len = strlen(e->name) + 1;
			ret = kdbus_pool_write(conn->pool, pos, e->name, len);
			if (ret < 0) {
				kdbus_pool_free(conn->pool, off);
				goto exit_unlock;
			}

			pos += KDBUS_ALIGN8(len);
		}
	}

	if (kdbus_offset_set_user(&off, buf, struct kdbus_cmd_name_list)) {
		ret = -EFAULT;
		kdbus_pool_free(conn->pool, off);
		goto exit_unlock;
	}

exit_unlock:
	mutex_unlock(&reg->entries_lock);
	kfree(cmd_list);

	return ret;
}
