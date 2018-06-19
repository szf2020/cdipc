/*
 * Software License Agreement (MIT License)
 *
 * Copyright (c) 2018, DUKELEC, Inc.
 * All rights reserved.
 *
 * Author: Duke Fong <duke@dukelec.com>
 */

#include <pthread.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <assert.h>

#include "cdipc.h"

int cdipc_create(const char *name, cdipc_type_t type,
        int max_pub, int max_sub, int max_nd, size_t max_len)
{
    int i, r;
    cdipc_ch_t _ch;
    cdipc_ch_t *ch = &_ch;

    if (!max_pub || !max_sub || !max_nd || !max_len) {
        dnf_error(name, "zero arguments detected\n");
        return -1;
    }
    if (type == CDIPC_SERVICE && max_sub != 1) {
        dnf_error(name, "max_sub must be 1 for service\n");
        return -1;
    }

    memset(ch, 0, sizeof(cdipc_ch_t));
    ch->map_len = sizeof(cdipc_hdr_t) +
            sizeof(cdipc_pub_t) * max_pub +
            sizeof(cdipc_sub_t) * max_sub +
            (sizeof(cdipc_nd_t) + max_len) * max_nd;
    strncpy(ch->name, name, NAME_MAX);

    ch->fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR | O_TRUNC, S_IRWXU | S_IRWXG);
    if (ch->fd < 0) {
        dnf_error(ch->name, "shm_open: %s, ret code: %d\n", name, ch->fd);
        return ch->fd;
    }

    if (ftruncate(ch->fd, ch->map_len) == -1) {
        dnf_error(ch->name, "ftruncate to size %ld\n", ch->map_len);
        return -1;
    }

    ch->hdr = (cdipc_hdr_t*) mmap(NULL, ch->map_len,
            PROT_READ | PROT_WRITE, MAP_SHARED, ch->fd, 0);
    if (ch->hdr == MAP_FAILED) {
        dnf_error(ch->name, "mmap\n");
        return -1;
    }

    memset(ch->hdr, 0, sizeof(cdipc_hdr_t));
    ch->hdr->type = type;
    ch->hdr->max_pub = max_pub;
    ch->hdr->max_sub = max_sub;
    ch->hdr->max_nd = max_nd;
    ch->hdr->max_len = max_len;

    pthread_mutexattr_t mutexattr;
    if ((r = pthread_mutexattr_setpshared(&mutexattr, PTHREAD_PROCESS_SHARED))) {
        dnf_error(ch->name, "pthread_mutexattr_setpshared\n");
        return -1;
    }
#ifdef HAVE_MUTEX_PRIORITY_INHERIT
    if ((r = pthread_mutexattr_setprotocol(&mutexattr, PTHREAD_PRIO_INHERIT))) {
        dnf_error(ch->name, "pthread_mutexattr_setprotocol");
        return -1;
    }
#endif
#ifdef HAVE_MUTEX_ROBUST
    if ((r = pthread_mutexattr_setrobust(&mutexattr, PTHREAD_MUTEX_ROBUST))) {
        dnf_error(ch->name, "pthread_mutexattr_setrobust");
        return -1;
    }
#endif
#if defined (HAVE_MUTEX_ERROR_CHECK) && defined (DEBUG)
    if ((r = pthread_mutexattr_settype(&mutexattr, PTHREAD_MUTEX_ERRORCHECK))) {
        dnf_error(ch->name, "pthread_mutexattr_settype");
        return -1;
    }
