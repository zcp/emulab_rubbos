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
 * Description: URI to worker map object.                                  *
 *                                                                         *
 * Author:      Gal Shachor <shachor@il.ibm.com>                           *
 * Author:      Mladen Turk <mturk@apache.org>                             *
 * Version:     $Revision: 1840607 $                                          *
 ***************************************************************************/

#include "jk_pool.h"
#include "jk_util.h"
#include "jk_map.h"
#include "jk_mt.h"
#include "jk_uri_worker_map.h"
#include "jk_worker.h"
#include "jk_lb_worker.h"

#ifdef WIN32
#define JK_STRCMP   strcasecmp
#define JK_STRNCMP  strnicmp
#else
#define JK_STRCMP   strcmp
#define JK_STRNCMP  strncmp
#endif

#define JK_UWMAP_EXTENSION_REPLY_TIMEOUT       "reply_timeout="
#define JK_UWMAP_EXTENSION_STICKY_IGNORE       "sticky_ignore="
#define JK_UWMAP_EXTENSION_STATELESS           "stateless="
#define JK_UWMAP_EXTENSION_ACTIVE              "active="
#define JK_UWMAP_EXTENSION_DISABLED            "disabled="
#define JK_UWMAP_EXTENSION_STOPPED             "stopped="
#define JK_UWMAP_EXTENSION_FAIL_ON_STATUS      "fail_on_status="
#define JK_UWMAP_EXTENSION_USE_SRV_ERRORS      "use_server_errors="
#define JK_UWMAP_EXTENSION_SESSION_COOKIE      "session_cookie="
#define JK_UWMAP_EXTENSION_SESSION_PATH        "session_path="
#define JK_UWMAP_EXTENSION_SET_SESSION_COOKIE  "set_session_cookie="
#define JK_UWMAP_EXTENSION_SESSION_COOKIE_PATH "session_cookie_path="

#define IND_SWITCH(x)                      (((x)+1) % 2)
#define IND_THIS(x)                        ((x)[uw_map->index])
#define IND_NEXT(x)                        ((x)[IND_SWITCH(uw_map->index)])

#define STRNULL_FOR_NULL(x) ((x) ? (x) : "(null)")

static volatile int map_id_counter = 0;

static const char *uri_worker_map_source_type[] = {
    "unknown",
    SOURCE_TYPE_TEXT_WORKERDEF,
    SOURCE_TYPE_TEXT_JKMOUNT,
    SOURCE_TYPE_TEXT_URIMAP,
    SOURCE_TYPE_TEXT_DISCOVER,
    NULL
};


/* Return the string representation of the uwr source */
const char *uri_worker_map_get_source(uri_worker_record_t *uwr, jk_logger_t *l)
{
    return uri_worker_map_source_type[uwr->source_type];
}

/* Return the string representation of the uwr match type */
char *uri_worker_map_get_match(uri_worker_record_t *uwr, char *buf, jk_logger_t *l)
{
    unsigned int match;

    buf[0] = '\0';
    match = uwr->match_type;

    if (match & MATCH_TYPE_DISABLED)
        strcat(buf, "Disabled ");
/* deprecated
    if (match & MATCH_TYPE_STOPPED)
        strcat(buf, "Stopped ");
 */
    if (match & MATCH_TYPE_NO_MATCH)
        strcat(buf, "Unmount ");
    if (match & MATCH_TYPE_EXACT)
        strcat(buf, "Exact");
    else if (match & MATCH_TYPE_WILDCHAR_PATH)
        strcat(buf, "Wildchar");
/* deprecated
    else if (match & MATCH_TYPE_CONTEXT)
        strcat(buf, "Context");
    else if (match & MATCH_TYPE_CONTEXT_PATH)
        strcat(buf, "Context Path");
    else if (match & MATCH_TYPE_SUFFIX)
        strcat(buf, "Suffix");
    else if (match & MATCH_TYPE_GENERAL_SUFFIX)
        return "General Suffix";
 */
    else
        strcat(buf, "Unknown");
    return buf;
}

/*
 * Given context uri, count the number of path tokens.
 *
 * Servlet specification 2.4, SRV.11.1 says

 *   The container will recursively try tomatch the longest
 *   path-prefix. This is done by stepping down the path tree a
 *   directory at a time, using the / character as a path
 *   separator. The longest match determines the servlet selected.
 *
 * The implication seems to be `most uri path elements is most exact'.
 * This is a little helper function to count uri tokens, so we can
 * keep the worker map sorted with most specific first.
 */
static int worker_count_context_uri_tokens(const char * context)
{
    const char * c = context;
    int count = 0;
    while (c && *c) {
        if ('/' == *c++)
            count++;
    }
    return count;
}

static int worker_compare(const void *elem1, const void *elem2)
{
    uri_worker_record_t *e1 = *(uri_worker_record_t **)elem1;
    uri_worker_record_t *e2 = *(uri_worker_record_t **)elem2;
    int e1_tokens = 0;
    int e2_tokens = 0;

    e1_tokens = worker_count_context_uri_tokens(e1->context);
    e2_tokens = worker_count_context_uri_tokens(e2->context);

    if (e1_tokens != e2_tokens) {
        return (e2_tokens - e1_tokens);
    }
    /* given the same number of URI tokens, use character
     * length as a tie breaker
     */
    if(e2->context_len != e1->context_len)
        return ((int)e2->context_len - (int)e1->context_len);

    return ((int)e2->source_type - (int)e1->source_type);
}

static void worker_qsort(jk_uri_worker_map_t *uw_map)
{

   /* Sort remaining args using Quicksort algorithm: */
   qsort((void *)IND_NEXT(uw_map->maps), IND_NEXT(uw_map->size),
         sizeof(uri_worker_record_t *), worker_compare );

}

