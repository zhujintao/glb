/*
 * Copyright (C) 2008 Codership Oy <info@codership.com>
 *
 * $Id$
 */

#include <stdlib.h>
#include <sys/select.h> // for select() and FD_SET
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>

#include "glb_pool.h"

typedef enum pool_ctl_code
{
    POOL_CTL_ADD_CONN,
    POOL_CTL_DEL_DST,
    POOL_CTL_SHUTDOWN,
    POOL_CTL_MAX
} pool_ctl_code_t;

typedef struct pool_ctl
{
    pool_ctl_code_t code;
    void*           data;
} pool_ctl_t;

typedef struct pool_conn_end
{
    bool            inc;      // to differentiate between the ends
    int             sock;     // fd of connection
    size_t          sent;
    size_t          total;
    glb_sockaddr_t  dst_addr; // destinaiton id
    uint8_t         buf[];    // has pool_buf_size
} pool_conn_end_t;

// we want to allocate memory for both ends in one malloc() call and have it
// nicely aligned.
#define pool_conn_size 4096 // presumably a page and should be enough for
                            // two ethernet frames (what about jumbo?)
const size_t pool_buf_size  = (pool_conn_size/2 - sizeof(pool_conn_end_t));

typedef struct pool
{
    long            id;
    pthread_t       thread;
    int             ctl_recv; // receive commands - pool thread
    int             ctl_send; // send commands to pool - other function
    volatile ulong  n_conns;  // how many connecitons this pool serves
    fd_set          fds_read;
    fd_set          fds_write;
    int             fd_max;
    int             fd_min;
    glb_router_t*   router;
    pool_conn_end_t* route_map[FD_SETSIZE]; // looking for connection ctx by fd
} pool_t;

struct glb_pool
{
    pthread_mutex_t lock;
    ulong           n_pools;
    pool_t          pool[];  // pool array, can't be changed in runtime
};

static inline void
pool_set_conn_end (pool_t* pool, pool_conn_end_t* end1, pool_conn_end_t* end2)
{
    assert (NULL == pool->route_map[end1->sock]);
    pool->route_map[end1->sock] = end2;
    if (end1->sock > pool->fd_max) pool->fd_max = end1->sock;
    if (end1->sock < pool->fd_min) pool->fd_min = end1->sock;
    FD_SET (end1->sock, &pool->fds_read);
}

static long
pool_handle_ctl (pool_t* pool, pool_ctl_t* ctl)
{
    switch (ctl->code) {
    case POOL_CTL_ADD_CONN:
    {
        pool_conn_end_t* inc_end = ctl->data;
        pool_conn_end_t* dst_end = ctl->data + pool_conn_size/2;

        pool_set_conn_end (pool, inc_end, dst_end);
        pool_set_conn_end (pool, dst_end, inc_end);

        pool->n_conns++; // update connection count
    }
    break;
    default: // nothing else is implemented
        fprintf (stderr, "Unsupported CTL: %d\n", ctl->code);
    }

    return 0;
}

// removing traces of connection end - reverse to what pool_set_conn_end() did
static inline void
pool_reset_conn_end (pool_t* pool, int fd)
{
    int i;

    FD_CLR (fd, &pool->fds_read);
    FD_CLR (fd, &pool->fds_write);
    pool->route_map[fd] = NULL;

    if (fd == pool->fd_max) {
        // fd_max can't be less than pool->ctl_recv, because of select()
        for (i = fd - 1; i > pool->ctl_recv; i--) {
            if (pool->route_map[i] != NULL) break;
        }
        pool->fd_max = i;
    }

    if (fd == pool->fd_min) {
        for (i = fd + 1; i < pool->fd_max; i++) {
            if (pool->route_map[i] != NULL) break;
        }
        pool->fd_min = i;
    }
}

static void
pool_remove_conn (pool_t* pool, int src_fd)
{
    pool_conn_end_t* end    = pool->route_map[src_fd];
    int              dst_fd = end->sock;

    close (dst_fd);
    pool->n_conns--;
    glb_router_disconnect (pool->router, &end->dst_addr);

    if (end->inc) {
        free (end); // frees both ends
    }
    else {
        assert (pool->route_map[dst_fd]->inc);
        free (pool->route_map[dst_fd]);
    }

    pool_reset_conn_end (pool, src_fd);
    pool_reset_conn_end (pool, dst_fd);
}