#endif
    if ((r = pthread_mutex_init(&ch->hdr->mutex, &mutexattr))) {
        dnf_error(ch->name, "pthread_mutex_init\n");
        return -1;
    }
    if ((r = pthread_mutexattr_destroy(&mutexattr))) {
        dnf_error(ch->name, "pthread_mutexattr_destroy\n");
        return -1;
    }

    pthread_condattr_t condattr;
    if ((r = pthread_condattr_setpshared(&condattr, PTHREAD_PROCESS_SHARED))) {
        dnf_error(ch->name, "pthread_condattr_setpshared\n");
        return -1;
    }
    if ((r = pthread_condattr_setclock(&condattr, CLOCK_MONOTONIC))) {
        dnf_error(ch->name, "pthread_condattr_setclock\n");
        return -1;
    }
    if ((r = pthread_cond_init(&ch->hdr->cond, &condattr))) {
        dnf_error(ch->name, "pthread_cond_init\n");
        return -1;
    }
    if ((r = pthread_condattr_destroy(&condattr))) {
        dnf_error(ch->name, "pthread_condattr_destroy\n");
        return -1;
    }

    ch->pubs = (void *)ch->hdr + sizeof(cdipc_hdr_t);
    ch->subs = (void *)ch->pubs + sizeof(cdipc_pub_t) * max_pub;
    cdipc_nd_t *nds = (void *)ch->subs + sizeof(cdipc_sub_t) * max_sub;

    for (i = 0; i < max_pub; i++) {
        cdipc_pub_t *pub = ch->pubs + i;
        memset(pub, 0, sizeof(cdipc_pub_t));
        pub->id = i;
    }
    for (i = 0; i < max_sub; i++) {
        cdipc_sub_t *sub = ch->subs + i;
        memset(sub, 0, sizeof(cdipc_sub_t));
        sub->id = i;
    }
    for (i = 0; i < max_nd; i++) {
        cdipc_nd_t *nd = (void *)nds + (sizeof(cdipc_nd_t) + max_len) * i;
        memset(nd, 0, sizeof(cdipc_nd_t));
        nd->id = i;
        list_put(&ch->hdr->free, &nd->node);
    }

    ch->hdr->magic = CDIPC_MAGIC_NUM;
    return cdipc_close(ch);
}

int cdipc_unlink(const char *name)
{
    int r;

    if ((r = shm_unlink(name))) {
        dnf_error(name, "shm_unlink\n");
        return -1;
    }
    return 0;
}


int cdipc_open(cdipc_ch_t *ch, const char *name,
        cdipc_role_t role, int id)
{
    memset(ch, 0, sizeof(cdipc_ch_t));
    strncpy(ch->name, name, NAME_MAX);

    ch->fd = shm_open(name, O_RDWR, S_IRWXU | S_IRWXG);
    if (ch->fd < 0) {
        dnf_error(ch->name, "shm_open, ret code: %d\n", ch->fd);
        return ch->fd;
    }

    ch->hdr = (cdipc_hdr_t*) mmap(NULL, sizeof(cdipc_hdr_t),
            PROT_READ | PROT_WRITE, MAP_SHARED, ch->fd, 0);
    if (ch->hdr == MAP_FAILED) {
        dnf_error(ch->name, "mmap\n");
        return -1;
    }
    if (ch->hdr->magic != CDIPC_MAGIC_NUM) {
        dnf_error(ch->name, "wrong magic num: %08x, expect: %08x\n",
                ch->hdr->magic, CDIPC_MAGIC_NUM);
        return -1;
    }

    ch->map_len = sizeof(cdipc_hdr_t) +
            sizeof(cdipc_pub_t) * ch->hdr->max_pub +
            sizeof(cdipc_sub_t) * ch->hdr->max_sub +
            (sizeof(cdipc_nd_t) + ch->hdr->max_len) * ch->hdr->max_nd;

    if (-1 == munmap(ch->hdr, sizeof(cdipc_hdr_t))) {
        dnf_error(ch->name, "munmap\n");
        return -1;
    }

    ch->hdr = (cdipc_hdr_t*) mmap(NULL, ch->map_len,
            PROT_READ | PROT_WRITE, MAP_SHARED, ch->fd, 0);
    if (ch->hdr == MAP_FAILED) {
        dnf_error(ch->name, "re-mmap\n");
        return -1;
    }

    ch->pubs = (void *)ch->hdr + sizeof(cdipc_hdr_t);
    ch->subs = (void *)ch->pubs + sizeof(cdipc_pub_t) * ch->hdr->max_pub;
    ch->role = role;

    if (role == CDIPC_PUB) {
        ch->pub = ch->pubs + id;
    } else if (role == CDIPC_SUB) {
        ch->sub = ch->subs + id;
    }

    return 0;
}

