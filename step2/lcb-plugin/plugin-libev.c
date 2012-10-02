/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2012 Couchbase, Inc.
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

/**
 * This file contains IO operations that use libev
 *
 * @author Sergey Avseyev
 */

#include "lcb-plugin/libev_io_opts.h"

struct libev_cookie {
    struct ev_loop *loop;
    int allocated;
};

static lcb_ssize_t lcb_io_recv(struct lcb_io_opt_st *iops,
                               lcb_socket_t sock,
                               void *buffer,
                               lcb_size_t len,
                               int flags)
{
    lcb_ssize_t ret = recv(sock, buffer, len, flags);
    if (ret < 0) {
        iops->error = errno;
    }
    return ret;
}

static lcb_ssize_t lcb_io_recvv(struct lcb_io_opt_st *iops,
                                lcb_socket_t sock,
                                struct lcb_iovec_st *iov,
                                lcb_size_t niov)
{
    struct msghdr msg;
    struct iovec vec[2];
    lcb_ssize_t ret;

    if (niov != 2) {
        err(1, "lcb_io_recvv: invalid niov");
    }
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = vec;
    msg.msg_iovlen = iov[1].iov_len ? (lcb_size_t)2 : (lcb_size_t)1;
    msg.msg_iov[0].iov_base = iov[0].iov_base;
    msg.msg_iov[0].iov_len = iov[0].iov_len;
    msg.msg_iov[1].iov_base = iov[1].iov_base;
    msg.msg_iov[1].iov_len = iov[1].iov_len;
    ret = recvmsg(sock, &msg, 0);

    if (ret < 0) {
        iops->error = errno;
    }

    return ret;
}

static lcb_ssize_t lcb_io_send(struct lcb_io_opt_st *iops,
                               lcb_socket_t sock,
                               const void *msg,
                               lcb_size_t len,
                               int flags)
{
    lcb_ssize_t ret = send(sock, msg, len, flags);
    if (ret < 0) {
        iops->error = errno;
    }
    return ret;
}

static lcb_ssize_t lcb_io_sendv(struct lcb_io_opt_st *iops,
                                lcb_socket_t sock,
                                struct lcb_iovec_st *iov,
                                lcb_size_t niov)
{
    struct msghdr msg;
    struct iovec vec[2];
    lcb_ssize_t ret;

    if (niov != 2) {
        err(1, "lcb_io_recvv: invalid niov");
    }
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = vec;
    msg.msg_iovlen = iov[1].iov_len ? (lcb_size_t)2 : (lcb_size_t)1;
    msg.msg_iov[0].iov_base = iov[0].iov_base;
    msg.msg_iov[0].iov_len = iov[0].iov_len;
    msg.msg_iov[1].iov_base = iov[1].iov_base;
    msg.msg_iov[1].iov_len = iov[1].iov_len;
    ret = sendmsg(sock, &msg, 0);

    if (ret < 0) {
        iops->error = errno;
    }
    return ret;
}

static int make_socket_nonblocking(lcb_socket_t sock)
{
#ifdef _WIN32
    u_long nonblocking = 1;
    if (ioctlsocket(sock, FIONBIO, &nonblocking) == SOCKET_ERROR) {
        return -1;
    }
#else
    int flags;
    if ((flags = fcntl(sock, F_GETFL, NULL)) < 0) {
        return -1;
    }
    if (fcntl(sock, F_SETFL, flags | O_NONBLOCK) == -1) {
        return -1;
    }
#endif

    return 0;
}

static int close_socket(lcb_socket_t sock)
{
#ifndef _WIN32
    return close(sock);
#else
    return closesocket(sock);
#endif
}

static lcb_socket_t lcb_io_socket(struct lcb_io_opt_st *iops,
                                  int domain,
                                  int type,
                                  int protocol)
{
    lcb_socket_t sock = socket(domain, type, protocol);
    if (sock == INVALID_SOCKET) {
        iops->error = errno;
    } else {
        if (make_socket_nonblocking(sock) != 0) {
            int error = errno;
            iops->close(iops, sock);
            iops->error = error;
            sock = INVALID_SOCKET;
        }
    }

    return sock;
}

static void lcb_io_close(struct lcb_io_opt_st *iops,
                         lcb_socket_t sock)
{
    close_socket(sock);
    (void)iops;
}

static int lcb_io_connect(struct lcb_io_opt_st *iops,
                          lcb_socket_t sock,
                          const struct sockaddr *name,
                          unsigned int namelen)
{
    int ret = connect(sock, name, (socklen_t)namelen);
    if (ret < 0) {
        iops->error = errno;
    }
    return ret;
}

struct libev_event {
    union {
        struct ev_io io;
        struct ev_timer timer;
    } ev;
    void *data;
    void (*handler)(lcb_socket_t sock, short which, void *cb_data);
};

static void handler_thunk(struct ev_loop *loop, ev_io *io, int events)
{
    struct libev_event *evt = (struct libev_event *)io;
    int which = 0;

    if (events & EV_READ) {
        which |= LCB_READ_EVENT;
    }
    if (events & EV_WRITE) {
        which |= LCB_WRITE_EVENT;
    }
    evt->handler(io->fd, which, evt->data);

    (void)loop;
}

static void *lcb_io_create_event(struct lcb_io_opt_st *iops)
{
    struct libev_event *event = calloc(1, sizeof(*event));
    (void)iops;
    return event;
}