static inline ssize_t
pool_send_data (pool_t* pool, pool_conn_end_t* dst)
{
    ssize_t ret;

    ret = write (dst->sock, &dst->buf[dst->sent], dst->total - dst->sent);
    if (ret >= 0) {
        dst->sent += ret;
        if (dst->sent == dst->total) { // all data sent, reset pointers
            dst->sent =  dst->total = 0;
            FD_CLR (dst->sock, &pool->fds_write);
        }
        else { // hm, will it work? - remind to send later
            FD_SET (dst->sock, &pool->fds_write);
        }
    }

    return ret;
}

// inline because frequent
static inline ssize_t
pool_handle_read (pool_t* pool, int src_fd)
{
    ssize_t ret = 0;
    pool_conn_end_t* dst = pool->route_map[src_fd];

    // first, try read data from source, if there's enough space
    if (dst->total < pool_buf_size) {
        ret = read (src_fd, &dst->buf[dst->total], pool_buf_size - dst->total);
        if (ret > 0) {
            dst->total += ret;
            // now try to send whatever we have
            if (pool_send_data (pool, dst) < 0) {
                // probably don't care what error is
                perror ("pool_handle_read(): sending data error");
            }
        }
        else {
            if (0 == ret) { // socket closed, must close another end and cleanup
                pool_remove_conn (pool, src_fd);
                ret = -1;
            }
            else { // some other error
                perror ("pool_handle_read(): receiving data error");
            }
        }
    }
    return ret;
}

static inline ssize_t
pool_handle_write (pool_t* pool, int dst_fd)
{
    register int     src_fd = pool->route_map[dst_fd]->sock;
    pool_conn_end_t* dst    = pool->route_map[src_fd];

    if (dst->total) {
        assert (dst->total > dst->sent);
        if (pool_send_data (pool, dst) < 0) {
            // probably don't care what error is
            perror ("pool_handle_read(): sending data error");
        }
    }
    return 0;
}

static void*
pool_thread (void* arg)
{
    pool_t* pool = arg;
    struct timeval timeout = { 1, 0 }; // 1 second
    struct timeval tmp;

    while (1) {
        long ret;
        fd_set fds_read, fds_write;

        tmp = timeout;
        memcpy (&fds_read,  &pool->fds_read,  sizeof (fd_set));
        memcpy (&fds_write, &pool->fds_write, sizeof (fd_set));
        ret = select (pool->fd_max+1, &fds_read, &fds_write,
                      NULL, &tmp);

        if (ret > 0) { // we have some input
            long count = ret;
            int  fd    = pool->fd_min;
            // first, check ctl pipe
            if (FD_ISSET (pool->ctl_recv, &fds_read)) {
                pool_ctl_t ctl;

                ret = read (pool->ctl_recv, &ctl, sizeof(ctl));
                if (sizeof(ctl) == ret) { // complete ctl read
                    pool_handle_ctl (pool, &ctl);
                }
                else { // should never happen!
                    perror ("read from ctl");
                    abort();
                }
                // pool->ctl_recv can theoretically get between fd_min and
                // fd_max.
                // For simplicity we want to assume that it is not set when
                // processing normal fds
                // also set of connections could be modified, start over
                continue;
            }

            assert (!FD_ISSET (pool->ctl_recv, &fds_read));

            // check remaining connections
            while (count) {
                assert (fd <= pool->fd_max);

                if (FD_ISSET (fd, &fds_read)) {
                    if (pool_handle_read (pool, fd) < 0) continue;
                    count--;
                }

                if (FD_ISSET (fd, &fds_write)) {
                    if (pool_handle_write (pool, fd) < 0) continue;
                    count--;
                }

                fd++;
            }
        }
        else if (-1 == ret) {
            perror ("select() failed");
        }
        else {
            // timed out
            printf ("Thread %ld is idle\n", pool->id);
        }
    }

    return NULL;
}