/* Dump the map contents - only call if debug log is active. */
static void uri_worker_map_dump(jk_uri_worker_map_t *uw_map,
                                const char *reason, jk_logger_t *l)
{
    JK_TRACE_ENTER(l);
    if (uw_map) {
        int i, off;
        if (JK_IS_DEBUG_LEVEL(l)) {
            jk_log(l, JK_LOG_DEBUG, "uri map dump %s: id=%d, index=%d file='%s' reject_unsafe=%d "
                  "reload=%d modified=%d checked=%d",
                   reason, uw_map->id, uw_map->index, STRNULL_FOR_NULL(uw_map->fname),
                   uw_map->reject_unsafe, uw_map->reload, uw_map->modified, uw_map->checked);
        }
        for (i = 0; i <= 1; i++) {
            jk_log(l, JK_LOG_DEBUG, "generation %d: size=%d nosize=%d capacity=%d",
                   i, uw_map->size[i], uw_map->nosize[i], uw_map->capacity[i], uw_map->maps[i]);
        }

        off = uw_map->index;
        for (i = 0; i <= 1; i++) {
            char buf[32];
            int k;
            unsigned int j;
            k = (i + off) % 2;
            for (j = 0; j < uw_map->size[k]; j++) {
                uri_worker_record_t *uwr = uw_map->maps[k][j];
                if (uwr && JK_IS_DEBUG_LEVEL(l)) {
                    jk_log(l, JK_LOG_DEBUG, "%s (%d) map #%d: uri=%s worker=%s context=%s "
                           "source=%s type=%s len=%d",
                           i ? "NEXT" : "THIS", i, j,
                           STRNULL_FOR_NULL(uwr->uri), STRNULL_FOR_NULL(uwr->worker_name),
                           STRNULL_FOR_NULL(uwr->context), STRNULL_FOR_NULL(uri_worker_map_get_source(uwr,l)),
                           STRNULL_FOR_NULL(uri_worker_map_get_match(uwr,buf,l)), uwr->context_len);
                }
            }
        }
    }
    JK_TRACE_EXIT(l);
}


int uri_worker_map_alloc(jk_uri_worker_map_t **uw_map_p,
                         jk_map_t *init_data, jk_logger_t *l)
{
    int i;

    JK_TRACE_ENTER(l);

    if (uw_map_p) {
        int rc;
        jk_uri_worker_map_t *uw_map;
        *uw_map_p = (jk_uri_worker_map_t *)calloc(1, sizeof(jk_uri_worker_map_t));
        uw_map = *uw_map_p;

        JK_INIT_CS(&(uw_map->cs), rc);
        if (rc == JK_FALSE) {
            jk_log(l, JK_LOG_ERROR,
                   "creating thread lock (errno=%d)",
                   errno);
            JK_TRACE_EXIT(l);
            return JK_FALSE;
        }

        jk_open_pool(&(uw_map->p),
                     uw_map->buf, sizeof(jk_pool_atom_t) * BIG_POOL_SIZE);
        for (i = 0; i <= 1; i++) {
            jk_open_pool(&(uw_map->p_dyn[i]),
                         uw_map->buf_dyn[i], sizeof(jk_pool_atom_t) * BIG_POOL_SIZE);
            uw_map->size[i] = 0;
            uw_map->nosize[i] = 0;
            uw_map->capacity[i] = 0;
            uw_map->maps[i] = NULL;
        }
        uw_map->id = 0;
        uw_map->index = 0;
        uw_map->fname = NULL;
        uw_map->reject_unsafe = 0;
        uw_map->reload = JK_URIMAP_DEF_RELOAD;
        uw_map->modified = 0;
        uw_map->checked = 0;

        if (init_data)
            rc = uri_worker_map_open(uw_map, init_data, l);
        if (rc == JK_TRUE)
            uw_map->id = ++map_id_counter;
        JK_TRACE_EXIT(l);
        return rc;
    }

    JK_LOG_NULL_PARAMS(l);
    JK_TRACE_EXIT(l);

    return JK_FALSE;
}

static int uri_worker_map_close(jk_uri_worker_map_t *uw_map, jk_logger_t *l)
{
    JK_TRACE_ENTER(l);

    if (uw_map) {
        JK_DELETE_CS(&uw_map->cs);
        jk_close_pool(&uw_map->p_dyn[0]);
        jk_close_pool(&uw_map->p_dyn[1]);
        jk_close_pool(&uw_map->p);
        JK_TRACE_EXIT(l);
        return JK_TRUE;
    }

    JK_LOG_NULL_PARAMS(l);
    JK_TRACE_EXIT(l);
    return JK_FALSE;
}

int uri_worker_map_free(jk_uri_worker_map_t **uw_map, jk_logger_t *l)
{
    JK_TRACE_ENTER(l);

    if (uw_map && *uw_map) {
        uri_worker_map_close(*uw_map, l);
        free(*uw_map);
        *uw_map = NULL;
        JK_TRACE_EXIT(l);
        return JK_TRUE;
    }
    else
        JK_LOG_NULL_PARAMS(l);

    JK_TRACE_EXIT(l);
    return JK_FALSE;
}

/*
 * Ensure there will be memory in context info to store Context Bases
 */

#define UW_INC_SIZE 4           /* 4 URI->WORKER STEP */

static int uri_worker_map_realloc(jk_uri_worker_map_t *uw_map)
{
    if (IND_NEXT(uw_map->size) == IND_NEXT(uw_map->capacity)) {
        uri_worker_record_t **uwr;
        int capacity = IND_NEXT(uw_map->capacity) + UW_INC_SIZE;

        uwr =
            (uri_worker_record_t **) jk_pool_alloc(&IND_NEXT(uw_map->p_dyn),
                                                   sizeof(uri_worker_record_t
                                                          *) * capacity);

        if (!uwr)
            return JK_FALSE;

        if (IND_NEXT(uw_map->capacity) && IND_NEXT(uw_map->maps))
            memcpy(uwr, IND_NEXT(uw_map->maps),
                   sizeof(uri_worker_record_t *) * IND_NEXT(uw_map->capacity));

        IND_NEXT(uw_map->maps) = uwr;
        IND_NEXT(uw_map->capacity) = capacity;
    }

    return JK_TRUE;
}


/*
 * Delete all entries of a given source type
 */

