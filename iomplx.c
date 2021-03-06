/*
 * IOMPLX
 * Copyright © 2012 Felipe Astroza
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <pthread.h>
#include <sys/types.h>
#include <sys/resource.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>
#include <iomplx.h>
#include <time.h>
#include <unistd.h>
#include <assert.h>

static inline void iomplx_monitor_add(iomplx_monitor *mon, iomplx_item *item)
{
	DLIST_APPEND(mon, item);
}

static inline void iomplx_monitor_del(iomplx_monitor *mon, iomplx_item *item)
{
	DLIST_DEL(mon, item);
}

static inline void iomplx_monitor_init(iomplx_monitor *mon, int timeout_granularity)
{
	DLIST_INIT(mon);
	mon->timeout_granularity = timeout_granularity;
}

static void iomplx_waiter_init(iomplx_waiter *waiter)
{
	waiter->item = NULL;
	uqueue_event_init(waiter);
}

static int iomplx_dummy_call1(iomplx_item *item)
{
	return 0;
}

static inline void iomplx_active_list_size_fix(unsigned int *active_list_size, unsigned int threads)
{
	struct rlimit rlmt;
	unsigned int active_items = 0;

	assert(threads);
	assert(getrlimit(RLIMIT_NOFILE, &rlmt) != -1 || rlmt.rlim_cur != 0);

	if ( !active_list_size[THREAD_N] )
		active_list_size[THREAD_N] = IOMPLX_THREAD_N_ACTIVE_ITEMS;

	if ( !active_list_size[THREAD_0] )
		active_list_size[THREAD_0] = IOMPLX_THREAD_0_ACTIVE_ITEMS;

	active_items = active_list_size[THREAD_N] * threads + active_list_size[THREAD_0];

	if ( active_items > rlmt.rlim_cur ) {
		assert(active_list_size[THREAD_0] < rlmt.rlim_cur);
		active_list_size[THREAD_N] = (rlmt.rlim_cur - active_list_size[THREAD_0]) / threads;
	}
}

static const iomplx_callbacks iomplx_dummy_calls = {
	.calls_arr = {iomplx_dummy_call1, iomplx_dummy_call1, iomplx_dummy_call1, iomplx_dummy_call1, iomplx_dummy_call1}
};

void iomplx_callbacks_init(iomplx_item *item)
{
	memcpy(&item->cb, &iomplx_dummy_calls, sizeof(iomplx_callbacks));
}

static void iomplx_active_list_init(iomplx_active_list *active_list, unsigned int calls_number)
{
	DLIST_INIT(active_list);
	mempool_init(&active_list->item_calls_pool, sizeof(iomplx_item_call), calls_number);
	active_list->available_item_calls = calls_number;
}

static int iomplx_active_list_call_add(iomplx_active_list *active_list, iomplx_item *item, unsigned char call_idx)
{
	iomplx_item_call *item_call;

	item_call = mempool_alloc(&active_list->item_calls_pool);
	if(!item_call)
		return -1;

	item_call->call_idx = call_idx;
	item_call->item = item;

	DLIST_APPEND(active_list, item_call);
	active_list->available_item_calls--;

	return 0;
}

static void iomplx_active_list_call_del(iomplx_active_list *active_list, iomplx_item_call *call)
{
	DLIST_DEL(active_list, call);
	mempool_free(&active_list->item_calls_pool, call);
	active_list->available_item_calls++;
}

static void iomplx_active_list_populate(iomplx_active_list *active_list, uqueue *q, int wait_timeout)
{
	int timeout;
	int rmg;
	iomplx_waiter waiter;

	if(active_list->available_item_calls == 0)
		return;

	iomplx_waiter_init(&waiter);

	if(DLIST_ISEMPTY(active_list))
		timeout = wait_timeout;
	else
		timeout = 0;

	waiter.max_events = active_list->available_item_calls;
	do {
		rmg = uqueue_event_get(q, &waiter, timeout);
		if(rmg != -1 && (waiter.type == IOMPLX_CLOSE_EVENT || iomplx_active_set(waiter.item)))
			iomplx_active_list_call_add(active_list, waiter.item, waiter.type);
	} while(rmg > 0);
}

static void iomplx_do_maintenance(iomplx_instance *mplx, unsigned long *start_time)
{
	iomplx_item *item;
	time_t cur_time = time(NULL);

	if(cur_time - *start_time >= mplx->monitor.timeout_granularity) {
		DLIST_FOREACH(&mplx->monitor AS item) {
			/* Free closed item */
			if(item->fd == -1) {
				iomplx_monitor_del(&mplx->monitor, item);
				mplx->item_free(item);
				continue;
			}
			/* Timeout check */
			item->elapsed_time++;
			if(item->elapsed_time >= item->timeout && item->cb.ev_timeout(item) == -1) {
				if(iomplx_timeout_tryset(item))
					shutdown(item->fd, SHUT_RDWR);
			}
		}
		*start_time = cur_time;
	}
}

