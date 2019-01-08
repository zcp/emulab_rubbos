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
 * Description: Multi thread portability code for JK                       *
 * Author:      Gal Shachor <shachor@il.ibm.com>                           *
 * Version:     $Revision: 1837376 $                                           *
 ***************************************************************************/

#ifndef _JK_MT_H
#define _JK_MT_H

#include "jk_global.h"


#if defined(WIN32)
#define jk_gettid()    ((jk_uint32_t)GetCurrentThreadId())
#endif

#ifdef JK_PREFORK 
#define _MT_CODE 0
#else
#define _MT_CODE 1
#endif

/*
 * Marks execution under MT compilation
 */
#if _MT_CODE
#ifdef WIN32
#include <windows.h>

typedef CRITICAL_SECTION JK_CRIT_SEC;
#define JK_INIT_CS(x, rc)   InitializeCriticalSection(x); rc = JK_TRUE
#define JK_DELETE_CS(x)     DeleteCriticalSection(x)
#define JK_ENTER_CS(x)      EnterCriticalSection(x)
#define JK_LEAVE_CS(x)      LeaveCriticalSection(x)

#else /* !WIN32 */
#define _MT_CODE_PTHREAD
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>

typedef pthread_mutex_t JK_CRIT_SEC;
#define JK_INIT_CS(x, rc)       \
            if (pthread_mutex_init(x, NULL)) rc = JK_FALSE; else rc = JK_TRUE

#define JK_DELETE_CS(x) pthread_mutex_destroy(x)
#define JK_ENTER_CS(x)  pthread_mutex_lock(x)
#define JK_LEAVE_CS(x)  pthread_mutex_unlock(x)

#if defined(AS400)
#define jk_pthread_t   jk_uint32_t
#endif /* AS400 */
jk_pthread_t jk_gettid(void);
#endif /* WIN32 */

#else /* !_MT_CODE */

typedef void *JK_CRIT_SEC;
#define JK_INIT_CS(x, rc)   rc = JK_TRUE
#define JK_DELETE_CS(x)     (void)0
#define JK_ENTER_CS(x)      (void)0
#define JK_LEAVE_CS(x)      (void)0
#define jk_gettid()         0
#endif /* MT_CODE */

#if !defined(WIN32)
#include <unistd.h>
#include <fcntl.h>


#if HAVE_FLOCK
#ifdef JK_USE_FLOCK
#define USE_FLOCK_LK 1
#endif
#endif
#ifndef USE_FLOCK_LK
#define USE_FLOCK_LK 0
#endif

#if USE_FLOCK_LK
#include <sys/file.h>

#define JK_ENTER_LOCK(x, rc)        \
    do {                            \
      while ((rc = flock((x), LOCK_EX) < 0) && (errno == EINTR)); \
      rc = rc == 0 ? JK_TRUE : JK_FALSE; \
    } while (0)

#define JK_LEAVE_LOCK(x, rc)        \
    do {                            \
      while ((rc = flock((x), LOCK_UN) < 0) && (errno == EINTR)); \
      rc = rc == 0 ? JK_TRUE : JK_FALSE; \
    } while (0)

#else /* !USE_FLOCK_LK */

#define JK_ENTER_LOCK(x, rc)        \
    do {                            \
      struct flock _fl;             \
      _fl.l_type   = F_WRLCK;       \
      _fl.l_whence = SEEK_SET;      \
      _fl.l_start  = 0;             \
      _fl.l_len    = 1L;            \
      _fl.l_pid    = 0;             \
      while ((rc = fcntl((x), F_SETLKW, &_fl) < 0) && (errno == EINTR)); \
      rc = rc == 0 ? JK_TRUE : JK_FALSE; \
    } while (0)

#define JK_LEAVE_LOCK(x, rc)        \
    do {                            \
      struct flock _fl;             \
      _fl.l_type   = F_UNLCK;       \
      _fl.l_whence = SEEK_SET;      \
      _fl.l_start  = 0;             \
      _fl.l_len    = 1L;            \
      _fl.l_pid    = 0;             \
      while ((rc = fcntl((x), F_SETLKW, &_fl) < 0) && (errno == EINTR)); \
      rc = rc == 0 ? JK_TRUE : JK_FALSE; \
    } while (0)

#endif /* USE_FLOCK_LK */

#else  /* WIN32 */
#define JK_ENTER_LOCK(x, rc) rc = JK_TRUE
#define JK_LEAVE_LOCK(x, rc) rc = JK_TRUE
#endif

#endif /* _JK_MT_H */
