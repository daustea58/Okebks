// SPDX-License-Identifier: GPL-2.0
/*
 * block/zen-iosched.c
 *
 * "Zen" I/O scheduler — dispatcher latensi-rendah untuk storage flash
 * (UFS/eMMC) khas perangkat mobile. Dirancang sebagai scheduler sederhana:
 * dua antrian FIFO (sync & async) dengan prioritas sync, tanpa merge-sort
 * kompleks ala CFQ/deadline, supaya overhead CPU rendah — cocok dipasangkan
 * dengan game_mode untuk menekan I/O stutter (loading asset/texture) tanpa
 * membebani CPU saat gaming.
 *
 * Ditulis untuk legacy elevator API kernel 4.14 non-GKI (block/elevator.c).
 */

#include <linux/blkdev.h>
#include <linux/elevator.h>
#include <linux/bio.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/init.h>

extern bool game_mode_is_active(void);

/* Batas waktu tunggu request di antrian sebelum dipaksa expire (ms) */
#define ZEN_SYNC_EXPIRE_MS		125
#define ZEN_ASYNC_EXPIRE_MS		250
#define ZEN_SYNC_EXPIRE_GAME_MS		60
#define ZEN_ASYNC_EXPIRE_GAME_MS	400	/* async boleh lebih ditahan saat gaming */

struct zen_data {
	struct list_head fifo_sync;
	struct list_head fifo_async;
	unsigned int sync_expire_ms;
	unsigned int async_expire_ms;
	unsigned int sync_expire_game_ms;
	unsigned int async_expire_game_ms;
};

static inline struct list_head *zen_list_for(struct zen_data *zd, int sync)
{
	return sync ? &zd->fifo_sync : &zd->fifo_async;
}

static void zen_merged_requests(struct request_queue *q, struct request *rq,
				 struct request *next)
{
	list_del_init(&next->queuelist);
}

static void zen_add_request(struct request_queue *q, struct request *rq)
{
	struct zen_data *zd = q->elevator->elevator_data;
	const int sync = rq_is_sync(rq);
	unsigned long expire_jiffies;
	unsigned int expire_ms;

	if (game_mode_is_active())
		expire_ms = sync ? zd->sync_expire_game_ms : zd->async_expire_game_ms;
	else
		expire_ms = sync ? zd->sync_expire_ms : zd->async_expire_ms;

	expire_jiffies = jiffies + msecs_to_jiffies(expire_ms);
	rq_set_fifo_time(rq, expire_jiffies);

	list_add_tail(&rq->queuelist, zen_list_for(zd, sync));
}

static struct request *zen_expired_request(struct zen_data *zd, int sync)
{
	struct list_head *list = zen_list_for(zd, sync);

	if (list_empty(list))
		return NULL;

	if (time_after_eq(jiffies, rq_fifo_time(list_first_entry(list, struct request, queuelist))))
		return list_first_entry(list, struct request, queuelist);

	return NULL;
}

static struct request *zen_choose_request(struct zen_data *zd)
{
	struct request *rq;

	/* Prioritas: request sync yang sudah expire, lalu async yang expire,
	 * lalu sync teratas (in-order), baru async. Ini menjaga urutan FIFO
	 * sederhana sambil tetap memberi prioritas ke I/O sinkron (mis.
	 * baca asset yang ditunggu game thread). */
	rq = zen_expired_request(zd, 1);
	if (rq)
		return rq;

	rq = zen_expired_request(zd, 0);
	if (rq)
		return rq;

	if (!list_empty(&zd->fifo_sync))
		return list_first_entry(&zd->fifo_sync, struct request, queuelist);

	if (!list_empty(&zd->fifo_async))
		return list_first_entry(&zd->fifo_async, struct request, queuelist);

	return NULL;
}

static int zen_dispatch(struct request_queue *q, int force)
{
	struct zen_data *zd = q->elevator->elevator_data;
	struct request *rq = zen_choose_request(zd);

	if (!rq)
		return 0;

	list_del_init(&rq->queuelist);
	elv_dispatch_add_tail(q, rq);
	return 1;
}