static int lcb_io_update_event(struct lcb_io_opt_st *iops,
                               lcb_socket_t sock,
                               void *event,
                               short flags,
                               void *cb_data,
                               void (*handler)(lcb_socket_t sock,
                                               short which,
                                               void *cb_data))
{
    struct libev_cookie *io_cookie = iops->cookie;
    struct libev_event *evt = event;
    int events = EV_NONE;

    if (flags & LCB_READ_EVENT) {
        events |= EV_READ;
    }
    if (flags & LCB_WRITE_EVENT) {
        events |= EV_WRITE;
    }

    if (events == evt->ev.io.events && handler == evt->handler) {
        /* no change! */
        return 0;
    }

    ev_io_stop(io_cookie->loop, &evt->ev.io);
    evt->data = cb_data;
    evt->handler = handler;
    ev_init(&evt->ev.io, handler_thunk);
    ev_io_set(&evt->ev.io, sock, events);
    ev_io_stop(io_cookie->loop, &evt->ev.io);
    ev_io_start(io_cookie->loop, &evt->ev.io);

    return 0;
}
static void lcb_io_delete_event(struct lcb_io_opt_st *iops,
                                lcb_socket_t sock,
                                void *event)
{
    struct libev_cookie *io_cookie = iops->cookie;
    struct libev_event *evt = event;
    ev_io_stop(io_cookie->loop, &evt->ev.io);
    (void)sock;
}

static void lcb_io_destroy_event(struct lcb_io_opt_st *iops,
                                 void *event)
{
    lcb_io_delete_event(iops, -1, event);
    free(event);
}

static int lcb_io_update_timer(struct lcb_io_opt_st *iops,
                               void *timer,
                               lcb_uint32_t usec,
                               void *cb_data,
                               void (*handler)(lcb_socket_t sock,
                                               short which,
                                               void *cb_data))
{
    struct libev_cookie *io_cookie = iops->cookie;
    struct libev_event *evt = timer;

#ifdef HAVE_LIBEV4
    if (evt->handler == handler && evt->ev.io.events == EV_TIMER) {
#else
    if (evt->handler == handler && evt->ev.io.events == EV_TIMEOUT) {
#endif
        /* no change! */
        return 0;
    }
    evt->data = cb_data;
    evt->handler = handler;
    ev_init(&evt->ev.io, handler_thunk);
    evt->ev.timer.repeat = usec;
    ev_timer_again(io_cookie->loop, &evt->ev.timer);

    return 0;
}

static void lcb_io_delete_timer(struct lcb_io_opt_st *iops,
                                void *event)
{
    struct libev_cookie *io_cookie = iops->cookie;
    struct libev_event *evt = event;
    ev_timer_stop(io_cookie->loop, &evt->ev.timer);
}

static void lcb_io_destroy_timer(struct lcb_io_opt_st *iops,
                                 void *event)
{
    lcb_io_delete_timer(iops, event);
    free(event);
}

static void lcb_io_stop_event_loop(struct lcb_io_opt_st *iops)
{
    struct libev_cookie *io_cookie = iops->cookie;
#ifdef HAVE_LIBEV4
    ev_break(io_cookie->loop, EVBREAK_ONE);
#else
    ev_unloop(io_cookie->loop, EVUNLOOP_ONE);
#endif
}

static void lcb_io_run_event_loop(struct lcb_io_opt_st *iops)
{
    struct libev_cookie *io_cookie = iops->cookie;
#ifdef HAVE_LIBEV4
    ev_run(io_cookie->loop, 0);
#else
    ev_loop(io_cookie->loop, 0);
#endif
}

static void lcb_destroy_io_opts(struct lcb_io_opt_st *iops)
{
    struct libev_cookie *io_cookie = iops->cookie;
    if (io_cookie->allocated) {
        ev_loop_destroy(io_cookie->loop);
    }
    free(io_cookie);
    free(iops);
}

LIBCOUCHBASE_API
struct lcb_io_opt_st *lcb_create_libev_io_opts(struct ev_loop *loop) {
    struct lcb_io_opt_st *ret = calloc(1, sizeof(*ret));
    struct libev_cookie *cookie = calloc(1, sizeof(*cookie));
    if (ret == NULL || cookie == NULL) {
        free(ret);
        free(cookie);
        return NULL;
    }

    /* setup io iops! */
    ret->version = 1;
    ret->dlhandle = NULL;
    ret->recv = lcb_io_recv;
    ret->send = lcb_io_send;
    ret->recvv = lcb_io_recvv;
    ret->sendv = lcb_io_sendv;
    ret->socket = lcb_io_socket;
    ret->close = lcb_io_close;
    ret->connect = lcb_io_connect;
    ret->delete_event = lcb_io_delete_event;
    ret->destroy_event = lcb_io_destroy_event;
    ret->create_event = lcb_io_create_event;
    ret->update_event = lcb_io_update_event;

    ret->delete_timer = lcb_io_delete_timer;
    ret->destroy_timer = lcb_io_destroy_timer;
    ret->create_timer = lcb_io_create_event;
    ret->update_timer = lcb_io_update_timer;

    ret->run_event_loop = lcb_io_run_event_loop;
    ret->stop_event_loop = lcb_io_stop_event_loop;
    ret->destructor = lcb_destroy_io_opts;

    if (loop == NULL) {
        if ((cookie->loop = ev_loop_new(EVFLAG_AUTO | EVFLAG_NOENV)) == NULL) {
            free(ret);
            free(cookie);
            return NULL;
        }
        cookie->allocated = 1;
    } else {
        cookie->loop = loop;
        cookie->allocated = 0;
    }
    ret->cookie = cookie;

    return ret;
}