static int uri_worker_map_clear(jk_uri_worker_map_t *uw_map,
                                jk_logger_t *l)
{
    uri_worker_record_t *uwr = NULL;
    unsigned int i;
    unsigned int new_size = 0;
    unsigned int new_nosize = 0;

    JK_TRACE_ENTER(l);

    IND_NEXT(uw_map->maps) =
            (uri_worker_record_t **) jk_pool_alloc(&(IND_NEXT(uw_map->p_dyn)),
                                                   sizeof(uri_worker_record_t
                                                          *) * IND_THIS(uw_map->size));
    IND_NEXT(uw_map->capacity) = IND_THIS(uw_map->size);
    IND_NEXT(uw_map->size) = 0;
    IND_NEXT(uw_map->nosize) = 0;
    for (i = 0; i < IND_THIS(uw_map->size); i++) {
        uwr = IND_THIS(uw_map->maps)[i];
        if (uwr->source_type == SOURCE_TYPE_URIMAP) {
            if (JK_IS_DEBUG_LEVEL(l))
                jk_log(l, JK_LOG_DEBUG,
                       "deleting map rule '%s=%s' source '%s'",
                       uwr->context, uwr->worker_name, uri_worker_map_get_source(uwr, l));
        }
        else {
            IND_NEXT(uw_map->maps)[new_size] = uwr;
            new_size++;
            if (uwr->match_type & MATCH_TYPE_NO_MATCH)
                new_nosize++;
        }
    }
    IND_NEXT(uw_map->size) = new_size;
    IND_NEXT(uw_map->nosize) = new_nosize;

    JK_TRACE_EXIT(l);
    return JK_TRUE;
}

