/*
 *  Licensed to the Apache Software Foundation (ASF) under one or more
 *  contributor license agreements.  See the NOTICE file distributed with
 *  this work for additional information regarding copyright ownership.
 *  The ASF licenses this file to You under the Apache License, Version 2.0
 *  (the "License"); you may not use this file except in compliance with
 *  the License.  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

/***************************************************************************
 * Description: Simple memory pool                                         *
 * Author:      Gal Shachor <shachor@il.ibm.com>                           *
 * Version:     $Revision: 1305768 $                                       *
 ***************************************************************************/

#include "jk_pool.h"

#define DEFAULT_DYNAMIC 10

void jk_open_pool(jk_pool_t *p, jk_pool_atom_t *buf, size_t size)
{
    p->pos = 0;
    p->size = size;
    p->buf = (char *)buf;

    p->dyn_pos = 0;
    p->dynamic = NULL;
    p->dyn_size = 0;
}

void jk_close_pool(jk_pool_t *p)
{
    jk_reset_pool(p);
    if (p->dynamic) {
        free(p->dynamic);
    }
}

void jk_reset_pool(jk_pool_t *p)
{
    if (p->dyn_pos && p->dynamic) {
        size_t i;
        for (i = 0; i < p->dyn_pos; i++) {
            if (p->dynamic[i]) {
                free(p->dynamic[i]);
            }
        }
    }

    p->dyn_pos = 0;
    p->pos = 0;
}

void *jk_pool_alloc(jk_pool_t *p, size_t size)
{
    void *rc = NULL;

    if (size == 0)
        return NULL;
    size = JK_ALIGN_DEFAULT(size);
    if ((p->size - p->pos) >= size) {
        rc = &(p->buf[p->pos]);
        p->pos += size;
    }
    else {
        if (p->dyn_size == p->dyn_pos) {
            size_t new_dyn_size = p->dyn_size * 2 + DEFAULT_DYNAMIC;
            void **new_dynamic = (void **)realloc(p->dynamic,
                                                new_dyn_size * sizeof(void *));
            if (new_dynamic) {
                p->dynamic = new_dynamic;
                p->dyn_size = new_dyn_size;
            }
            else {
                return NULL;
            }
        }

        rc = p->dynamic[p->dyn_pos] = malloc(size);
        if (p->dynamic[p->dyn_pos]) {
            p->dyn_pos++;
        }
    }
    return rc;
}

void *jk_pool_calloc(jk_pool_t *p, size_t size)
{
    void *rc = jk_pool_alloc(p, size);
    if (rc)
        memset(rc, 0, size);
    return rc;
}

void *jk_pool_realloc(jk_pool_t *p, size_t sz, const void *old, size_t old_sz)
{
    char *rc;

    if (!p || (sz < old_sz)) {
        return NULL;
    }
    if (!old)
        return jk_pool_calloc(p, sz);
    rc = (char *)jk_pool_alloc(p, sz);
    if (rc) {
        memcpy(rc, old, old_sz);
        memset(rc + old_sz, 0, sz - old_sz);
    }

    return rc;
}

char *jk_pool_strdup(jk_pool_t *p, const char *s)
{
    char *rc = NULL;
    if (s && p) {
        size_t size = strlen(s);

        if (!size) {
            return "";
        }

        size++;
        rc = jk_pool_alloc(p, size);
        if (rc) {
            memcpy(rc, s, size);
        }
    }

    return rc;
}

char *jk_pool_strcat(jk_pool_t *p, const char *s, const char *a)
{
    char *rc = NULL;

    if (s && a && p) {
        size_t szs = strlen(s);
        size_t sza = strlen(a);
        if ((szs + sza) == 0) {
            return "";
        }
        rc = jk_pool_alloc(p, szs + sza + 1);
        if (rc) {
            memcpy(rc, s, szs);
            memcpy(rc + szs, a, sza + 1);
        }
    }

    return rc;
}

char *jk_pool_strcatv(jk_pool_t *p, ...)
{
    char *cp;
    char *rc = NULL;
    va_list ap;

    if (p) {
        char   *str;
        size_t size = 0;
        va_start(ap, p);
        while ((str = va_arg(ap, char *)) != 0) {
            size += strlen(str);
        }
        va_end(ap);
        if (size == 0) {
            return "";
        }
        size++;
        cp = rc = jk_pool_alloc(p, size);
        if (rc) {
            size_t len = 0;
            va_start(ap, p);
            while ((str = va_arg(ap, char *)) != 0) {
                len = strlen(str);
                memcpy(cp, str, len);
                cp += len;
            }
            va_end(ap);
            *cp = '\0';
        }
    }

    return rc;
}