int cdipc_close(cdipc_ch_t *ch)
{
    if (munmap(ch->hdr, ch->map_len)) {
        dnf_error(ch->name, "munmap\n");
        return -1;
    }
    ch->hdr = NULL;

    if (close(ch->fd)) {
        dnf_error(ch->name, "close fd\n");
        return -1;
    }
    ch->fd = -1;
    return 0;
}


// for pub:

int cdipc_pub_alloc(cdipc_ch_t *ch, const struct timespec *abstime)
{
    int r = 0;
    cdipc_hdr_t *hdr = ch->hdr;
    cdipc_pub_t *pub = ch->pub;
    assert(ch->role == CDIPC_PUB);

    if (pub->cur) {
        df_warn("aready allocated\n");
        pub->cur->owner = pub->id;
        return 0;
    }
    if (hdr->type == CDIPC_SERVICE && pub->ans) {
        df_warn("unexpected ans\n");
        pub->cur = pub->ans;
        pub->ans = NULL;
        pub->cur->owner = pub->id;
        return 0;
    }

    pthread_mutex_lock(&hdr->mutex);
    while (!(pub->cur = list_get_entry(&hdr->free, cdipc_nd_t))) {
        r = pthread_cond_timedwait(&hdr->cond, &hdr->mutex, abstime);
        if (r == ETIMEDOUT) {
            break;
        } else if (r != 0) {
            dnf_error(ch->name, "cond_timedwait, ret: %d\n", r);
            break;
        }
    }
    if (pub->cur)
        pub->cur->owner = pub->id;
    pthread_mutex_unlock(&hdr->mutex);
    return r;
}

int cdipc_pub_put(cdipc_ch_t *ch, const struct timespec *abstime)
{
    int i, r = 0;
    cdipc_hdr_t *hdr = ch->hdr;
    cdipc_pub_t *pub = ch->pub;
    assert(ch->role == CDIPC_PUB);

    if (!pub->cur) {
        dnf_error(ch->name, "cur empty\n");
        return -1;
    }

    pthread_mutex_lock(&hdr->mutex);

    while (true) {
        bool need_wait = false;
        for (i = 0; i < hdr->max_sub; i++) {
            cdipc_sub_t *sub = ch->subs + i;
            if (sub->need_wait && sub->pend.len == sub->max_len) {
                need_wait = true;
                break;
            }
        }
        if (!need_wait)
            break;

        r = pthread_cond_timedwait(&hdr->cond, &hdr->mutex, abstime);
        if (r == ETIMEDOUT) {
            break;
        } else if (r != 0) {
            dnf_error(ch->name, "cond_timedwait, ret: %d\n", r);
            break;
        }
    }

    if (r == 0) {
        pub->cur->ref = 0;
        for (i = 0; i < hdr->max_sub; i++) {
            cdipc_sub_t *sub = ch->subs + i;
            if (sub->pend.len == sub->max_len) {
                cdipc_nd_t *nd = list_get_entry(&sub->pend, cdipc_nd_t);
                nd->owner = -1;
                list_put(&hdr->free, &nd->node);
            }
            if (sub->max_len != 0) {
                list_put(&sub->pend, &pub->cur->node);
                pub->cur->ref++;
            }
        }
        pub->cur = NULL;
    }

    pthread_mutex_unlock(&hdr->mutex);

    return r;
}