static void extract_activation(jk_pool_t *p,
                               lb_worker_t *lb,
                               int *activations,
                               char *workers,
                               int activation,
                               jk_logger_t *l)
{
    unsigned int i;
    char *worker;
#ifdef _MT_CODE_PTHREAD
    char *lasts;
#endif

    JK_TRACE_ENTER(l);

    worker = jk_pool_strdup(p, workers);

#ifdef _MT_CODE_PTHREAD
    for (worker = strtok_r(worker, ", ", &lasts);
         worker; worker = strtok_r(NULL, ", ", &lasts)) {
#else
    for (worker = strtok(worker, ", "); worker; worker = strtok(NULL, ", ")) {
#endif
        for (i = 0; i < lb->num_of_workers; i++) {
            if (!strcmp(worker, lb->lb_workers[i].name)) {
                if (activations[i] != JK_LB_ACTIVATION_UNSET)
                    jk_log(l, JK_LOG_WARNING,
                           "inconsistent activation overwrite for member %s "
                           "of load balancer %s: '%s' replaced by '%s'",
                           worker, lb->name,
                           jk_lb_get_activation_direct(activations[i], l),
                           jk_lb_get_activation_direct(activation, l));
                activations[i] = activation;
                break;
            }
        }
        if (i >= lb->num_of_workers)
            jk_log(l, JK_LOG_WARNING,
                   "could not find member %s of load balancer %s",
                   worker, lb->name);
    }

    JK_TRACE_EXIT(l);

}

static void extension_fix_fail_on_status(jk_pool_t *p,
                                         const char *name,
                                         rule_extension_t *extensions,
                                         jk_logger_t *l)
{
    unsigned int i;
    int j;
    int cnt = 1;
    size_t status_len;
    char *status;
#ifdef _MT_CODE_PTHREAD
    char *lasts;
#endif

    JK_TRACE_ENTER(l);

    status_len = strlen(extensions->fail_on_status_str);
    for (i = 0; i < status_len; i++) {
        if (extensions->fail_on_status_str[i] == ',' ||
            extensions->fail_on_status_str[i] == ' ')
            cnt++;
    }
    extensions->fail_on_status_size = cnt;

    status = jk_pool_strdup(p, extensions->fail_on_status_str);
    extensions->fail_on_status = (int *)jk_pool_alloc(p,
                                            extensions->fail_on_status_size * sizeof(int));
    if (!extensions->fail_on_status) {
        jk_log(l, JK_LOG_ERROR,
               "can't alloc extensions fail_on_status list for worker (%s)",
               name);
        JK_TRACE_EXIT(l);
        return;
    } else if (JK_IS_DEBUG_LEVEL(l))
        jk_log(l, JK_LOG_DEBUG,
               "Allocated fail_on_status array of size %d for worker (%s)",
               extensions->fail_on_status_size, name);


    for (j=0; j<extensions->fail_on_status_size; j++) {
        extensions->fail_on_status[j] = 0;
    }

    cnt = 0;
#ifdef _MT_CODE_PTHREAD
    for (status = strtok_r(status, ", ", &lasts);
         status; status = strtok_r(NULL, ", ", &lasts)) {
#else
    for (status = strtok(status, ", "); status; status = strtok(NULL, ", ")) {
#endif
        extensions->fail_on_status[cnt] = atoi(status);
        cnt++;
    }

    JK_TRACE_EXIT(l);

}

static int extension_fix_activation(jk_pool_t *p, const char *name, jk_worker_t *jw,
                                    rule_extension_t *extensions, jk_logger_t *l)
{

    JK_TRACE_ENTER(l);

    if (JK_IS_DEBUG_LEVEL(l))
        jk_log(l, JK_LOG_DEBUG,
               "Checking extension for worker %s of type %s (%d)",
               name, wc_get_name_for_type(jw->type,l), jw->type);

    if (jw->type == JK_LB_WORKER_TYPE &&
        (extensions->active || extensions->disabled || extensions->stopped)) {
        int j;
        lb_worker_t *lb = (lb_worker_t *)jw->worker_private;
        if (!extensions->activation) {
            extensions->activation_size = lb->num_of_workers;
            extensions->activation = (int *)jk_pool_alloc(p,
                                                    extensions->activation_size * sizeof(int));
            if (!extensions->activation) {
                jk_log(l, JK_LOG_ERROR,
                       "can't alloc extensions activation list");
                JK_TRACE_EXIT(l);
                return JK_FALSE;
            }
            else if (JK_IS_DEBUG_LEVEL(l))
                jk_log(l, JK_LOG_DEBUG,
                       "Allocated activations array of size %d for lb worker %s",
                       extensions->activation_size, name);
            for (j=0; j<extensions->activation_size; j++) {
                extensions->activation[j] = JK_LB_ACTIVATION_UNSET;
            }
        }
        if (extensions->active)
            extract_activation(p, lb, extensions->activation,
                               extensions->active, JK_LB_ACTIVATION_ACTIVE, l);
        if (extensions->disabled)
            extract_activation(p, lb, extensions->activation,
                               extensions->disabled, JK_LB_ACTIVATION_DISABLED, l);
        if (extensions->stopped)
            extract_activation(p, lb, extensions->activation,
                               extensions->stopped, JK_LB_ACTIVATION_STOPPED, l);
    }
    else if (extensions->active) {
        jk_log(l, JK_LOG_WARNING,
               "Worker %s is not of type lb, activation extension "
               JK_UWMAP_EXTENSION_ACTIVE " for %s ignored",
               name, extensions->active);
    }
    else if (extensions->disabled) {
        jk_log(l, JK_LOG_WARNING,
               "Worker %s is not of type lb, activation extension "
               JK_UWMAP_EXTENSION_DISABLED " for %s ignored",
               name, extensions->disabled);
    }
    else if (extensions->stopped) {
        jk_log(l, JK_LOG_WARNING,
               "Worker %s is not of type lb, activation extension "
               JK_UWMAP_EXTENSION_STOPPED " for %s ignored",
               name, extensions->stopped);
    }

    JK_TRACE_EXIT(l);
    return JK_TRUE;
}

static void extension_fix_session(jk_pool_t *p, const char *name, jk_worker_t *jw,
                                  rule_extension_t *extensions, jk_logger_t *l)
{
    if (jw->type != JK_LB_WORKER_TYPE && extensions->session_cookie) {
        jk_log(l, JK_LOG_WARNING,
                "Worker %s is not of type lb, extension "
                JK_UWMAP_EXTENSION_SESSION_COOKIE " for %s ignored",
                name, extensions->session_cookie);
    }
    if (jw->type != JK_LB_WORKER_TYPE && extensions->session_path) {
        jk_log(l, JK_LOG_WARNING,
                "Worker %s is not of type lb, extension "
                JK_UWMAP_EXTENSION_SESSION_PATH " for %s ignored",
                name, extensions->session_path);
    }
    if (jw->type != JK_LB_WORKER_TYPE && extensions->set_session_cookie) {
        jk_log(l, JK_LOG_WARNING,
                "Worker %s is not of type lb, extension "
                JK_UWMAP_EXTENSION_SET_SESSION_COOKIE " for %s ignored",
                name, extensions->set_session_cookie ? "'true'" : "'false'");
    }
    if (jw->type != JK_LB_WORKER_TYPE && extensions->session_cookie_path) {
        jk_log(l, JK_LOG_WARNING,
                "Worker %s is not of type lb, extension "
                JK_UWMAP_EXTENSION_SESSION_COOKIE_PATH " for %s ignored",
                name, extensions->session_cookie_path);
    }
}

void extension_fix(jk_pool_t *p, const char *name,
                   rule_extension_t *extensions, jk_logger_t *l)
{
    jk_worker_t *jw = wc_get_worker_for_name(name, l);
    if(!jw) {
        jk_log(l, JK_LOG_ERROR,
               "Could not find worker with name '%s' in uri map post processing.",
               name);
        return;
    }
    if (!extension_fix_activation(p, name, jw, extensions, l))
        return;
    if (extensions->fail_on_status_str) {
        extension_fix_fail_on_status(p, name, extensions, l);
    }
    extension_fix_session(p, name, jw, extensions, l);
}

void uri_worker_map_switch(jk_uri_worker_map_t *uw_map, jk_logger_t *l)
{
    int new_index;

    JK_TRACE_ENTER(l);

    if (uw_map) {
        new_index = IND_SWITCH(uw_map->index);
        if (JK_IS_DEBUG_LEVEL(l))
            jk_log(l, JK_LOG_DEBUG,
                   "Switching uri worker map from index %d to index %d",
                   uw_map->index, new_index);
        uw_map->index = new_index;
        jk_reset_pool(&(IND_NEXT(uw_map->p_dyn)));
    }

    JK_TRACE_EXIT(l);

}

void uri_worker_map_ext(jk_uri_worker_map_t *uw_map, jk_logger_t *l)
{
    unsigned int i;

    JK_TRACE_ENTER(l);

    for (i = 0; i < IND_NEXT(uw_map->size); i++) {
        uri_worker_record_t *uwr = IND_NEXT(uw_map->maps)[i];
        jk_pool_t *p;
        if(uwr->match_type & MATCH_TYPE_NO_MATCH)
            continue;
        if (uwr->source_type == SOURCE_TYPE_URIMAP)
            p = &IND_NEXT(uw_map->p_dyn);
        else
            p = &uw_map->p;
        extension_fix(p, uwr->worker_name, &uwr->extensions, l);
    }
    if (JK_IS_DEBUG_LEVEL(l))
        uri_worker_map_dump(uw_map, "after extension stripping", l);

    JK_TRACE_EXIT(l);
    return;

}

/* Parse rule extensions */
void parse_rule_extensions(char *rule, rule_extension_t *extensions,
                           jk_logger_t *l)
{
    char *param;
#ifdef _MT_CODE_PTHREAD
    char *lasts = NULL;
#endif

    extensions->reply_timeout = -1;
    extensions->sticky_ignore = JK_FALSE;
    extensions->stateless = JK_FALSE;
    extensions->active = NULL;
    extensions->disabled = NULL;
    extensions->stopped = NULL;
    extensions->activation_size = 0;
    extensions->activation = NULL;
    extensions->fail_on_status_size = 0;
    extensions->fail_on_status = NULL;
    extensions->fail_on_status_str = NULL;
    extensions->use_server_error_pages = 0;
    extensions->session_cookie = NULL;
    extensions->session_path = NULL;
    extensions->set_session_cookie = JK_FALSE;
    extensions->session_cookie_path = NULL;

#ifdef _MT_CODE_PTHREAD
    param = strtok_r(rule, ";", &lasts);
#else
    param = strtok(rule, ";");
#endif
    if (param) {
#ifdef _MT_CODE_PTHREAD
        for (param = strtok_r(NULL, ";", &lasts); param; param = strtok_r(NULL, ";", &lasts)) {
#else
        for (param = strtok(NULL, ";"); param; param = strtok(NULL, ";")) {
#endif
            if (!strncmp(param, JK_UWMAP_EXTENSION_REPLY_TIMEOUT, strlen(JK_UWMAP_EXTENSION_REPLY_TIMEOUT))) {
                extensions->reply_timeout = atoi(param + strlen(JK_UWMAP_EXTENSION_REPLY_TIMEOUT));
            }
            else if (!strncmp(param, JK_UWMAP_EXTENSION_STICKY_IGNORE, strlen(JK_UWMAP_EXTENSION_STICKY_IGNORE))) {
                int val = atoi(param + strlen(JK_UWMAP_EXTENSION_STICKY_IGNORE));
                if (val) {
                    extensions->sticky_ignore = JK_TRUE;
                }
                else {
                    extensions->sticky_ignore = JK_FALSE;
                }
            }
            else if (!strncmp(param, JK_UWMAP_EXTENSION_STATELESS, strlen(JK_UWMAP_EXTENSION_STATELESS))) {
                int val = atoi(param + strlen(JK_UWMAP_EXTENSION_STATELESS));
                if (val) {
                    extensions->stateless = JK_TRUE;
                }
                else {
                    extensions->stateless = JK_FALSE;
                }
            }
            else if (!strncmp(param, JK_UWMAP_EXTENSION_USE_SRV_ERRORS, strlen(JK_UWMAP_EXTENSION_USE_SRV_ERRORS))) {
                extensions->use_server_error_pages = atoi(param + strlen(JK_UWMAP_EXTENSION_USE_SRV_ERRORS));
            }
            else if (!strncmp(param, JK_UWMAP_EXTENSION_ACTIVE, strlen(JK_UWMAP_EXTENSION_ACTIVE))) {
                if (extensions->active)
                    jk_log(l, JK_LOG_WARNING,
                           "rule extension '" JK_UWMAP_EXTENSION_ACTIVE "' only allowed once");
                else
                    extensions->active = param + strlen(JK_UWMAP_EXTENSION_ACTIVE);
            }
            else if (!strncmp(param, JK_UWMAP_EXTENSION_DISABLED, strlen(JK_UWMAP_EXTENSION_DISABLED))) {
                if (extensions->disabled)
                    jk_log(l, JK_LOG_WARNING,
                           "rule extension '" JK_UWMAP_EXTENSION_DISABLED "' only allowed once");
                else
                    extensions->disabled = param + strlen(JK_UWMAP_EXTENSION_DISABLED);
            }
            else if (!strncmp(param, JK_UWMAP_EXTENSION_STOPPED, strlen(JK_UWMAP_EXTENSION_STOPPED))) {
                if (extensions->stopped)
                    jk_log(l, JK_LOG_WARNING,
                           "rule extension '" JK_UWMAP_EXTENSION_STOPPED "' only allowed once");
                else
                    extensions->stopped = param + strlen(JK_UWMAP_EXTENSION_STOPPED);
            }
            else if (!strncmp(param, JK_UWMAP_EXTENSION_FAIL_ON_STATUS, strlen(JK_UWMAP_EXTENSION_FAIL_ON_STATUS))) {
                if (extensions->fail_on_status_str)
                    jk_log(l, JK_LOG_WARNING,
                           "rule extension '" JK_UWMAP_EXTENSION_FAIL_ON_STATUS "' only allowed once");
                else
                    extensions->fail_on_status_str = param + strlen(JK_UWMAP_EXTENSION_FAIL_ON_STATUS);
            }
            else if (!strncmp(param, JK_UWMAP_EXTENSION_SESSION_COOKIE, strlen(JK_UWMAP_EXTENSION_SESSION_COOKIE))) {
                if (extensions->session_cookie)
                    jk_log(l, JK_LOG_WARNING,
                           "extension '" JK_UWMAP_EXTENSION_SESSION_COOKIE "' in uri worker map only allowed once");
                else
                    extensions->session_cookie = param + strlen(JK_UWMAP_EXTENSION_SESSION_COOKIE);
            }
            else if (!strncmp(param, JK_UWMAP_EXTENSION_SESSION_PATH, strlen(JK_UWMAP_EXTENSION_SESSION_PATH))) {
                if (extensions->session_path)
                    jk_log(l, JK_LOG_WARNING,
                           "extension '" JK_UWMAP_EXTENSION_SESSION_PATH "' in uri worker map only allowed once");
                else {
                    // Check if the session identifier starts with semicolon.
                    if (!strcmp(param, JK_UWMAP_EXTENSION_SESSION_PATH)) {
#ifdef _MT_CODE_PTHREAD
                        param = strtok_r(NULL, ";", &lasts);
#else
                        param = strtok(NULL, ";");
#endif
                        extensions->session_path = param;
                    } else
                        extensions->session_path = param + strlen(JK_UWMAP_EXTENSION_SESSION_PATH);
                }
            }
            else if (!strncmp(param, JK_UWMAP_EXTENSION_SET_SESSION_COOKIE, strlen(JK_UWMAP_EXTENSION_SET_SESSION_COOKIE))) {
                if (extensions->set_session_cookie)
                    jk_log(l, JK_LOG_WARNING,
                           "extension '" JK_UWMAP_EXTENSION_SET_SESSION_COOKIE "' in uri worker map only allowed once");
                else {
                    int val = atoi(param + strlen(JK_UWMAP_EXTENSION_SET_SESSION_COOKIE));
                    if (val) {
                        extensions->set_session_cookie = JK_TRUE;
                    }
                    else {
                        extensions->set_session_cookie = JK_FALSE;
                    }
                }
            }
            else if (!strncmp(param, JK_UWMAP_EXTENSION_SESSION_COOKIE_PATH, strlen(JK_UWMAP_EXTENSION_SESSION_COOKIE_PATH))) {
                if (extensions->session_cookie_path)
                    jk_log(l, JK_LOG_WARNING,
                           "extension '" JK_UWMAP_EXTENSION_SESSION_COOKIE_PATH "' in uri worker map only allowed once");
                else
                    extensions->session_cookie_path = param + strlen(JK_UWMAP_EXTENSION_SESSION_COOKIE_PATH);
            }
            else {
                jk_log(l, JK_LOG_WARNING,
                       "unknown rule extension '%s'",
                       param);
            }
        }
    }
}


/* Add new entry to NEXT generation */
int uri_worker_map_add(jk_uri_worker_map_t *uw_map,
                       const char *puri, const char *worker,
                       unsigned int source_type, jk_logger_t *l)
{
    uri_worker_record_t *uwr = NULL;
    char *uri;
    jk_pool_t *p;
    unsigned int match_type = 0;

    JK_TRACE_ENTER(l);

    if (*puri == '-') {
        /* Disable urimap.
         * This way you can disable already mounted
         * context.
         */
        match_type = MATCH_TYPE_DISABLED;
        puri++;
    }
    if (*puri == '!') {
        match_type |= MATCH_TYPE_NO_MATCH;
        puri++;
    }

    if (uri_worker_map_realloc(uw_map) == JK_FALSE) {
        JK_TRACE_EXIT(l);
        return JK_FALSE;
    }
    if (source_type == SOURCE_TYPE_URIMAP)
        p = &IND_NEXT(uw_map->p_dyn);
    else
        p = &uw_map->p;

    uwr = (uri_worker_record_t *)jk_pool_alloc(p, sizeof(uri_worker_record_t));
    if (!uwr) {
        jk_log(l, JK_LOG_ERROR,
               "can't alloc map entry");
        JK_TRACE_EXIT(l);
        return JK_FALSE;
    }

    uri = jk_pool_strdup(p, puri);
    if (!uri || !worker) {
        jk_log(l, JK_LOG_ERROR,
               "can't alloc uri/worker strings");
        JK_TRACE_EXIT(l);
        return JK_FALSE;
    }

    if (*uri == '/') {
        char *w = jk_pool_strdup(p, worker);
        parse_rule_extensions(w, &uwr->extensions, l);
        uwr->source_type = source_type;
        uwr->worker_name = w;
        uwr->uri = uri;
        uwr->context = uri;
        uwr->context_len = strlen(uwr->context);
        if (strchr(uri, '*') ||
            strchr(uri, '?')) {
            /* Something like
             * /context/ * /user/ *
             * /context/ *.suffix
             */
            match_type |= MATCH_TYPE_WILDCHAR_PATH;
            if (JK_IS_DEBUG_LEVEL(l))
                jk_log(l, JK_LOG_DEBUG,
                       "wildchar rule '%s=%s' source '%s' was added",
                       uwr->context, uwr->worker_name, uri_worker_map_get_source(uwr, l));

        }
        else {
            /* Something like:  JkMount /login/j_security_check ajp13 */
            match_type |= MATCH_TYPE_EXACT;
            if (JK_IS_DEBUG_LEVEL(l))
                jk_log(l, JK_LOG_DEBUG,
                       "exact rule '%s=%s' source '%s' was added",
                       uwr->context, uwr->worker_name, uri_worker_map_get_source(uwr, l));
        }
    }
    else {
        /*
         * JFC: please check...
         * Not sure what to do, but I try to prevent problems.
         * I have fixed jk_mount_context() in apaches/mod_jk.c so we should
         * not arrive here when using Apache.
         */
        jk_log(l, JK_LOG_ERROR,
               "invalid context '%s': does not begin with '/'",
               uri);
        JK_TRACE_EXIT(l);
        return JK_FALSE;
    }
    uwr->match_type = match_type;
    IND_NEXT(uw_map->maps)[IND_NEXT(uw_map->size)] = uwr;
    IND_NEXT(uw_map->size)++;
    if (match_type & MATCH_TYPE_NO_MATCH) {
        /* If we split the mappings this one will be calculated */
        IND_NEXT(uw_map->nosize)++;
    }
    worker_qsort(uw_map);
    JK_TRACE_EXIT(l);
    return JK_TRUE;
}

int uri_worker_map_open(jk_uri_worker_map_t *uw_map,
                        jk_map_t *init_data, jk_logger_t *l)
{
    int rc = JK_TRUE;

    JK_TRACE_ENTER(l);

    if (uw_map) {
        int sz = jk_map_size(init_data);

        if (JK_IS_DEBUG_LEVEL(l))
            jk_log(l, JK_LOG_DEBUG,
                   "rule map size is %d",
                   sz);

        if (sz > 0) {
            int i;
            for (i = 0; i < sz; i++) {
                const char *u = jk_map_name_at(init_data, i);
                const char *w = jk_map_value_at(init_data, i);
                /* Multiple mappings like :
                 * /servlets-examples|/ *
                 * will create two mappings:
                 * /servlets-examples
                 * and:
                 * /servlets-examples/ *
                 */
                if (strchr(u, '|')) {
                    char *s, *r = strdup(u);
                    s = strchr(r, '|');
                    *(s++) = '\0';
                    /* Add first mapping */
                    if (!uri_worker_map_add(uw_map, r, w, SOURCE_TYPE_JKMOUNT, l)) {
                        jk_log(l, JK_LOG_ERROR,
                        "invalid mapping rule %s->%s", r, w);
                        rc = JK_FALSE;
                    }
                    for (; *s; s++)
                        *(s - 1) = *s;
                    *(s - 1) = '\0';
                    /* add second mapping */
                    if (!uri_worker_map_add(uw_map, r, w, SOURCE_TYPE_JKMOUNT, l)) {
                        jk_log(l, JK_LOG_ERROR,
                               "invalid mapping rule %s->%s", r, w);
                        rc = JK_FALSE;
                    }
                    free(r);
                }
                else if (!uri_worker_map_add(uw_map, u, w, SOURCE_TYPE_JKMOUNT, l)) {
                    jk_log(l, JK_LOG_ERROR,
                           "invalid mapping rule %s->%s",
                           u, w);
                    rc = JK_FALSE;
                    break;
                }
                if (rc == JK_FALSE)
                    break;
            }
        }

        if (rc == JK_FALSE) {
            jk_log(l, JK_LOG_ERROR,
                   "there was an error, freeing buf");
            jk_close_pool(&uw_map->p_dyn[0]);
            jk_close_pool(&uw_map->p_dyn[1]);
            jk_close_pool(&uw_map->p);
        }
        else if (JK_IS_DEBUG_LEVEL(l))
            uri_worker_map_dump(uw_map, "after map open", l);
    }

    JK_TRACE_EXIT(l);
    return rc;
}

static int find_match(jk_uri_worker_map_t *uw_map,
                      const char *url, jk_logger_t *l)
{
    unsigned int i;

    JK_TRACE_ENTER(l);

    for (i = 0; i < IND_THIS(uw_map->size); i++) {
        uri_worker_record_t *uwr = IND_THIS(uw_map->maps)[i];

        /* Check for match types */
        if ((uwr->match_type & MATCH_TYPE_DISABLED) ||
            (uwr->match_type & MATCH_TYPE_NO_MATCH))
            continue;

        if (JK_IS_DEBUG_LEVEL(l))
            jk_log(l, JK_LOG_DEBUG, "Attempting to map context URI '%s=%s' source '%s'",
                   uwr->context, uwr->worker_name, uri_worker_map_get_source(uwr, l));

        if (uwr->match_type & MATCH_TYPE_WILDCHAR_PATH) {
            /* Map is already sorted by context_len */
            if (jk_wildchar_match(url, uwr->context,
#ifdef WIN32
                               0
#else
                               0
#endif
                               ) == 0) {
                    if (JK_IS_DEBUG_LEVEL(l))
                        jk_log(l, JK_LOG_DEBUG,
                               "Found a wildchar match '%s=%s'",
                               uwr->context, uwr->worker_name);
                    JK_TRACE_EXIT(l);
                    return i;
             }
        }
        else if (JK_STRNCMP(uwr->context, url, uwr->context_len) == 0) {
            if (strlen(url) == uwr->context_len) {
                if (JK_IS_DEBUG_LEVEL(l))
                    jk_log(l, JK_LOG_DEBUG,
                           "Found an exact match '%s=%s'",
                           uwr->context, uwr->worker_name);
                JK_TRACE_EXIT(l);
                return i;
            }
        }
    }

    JK_TRACE_EXIT(l);
    return -1;
}

static int is_nomatch(jk_uri_worker_map_t *uw_map,
                      const char *uri, int match,
                      jk_logger_t *l)
{
    unsigned int i;
    const char *worker = IND_THIS(uw_map->maps)[match]->worker_name;

    JK_TRACE_ENTER(l);

    for (i = 0; i < IND_THIS(uw_map->size); i++) {
        uri_worker_record_t *uwr = IND_THIS(uw_map->maps)[i];

        /* Check only nomatch mappings */
        if (!(uwr->match_type & MATCH_TYPE_NO_MATCH) ||
            (uwr->match_type & MATCH_TYPE_DISABLED))
            continue;
        /* Check only matching workers */
        if (strcmp(uwr->worker_name, worker) &&
            strcmp(uwr->worker_name, "*"))
            continue;
        if (uwr->match_type & MATCH_TYPE_WILDCHAR_PATH) {
            /* Map is already sorted by context_len */
            if (jk_wildchar_match(uri, uwr->context,
#ifdef WIN32
                               0
#else
                               0
#endif
                               ) == 0) {
                    if (JK_IS_DEBUG_LEVEL(l))
                        jk_log(l, JK_LOG_DEBUG,
                               "Found a wildchar no match '%s=%s' source '%s'",
                               uwr->context, uwr->worker_name, uri_worker_map_get_source(uwr, l));
                    JK_TRACE_EXIT(l);
                    return JK_TRUE;
             }
        }
        else if (JK_STRNCMP(uwr->context, uri, uwr->context_len) == 0) {
            if (strlen(uri) == uwr->context_len) {
                if (JK_IS_DEBUG_LEVEL(l))
                    jk_log(l, JK_LOG_DEBUG,
                           "Found an exact no match '%s=%s' source '%s'",
                           uwr->context, uwr->worker_name, uri_worker_map_get_source(uwr, l));
                JK_TRACE_EXIT(l);
                return JK_TRUE;
            }
        }
    }

    JK_TRACE_EXIT(l);
    return JK_FALSE;
}

const char *map_uri_to_worker_ext(jk_uri_worker_map_t *uw_map,
                                  const char *uri, const char *vhost,
                                  rule_extension_t **extensions,
                                  int *index, jk_logger_t *l)
{
    unsigned int i;
    unsigned int vhost_len;
    int reject_unsafe;
    size_t uri_len;
    size_t remain;
    int rv = -1;
    char  url[JK_MAX_URI_LEN+1];

    JK_TRACE_ENTER(l);

    if (!uw_map || !uri || !extensions) {
        JK_LOG_NULL_PARAMS(l);
        JK_TRACE_EXIT(l);
        return NULL;
    }
    *extensions = NULL;
    if (index)
        *index = -1;
    if (*uri != '/') {
        if (*uri == '*' && *(uri+1) == '\0') {
            /* Most likely an "OPTIONS *" request */
            if (JK_IS_DEBUG_LEVEL(l)) {
                jk_log(l, JK_LOG_DEBUG,
                       "Uri %s can't be mapped.", uri);
            }
        } else {
            jk_log(l, JK_LOG_WARNING,
                   "Uri %s is invalid. Uri must start with /", uri);
        }
        JK_TRACE_EXIT(l);
        return NULL;
    }
    if (uw_map->fname) {
        uri_worker_map_update(uw_map, 0, l);
        if (!IND_THIS(uw_map->size)) {
            jk_log(l, JK_LOG_INFO,
                   "No worker maps defined for %s.",
                   uw_map->fname);
            JK_TRACE_EXIT(l);
            return NULL;
        }
    }
    reject_unsafe = uw_map->reject_unsafe;
    vhost_len = 0;
    /*
     * In case we got a vhost, we prepend a slash
     * and the vhost to the url in order to enable
     * vhost mapping rules especially for IIS.
     */
    if (vhost) {
        int off = 0;
        /* Add leading '/' if necessary */
        if (vhost[0] != '/') {
            url[0] = '/';
            off = 1;
        }
        /* Size including leading slash. */
        vhost_len = (unsigned int)strlen(vhost);
        if (vhost_len + off >= JK_MAX_URI_LEN) {
            vhost_len = 0;
            jk_log(l, JK_LOG_WARNING,
                   "Host prefix %s for URI %s is invalid and will be ignored."
                   " It must be smaller than %d chars",
                   vhost, JK_MAX_URI_LEN - off);
        }
        else {
            strncpy(&url[off], vhost, vhost_len + 1);
            if (JK_IS_DEBUG_LEVEL(l)) {
                jk_log(l, JK_LOG_DEBUG, "Prefixing mapping uri with vhost '%s'", vhost);
            }
        }
        vhost_len += off;
    }
    /* Make the copy of the provided uri, check length
     * and look for potentially unsafe constructs
     */
    uri_len = strlen(uri);
    remain = JK_MAX_URI_LEN - vhost_len;
    for (i = 0; i < uri_len; i++) {
        if (i == remain) {
            jk_log(l, JK_LOG_WARNING,
                   "URI %s is invalid. URI must be smaller than %d chars",
                   uri, remain);
            JK_TRACE_EXIT(l);
            return NULL;
        }
        url[i + vhost_len] = uri[i];
        if (reject_unsafe && (uri[i] == '%' || uri[i] == '\\')) {
            jk_log(l, JK_LOG_INFO, "Potentially unsafe request url '%s' rejected", uri);
            JK_TRACE_EXIT(l);
            return NULL;
        }
    }
    url[i + vhost_len] = '\0';

    if (JK_IS_DEBUG_LEVEL(l))
        jk_log(l, JK_LOG_DEBUG, "Attempting to map URI '%s' from %d maps",
               url, IND_THIS(uw_map->size));
    rv = find_match(uw_map, url, l);
    /* If this doesn't find a match, try without the vhost. */
    if (rv < 0 && vhost_len) {
        rv = find_match(uw_map, &url[vhost_len], l);
    }

    /* In case we found a match, check for the unmounts. */
    if (rv >= 0 && IND_THIS(uw_map->nosize)) {
        int rc;
        /* Again first including vhost. */
        rc = is_nomatch(uw_map, url, rv, l);
        /* If no unmount was found, try without vhost. */
        if (!rc && vhost_len)
            rc = is_nomatch(uw_map, &url[vhost_len], rv, l);
        if (rc) {
            if (JK_IS_DEBUG_LEVEL(l)) {
                jk_log(l, JK_LOG_DEBUG,
                       "Denying match for worker %s by nomatch rule",
                       IND_THIS(uw_map->maps)[rv]->worker_name);
            }
            rv = -1;
        }
    }

    if (rv >= 0) {
        *extensions = &(IND_THIS(uw_map->maps)[rv]->extensions);
        if (index)
            *index = rv;
        JK_TRACE_EXIT(l);
        return IND_THIS(uw_map->maps)[rv]->worker_name;
    }
    JK_TRACE_EXIT(l);
    return NULL;
}

rule_extension_t *get_uri_to_worker_ext(jk_uri_worker_map_t *uw_map,
                                        int index)
{
    if (index >= 0) {
        return &(IND_THIS(uw_map->maps)[index]->extensions);
    }
    else {
        return NULL;
    }
}

int uri_worker_map_load(jk_uri_worker_map_t *uw_map,
                        jk_logger_t *l)
{
    int rc = JK_FALSE;
    jk_map_t *map;

    jk_map_alloc(&map);
    if (jk_map_read_properties(map, NULL, uw_map->fname, &uw_map->modified,
                               JK_MAP_HANDLE_NORMAL, l)) {
        int i;
        if (JK_IS_DEBUG_LEVEL(l))
            jk_log(l, JK_LOG_DEBUG,
                   "Loading urimaps from %s with reload check interval %d seconds",
                   uw_map->fname, uw_map->reload);
        uri_worker_map_clear(uw_map, l);
        for (i = 0; i < jk_map_size(map); i++) {
            const char *u = jk_map_name_at(map, i);
            const char *w = jk_map_value_at(map, i);
            /* Multiple mappings like :
             * /servlets-examples|/ *
             * will create two mappings:
             * /servlets-examples
             * and:
             * /servlets-examples/ *
             */
            if (strchr(u, '|')) {
                char *s, *r = strdup(u);
                s = strchr(r, '|');
                *(s++) = '\0';
                /* Add first mapping */
                if (!uri_worker_map_add(uw_map, r, w, SOURCE_TYPE_URIMAP, l)) {
                    jk_log(l, JK_LOG_ERROR,
                    "invalid mapping rule %s->%s", r, w);
                }
                for (; *s; s++)
                   *(s - 1) = *s;
                *(s - 1) = '\0';
                /* add second mapping */
                if (!uri_worker_map_add(uw_map, r, w, SOURCE_TYPE_URIMAP, l)) {
                    jk_log(l, JK_LOG_ERROR,
                    "invalid mapping rule %s->%s", r, w);
                }
                free(r);
            }
            else if (!uri_worker_map_add(uw_map, u, w, SOURCE_TYPE_URIMAP, l)) {
                jk_log(l, JK_LOG_ERROR,
                       "invalid mapping rule %s->%s",
                       u, w);
            }
        }
        uw_map->checked = time(NULL);
        if (JK_IS_DEBUG_LEVEL(l))
            uri_worker_map_dump(uw_map, "after file load", l);
        rc = JK_TRUE;
    } else {
        jk_log(l, JK_LOG_ERROR, "Failed to load uri_worker_map file %s (errno=%d, err=%s).", uw_map->fname, errno, strerror(errno));
    }
    jk_map_free(&map);
    return rc;
}

int uri_worker_map_update(jk_uri_worker_map_t *uw_map,
                          int force, jk_logger_t *l)
{
    int rc = JK_TRUE;
    time_t now = time(NULL);

    if (force || (uw_map->reload > 0 && difftime(now, uw_map->checked) >
                  uw_map->reload)) {
        struct stat statbuf;
        uw_map->checked = now;
        if ((rc = jk_stat(uw_map->fname, &statbuf)) == -1) {
            jk_log(l, JK_LOG_ERROR,
                   "Unable to stat the %s (errno=%d)",
                   uw_map->fname, errno);
            return JK_FALSE;
        }
        if (statbuf.st_mtime == uw_map->modified) {
            if (JK_IS_DEBUG_LEVEL(l))
                jk_log(l, JK_LOG_DEBUG,
                       "File %s is not modified",
                       uw_map->fname);
            return JK_TRUE;
        }
        JK_ENTER_CS(&uw_map->cs);
        /* Check if some other thread updated status */
        if (statbuf.st_mtime == uw_map->modified) {
            JK_LEAVE_CS(&uw_map->cs);
            if (JK_IS_DEBUG_LEVEL(l))
                jk_log(l, JK_LOG_DEBUG,
                       "File %s  is not modified",
                       uw_map->fname);
            return JK_TRUE;
        }
        rc = uri_worker_map_load(uw_map, l);
        uri_worker_map_ext(uw_map, l);
        uri_worker_map_switch(uw_map, l);
        JK_LEAVE_CS(&uw_map->cs);
        jk_log(l, JK_LOG_INFO,
               "Reloaded urimaps from %s", uw_map->fname);
    }
    return JK_TRUE;
}