static long
pool_init (pool_t* pool, long id, glb_router_t* router)
{
    long ret;
    int pipe_fds[2];

    pool->id     = id;
    pool->router = router;

    ret = pipe(pipe_fds);
    if (ret) {
        perror ("Failed to open control pipe");
        return -ret;
    }

    pool->ctl_recv = pipe_fds[0];
    pool->ctl_send = pipe_fds[1];

    FD_ZERO (&pool->fds_read);
    FD_ZERO (&pool->fds_write);
    FD_SET  (pool->ctl_recv, &pool->fds_read);
    pool->fd_max = pool->ctl_recv;
    pool->fd_min = pool->fd_max;

    // FIXME: is this a race (pool->thread and pool)?
    ret = pthread_create (&pool->thread, NULL, pool_thread, pool);
    if (ret) {
        perror ("Failed to create thread");
        return -ret;
    }

    return 0;
}

extern glb_pool_t*
glb_pool_create (size_t n_pools, glb_router_t* router)
{
    size_t ret_size = sizeof(glb_pool_t) + n_pools * sizeof(pool_t);
    glb_pool_t* ret = malloc (ret_size);

    if (ret) {
        long err;
        long i;

        memset (ret, 0, ret_size);
        pthread_mutex_init (&ret->lock, NULL);
        ret->n_pools = n_pools;

        for (i = 0; i < n_pools; i++) {
            if ((err = pool_init(&ret->pool[i], i, router))) {
                fprintf (stderr, "Failed to initialize pool %ld\n", i);
                abort();
            }
        }
    }
    else {
        fprintf (stderr, "Could not allocate memory for %zu pools\n", n_pools);
        abort();
    }

    return ret;
}

extern void
glb_pool_destroy (glb_pool_t* pool)
{
    fprintf (stderr, "glb_pool_destroy() not implemented yet!");
}

// finds the least busy pool
static inline pool_t*
pool_get_pool (glb_pool_t* pool)
{
    pool_t* ret     = pool->pool;
    ulong min_conns = ret->n_conns;
    register ulong i;

    for (i = 1; i < pool->n_pools; i++) {
        if (min_conns > pool->pool[i].n_conns) {
            min_conns = pool->pool[i].n_conns;
            ret = pool->pool + i;
        }
    }
    return ret;
}

extern long
glb_pool_add_conn (glb_pool_t*     pool,
                   int             inc_sock,
                   int             dst_sock,
                   glb_sockaddr_t* dst_addr)
{
    pool_t* p     = pool_get_pool (pool);
    long    ret   = -ENOMEM;
    void*   route = NULL;

    if (pthread_mutex_lock (&pool->lock)) {
        perror ("glb_pool_add_conn: failed to lock mutex");
        abort();
    }

    route = malloc (pool_conn_size);
    if (route) {
        pool_conn_end_t* inc_end = route;
        pool_conn_end_t* dst_end = route + pool_conn_size / 2;
        pool_ctl_t       add_conn_ctl = { POOL_CTL_ADD_CONN, route };

        inc_end->inc      = true;
        inc_end->sock     = inc_sock;
        inc_end->sent     = 0;
        inc_end->total    = 0;
        inc_end->dst_addr = *dst_addr; // needed for cleanups

        dst_end->inc      = false;
        dst_end->sock     = dst_sock;
        dst_end->sent     = 0;
        dst_end->total    = 0;
        dst_end->dst_addr = *dst_addr; // needed for cleanups

        ret = write (p->ctl_send, &add_conn_ctl, sizeof (add_conn_ctl));
        if (ret != sizeof (add_conn_ctl)) {
            perror ("Sending add_conn_ctl failed");
            if (ret > 0) abort(); // partial ctl was sent, don't know what to do
        }
        else ret = 0;
    }

    pthread_mutex_unlock (&pool->lock);

    return ret;
}

extern long
glb_pool_drop_dst (glb_pool_t* pool, const glb_sockaddr_t* dst)
{
    return 0;
}