int cdipc_pub_get(cdipc_ch_t *ch, const struct timespec *abstime)
{
    int i, r = 0;
    cdipc_hdr_t *hdr = ch->hdr;
    cdipc_pub_t *pub = ch->pub;
    assert(ch->role == CDIPC_PUB);

    pthread_mutex_lock(&hdr->mutex);

    while (!pub->ans) {
        r = pthread_cond_timedwait(&hdr->cond, &hdr->mutex, abstime);
        if (r == ETIMEDOUT) {
            break;
        } else if (r != 0) {
            dnf_error(ch->name, "cond_timedwait, ret: %d\n", r);
            break;
        }
    }

    pthread_mutex_unlock(&hdr->mutex);
    return r;
}

int cdipc_pub_free(cdipc_ch_t *ch)
{
    cdipc_hdr_t *hdr = ch->hdr;
    cdipc_pub_t *pub = ch->pub;
    assert(ch->role == CDIPC_PUB && hdr->type == CDIPC_SERVICE);

    if (!pub->ans) {
        dnf_error(ch->name, "pub->ans empty\n");
        return -1;
    }
    pthread_mutex_lock(&hdr->mutex);
    pub->ans->owner = -1;
    list_put(&hdr->free, &pub->ans->node);
    pub->ans = NULL;
    pthread_mutex_unlock(&hdr->mutex);
    return 0;
}

// for sub:

int cdipc_sub_get(cdipc_ch_t *ch, const struct timespec *abstime)
{
    int i, r = 0;
    cdipc_hdr_t *hdr = ch->hdr;
    cdipc_sub_t *sub = ch->sub;
    assert(ch->role == CDIPC_SUB);

    pthread_mutex_lock(&hdr->mutex);

pick_node:
    while (!(sub->cur = list_get_entry(&sub->pend, cdipc_nd_t))) {
        r = pthread_cond_timedwait(&hdr->cond, &hdr->mutex, abstime);
        if (r == ETIMEDOUT) {
            break;
        } else if (r != 0) {
            dnf_error(ch->name, "cond_timedwait, ret: %d\n", r);
            break;
        }
    }

    if (r == 0 && sub->cur->owner < 0) {
        if (--sub->cur->ref <= 0) {
            list_put(&hdr->free, &sub->cur->node);
        }
        sub->cur = NULL;
        df_debug("avoid cancelled node\n");
        goto pick_node;
    }

    pthread_mutex_unlock(&hdr->mutex);
    return r;
}

int cdipc_sub_ret(cdipc_ch_t *ch)
{
    int i, r = 0;
    cdipc_hdr_t *hdr = ch->hdr;
    cdipc_sub_t *sub = ch->sub;
    assert(ch->role == CDIPC_SUB && hdr->type == CDIPC_SERVICE);

    if (!sub->cur) {
        dnf_error(ch->name, "cur empty\n");
        return -1;
    }

    pthread_mutex_lock(&hdr->mutex);

    if (sub->cur->owner < 0) {
        df_debug("avoid cancelled node\n");
        if (--sub->cur->ref != 0)
            df_warn("ref not zero: %d\n", sub->cur->ref);
        list_put(&hdr->free, &sub->cur->node);
        sub->cur = NULL;
    } else {
        cdipc_pub_t *pub = ch->pubs + sub->cur->owner;
        if (pub->ans) {
            dnf_error(ch->name, "pub->ans not empty\n");
            r = -1;
        } else {
            pub->ans = sub->cur;
            sub->cur = NULL;
        }
    }

    pthread_mutex_unlock(&hdr->mutex);
    return r;
}

int cdipc_sub_free(cdipc_ch_t *ch)
{
    cdipc_hdr_t *hdr = ch->hdr;
    cdipc_sub_t *sub = ch->sub;
    assert(ch->role == CDIPC_SUB && hdr->type == CDIPC_TOPIC);

    if (!sub->cur) {
        dnf_error(ch->name, "sub->cur empty\n");
        return -1;
    }
    pthread_mutex_lock(&hdr->mutex);
    if (--sub->cur->ref <= 0) {
        list_put(&hdr->free, &sub->cur->node);
    }
    sub->cur = NULL;
    pthread_mutex_unlock(&hdr->mutex);
    return 0;
}