static void *zen_init_queue(struct request_queue *q, struct elevator_type *e)
{
	struct zen_data *zd;
	struct elevator_queue *eq;

	eq = elevator_alloc(q, e);
	if (!eq)
		return NULL;

	zd = kzalloc_node(sizeof(*zd), GFP_KERNEL, q->node);
	if (!zd) {
		kobject_put(&eq->kobj);
		return NULL;
	}
	eq->elevator_data = zd;

	INIT_LIST_HEAD(&zd->fifo_sync);
	INIT_LIST_HEAD(&zd->fifo_async);
	zd->sync_expire_ms = ZEN_SYNC_EXPIRE_MS;
	zd->async_expire_ms = ZEN_ASYNC_EXPIRE_MS;
	zd->sync_expire_game_ms = ZEN_SYNC_EXPIRE_GAME_MS;
	zd->async_expire_game_ms = ZEN_ASYNC_EXPIRE_GAME_MS;

	spin_lock_irq(q->queue_lock);
	q->elevator = eq;
	spin_unlock_irq(q->queue_lock);

	return zd;
}

static void zen_exit_queue(struct elevator_queue *e)
{
	struct zen_data *zd = e->elevator_data;

	BUG_ON(!list_empty(&zd->fifo_sync));
	BUG_ON(!list_empty(&zd->fifo_async));
	kfree(zd);
}

/* ---------------------------- sysfs tunables ---------------------------- */

static ssize_t zen_var_show(unsigned int var, char *page)
{
	return sprintf(page, "%d\n", var);
}

static ssize_t zen_var_store(unsigned int *var, const char *page, size_t count)
{
	int ret = kstrtouint(page, 10, var);

	return ret ? ret : count;
}

#define SHOW_FUNCTION(__FUNC, __VAR)					\
static ssize_t __FUNC(struct elevator_queue *e, char *page)		\
{									\
	struct zen_data *zd = e->elevator_data;			\
	return zen_var_show((__VAR), page);				\
}
SHOW_FUNCTION(zen_sync_expire_show, zd->sync_expire_ms);
SHOW_FUNCTION(zen_async_expire_show, zd->async_expire_ms);
SHOW_FUNCTION(zen_sync_expire_game_show, zd->sync_expire_game_ms);
SHOW_FUNCTION(zen_async_expire_game_show, zd->async_expire_game_ms);
#undef SHOW_FUNCTION

#define STORE_FUNCTION(__FUNC, __PTR)					\
static ssize_t __FUNC(struct elevator_queue *e, const char *page,	\
		       size_t count)					\
{									\
	struct zen_data *zd = e->elevator_data;			\
	return zen_var_store((__PTR), page, count);			\
}
STORE_FUNCTION(zen_sync_expire_store, &zd->sync_expire_ms);
STORE_FUNCTION(zen_async_expire_store, &zd->async_expire_ms);
STORE_FUNCTION(zen_sync_expire_game_store, &zd->sync_expire_game_ms);
STORE_FUNCTION(zen_async_expire_game_store, &zd->async_expire_game_ms);
#undef STORE_FUNCTION

#define ZEN_ATTR(name) \
	__ATTR(name, S_IRUGO | S_IWUSR, zen_##name##_show, zen_##name##_store)

static struct elv_fs_entry zen_attrs[] = {
	ZEN_ATTR(sync_expire),
	ZEN_ATTR(async_expire),
	ZEN_ATTR(sync_expire_game),
	ZEN_ATTR(async_expire_game),
	__ATTR_NULL
};

static struct elevator_type iosched_zen = {
	.ops = {
		.elevator_merge_req_fn		= zen_merged_requests,
		.elevator_dispatch_fn		= zen_dispatch,
		.elevator_add_req_fn		= zen_add_request,
		.elevator_init_fn		= zen_init_queue,
		.elevator_exit_fn		= zen_exit_queue,
	},
	.elevator_attrs = zen_attrs,
	.elevator_name = "zen",
	.elevator_owner = THIS_MODULE,
};

static int __init zen_init(void)
{
	return elv_register(&iosched_zen);
}

static void __exit zen_exit(void)
{
	elv_unregister(&iosched_zen);
}

module_init(zen_init);
module_exit(zen_exit);

MODULE_AUTHOR("Asep");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Zen I/O scheduler — latensi rendah untuk gaming, surya (POCO X3 NFC)");