static void iomplx_thread_0(iomplx_instance *mplx)
{
	iomplx_item *item, *new_item, local_item;
	iomplx_active_list active_list;
	iomplx_item_call *item_call;
	unsigned long start_time = time(NULL);

	iomplx_active_list_init(&active_list, mplx->active_list_size[THREAD_0]);

	do {
		iomplx_active_list_populate(&active_list, &mplx->accept_uqueue, mplx->monitor.timeout_granularity);

		DLIST_FOREACH(&active_list AS item_call) {
			item = item_call->item;
			iomplx_callbacks_init(&local_item);
			local_item.new_filter = IOMPLX_READ;
			local_item.sa_size = item->sa_size;
			local_item.oneshot = 1;
			local_item.fd = accept_and_set(item->fd, &local_item.sa, &local_item.sa_size);
			if(local_item.fd != -1) {
				if(item->cb.ev_accept(&local_item) < 0 || !(new_item = iomplx_item_add(mplx, &local_item, 0)))
					close(local_item.fd);
				else
					iomplx_monitor_add(&mplx->monitor, new_item);
			} else {
				iomplx_active_list_call_del(&active_list, item_call);
				iomplx_active_unset(item);
			}
		}
		iomplx_do_maintenance(mplx, &start_time);

	} while(1);
}

static void iomplx_thread_n(iomplx_instance *mplx)
{
	iomplx_active_list active_list;
	iomplx_item_call *item_call;
	iomplx_item *item;
	int ret;

	if(mplx->thread_init)
		mplx->thread_init();

	iomplx_active_list_init(&active_list, mplx->active_list_size[THREAD_N]);

	do {
		iomplx_active_list_populate(&active_list, &mplx->n_uqueue, -1);

		DLIST_FOREACH(&active_list AS item_call) {
			item = item_call->item;
			ret = item->cb.calls_arr[item_call->call_idx](item);

			if(ret == IOMPLX_ITEM_CLOSE && item_call->call_idx != IOMPLX_CLOSE_EVENT)  {
				item_call->call_idx = IOMPLX_CLOSE_EVENT;
				continue;
			}

			if(item_call->call_idx == IOMPLX_CLOSE_EVENT) {
				uqueue_unwatch(&mplx->n_uqueue, item);
				close(item->fd);
				item->fd = -1;
				iomplx_active_list_call_del(&active_list, item_call);
				iomplx_active_unset(item);
			} else if(ret == IOMPLX_ITEM_WOULDBLOCK) {
				uqueue_rewatch(&mplx->n_uqueue, item);
				iomplx_active_list_call_del(&active_list, item_call);
				iomplx_active_unset(item);
			} else if(item->new_filter != IOMPLX_NONE) {
				if(item->new_filter == IOMPLX_WRITE)
					item_call->call_idx = IOMPLX_WRITE_EVENT;
				else if(item->new_filter == IOMPLX_READ)
					item_call->call_idx = IOMPLX_READ_EVENT;

				item->filter = item->new_filter;
				item->new_filter = IOMPLX_NONE;
			}
		}

	} while(1);
}

iomplx_item *iomplx_item_add(iomplx_instance *mplx, iomplx_item *item, int listening)
{
	iomplx_item *item_copy;

	item_copy = mplx->item_alloc(sizeof(iomplx_item));
	if(item_copy == NULL)
		return NULL;

	DLIST_NODE_INIT(item_copy);
	item_copy->new_filter = item->new_filter;
	memcpy(&item_copy->cb, &item->cb, sizeof(iomplx_callbacks));
	item_copy->fd = item->fd;
	item_copy->timeout = item->timeout;
	item_copy->oneshot = item->oneshot;
	item_copy->active = 0;
	item_copy->data = item->data;

	if(listening)
		uqueue_watch(&mplx->accept_uqueue, item_copy);
	else
		uqueue_watch(&mplx->n_uqueue, item_copy);

	item_copy->elapsed_time = 0;

	return item_copy;
}

void iomplx_init(iomplx_instance *mplx, alloc_func alloc, free_func free, init_func init, unsigned int threads, unsigned int timeout_granularity, unsigned int thread_zero_active_items, unsigned int thread_n_active_items)
{
	mplx->item_alloc = alloc;
	mplx->item_free = free;
	mplx->thread_init = init;
	mplx->threads = threads;

	mplx->active_list_size[THREAD_0] = thread_zero_active_items;
	mplx->active_list_size[THREAD_N] = thread_n_active_items;

	iomplx_active_list_size_fix(mplx->active_list_size, threads);
	iomplx_monitor_init(&mplx->monitor, timeout_granularity);

	uqueue_init(&mplx->accept_uqueue);
	uqueue_init(&mplx->n_uqueue);
}

static void iomplx_start_threads(iomplx_instance *mplx)
{
	unsigned int i;
	pthread_attr_t attr;
	pthread_t unused;

#if USE_CPU_AFFINITY
	int procs = 0;
	cpu_set_t cpu_set;

	procs = (unsigned int)sysconf( _SC_NPROCESSORS_ONLN );
	assert(procs != -1);

	//force to processor number
	if(mplx->threads >  procs)
		mplx->threads = procs;

	CPU_ZERO(&cpu_set);
#endif
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

	for(i = 0; i < mplx->threads; i++) {
		pthread_create(&unused, &attr, (void *(*)(void *))iomplx_thread_n, (void *)mplx);

#if USE_CPU_AFFINITY
		CPU_SET(i, &cpu_set);
		pthread_setaffinity_np(unused, sizeof(cpu_set_t), &cpu_set);
#endif

	}
}

void iomplx_launch(iomplx_instance *mplx)
{
	iomplx_start_threads(mplx);
	iomplx_thread_0(mplx);
}
