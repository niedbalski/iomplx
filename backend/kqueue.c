/*
 * iomplx
 * Copyright © 2011 Felipe Astroza
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

#include <iomplx.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <assert.h>
extern int errno;

void uqueue_init(uqueue *q)
{
	q->kqueue_iface = kqueue();
	q->changes_count = 0;
}

void uqueue_event_init(iomplx_event *ev)
{
	ev->data.events_count = 0;
	ev->data.current_event = 0;
}

int uqueue_wait(uqueue *q, iomplx_event *ev, int timeout)
{
	struct kevent *ke;
	struct timespec ts, *pts;
	int wait_ret;

	ev->data.current_event++;
	if(ev->data.events_count <= ev->data.current_event) {
		do {
			if(timeout < 0)
				pts = NULL;
			else {
				ts.tv_sec = timeout;
				ts.tv_nsec = 0;
				pts = &ts;
			}
			wait_ret = kevent(q->kqueue_iface, q->changes, 
			q->changes_count, ev->data.events, EVENTS, pts);
		} while(wait_ret == -1 && errno == EINTR);
		q->changes_count = 0;

		if(wait_ret == 0) 
			return 0;

		ev->data.events_count = wait_ret;
		ev->data.current_event = 0;
	}

	ke = ev->data.events + ev->data.current_event;
	ev->item = ke->udata;
	assert(ev->item != NULL);
	if(ke->flags & EV_EOF)
		ev->type = IOMPLX_CLOSE_EVENT;
	else if(ke->filter == EVFILT_READ)
		ev->type = IOMPLX_READ_EVENT;
	else if(ke->filter == EVFILT_WRITE)
		ev->type = IOMPLX_WRITE_EVENT;
	return 1;
}

void uqueue_watch(uqueue *q, iomplx_item *item)
{
	struct kevent c;
	
	EV_SET(&c, item->fd, EVFILT_READ, EV_ADD|EV_CLEAR|EV_ENABLE|EV_RECEIPT, 0, 0, item);
	kevent(q->kqueue_iface, &c, 1, NULL, 0, NULL);
}

void uqueue_unwatch(uqueue *q, iomplx_item *item)
{
}

void uqueue_filter_set(uqueue *q, iomplx_item *item)
{
	EV_SET(q->changes + q->changes_count, item->fd, item->new_filter, EV_ADD|EV_ENABLE|EV_CLEAR, 0, 0, item);
	q->changes_count++;
}

int accept_and_set(int fd, struct sockaddr *sa, unsigned int *sa_size)
{
	int new_fd;
	int set = 1;

	new_fd = accept(fd, sa, sa_size);
	if(new_fd != -1)
		ioctl(new_fd, FIONBIO, &set);

	return new_fd;
}
