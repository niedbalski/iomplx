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

#ifndef IOMPLX_H
#include <dlist.h>
#include <sys/socket.h>

#define IOMPLX_CONF_BACKLOG	10

#define IOMPLX_READ_EVENT	1
#define IOMPLX_WRITE_EVENT	2
#define IOMPLX_TIMEOUT_EVENT	3
#define IOMPLX_CLOSE_EVENT	4

typedef struct _iomplx_item iomplx_item;

typedef int (*ev_call1)(iomplx_item *);
typedef void *(*alloc_func)(unsigned long);
typedef void (*free_func)(void *);

typedef union {
	struct {
		union {
			ev_call1 ev_connect;
			ev_call1 ev_accept;
		};
		ev_call1 ev_read;
		ev_call1 ev_write;
		ev_call1 ev_timeout;
		ev_call1 ev_close;
	};
	ev_call1 calls_arr[5];
} iomplx_callbacks;

struct _iomplx_item {
	DLIST_NODE(item);
	int fd;
	int new_filter;
	iomplx_callbacks cb;
	int timeout;
	int elapsed_time;
	void *data;
	union {
		struct {
			struct sockaddr sa;
			unsigned int sa_size;
		};
	};
};

typedef struct {
	DLIST(items);
	int timeout_granularity;
} iomplx_monitor;

#include "glue.h"

typedef struct {
	uqueue_events data;
	unsigned char type;
	iomplx_item *item;
} iomplx_event;

typedef struct {
	uqueue n_queue;
	uqueue accept_queue;
	int threads;
	iomplx_monitor monitor;
	alloc_func item_alloc;
	free_func item_free;
} iomplx_instance;

#define IOMPLX_READ	UQUEUE_READ_EVENT
#define IOMPLX_WRITE	UQUEUE_WRITE_EVENT

void uqueue_init(uqueue *);
void uqueue_event_init(iomplx_event *);
int uqueue_wait(uqueue *, iomplx_event *, int);
void uqueue_watch(uqueue *, iomplx_item *);
void uqueue_unwatch(uqueue *, iomplx_item *);
void uqueue_filter_set(uqueue *, iomplx_item *);
int accept_and_set(int, struct sockaddr *, unsigned int *);

int iomplx_listen(iomplx_instance *, const char *, unsigned short, ev_call1, void *);
int iomplx_connect(const char *, unsigned short, iomplx_callbacks *, void *);
void iomplx_filter_set(iomplx_item *, int);
void iomplx_specialize();

void iomplx_init(iomplx_instance *, alloc_func, free_func, unsigned int, unsigned int);
void iomplx_launch(iomplx_instance *);

#endif