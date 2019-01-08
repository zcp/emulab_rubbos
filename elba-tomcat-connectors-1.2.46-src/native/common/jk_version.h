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
 * Description: JK version header file                                     *
 * Version:     $Revision: 1842049 $                                           *
 ***************************************************************************/

#ifndef __JK_VERSION_H
#define __JK_VERSION_H

/************** START OF AREA TO MODIFY BEFORE RELEASING *************/
#define JK_VERMAJOR     1
#define JK_VERMINOR     2
#define JK_VERFIX       46

/* set JK_VERISRELEASE to 1 when release (do not forget to commit!) */
#define JK_VERISRELEASE 1
/* Beta number */
#define JK_VERBETA      0
#define JK_BETASTRING   "0"
/* Release candidate */
#define JK_VERRC        0
#define JK_RCSTRING     "0"

/************** END OF AREA TO MODIFY BEFORE RELEASING *************/

#if !defined(PACKAGE)
#if defined(JK_ISAPI)
#define PACKAGE "isapi_redirector"
#define JK_DLL_SUFFIX "dll"
#elif defined(JK_NSAPI)
#define PACKAGE "nsapi_redirector"
#define JK_DLL_SUFFIX "dll"
#else
#define PACKAGE "mod_jk"
#define JK_DLL_SUFFIX "so"
#endif
#endif

/* Build JK_EXPOSED_VERSION and JK_VERSION */
#define JK_EXPOSED_VERSION_INT PACKAGE "/" JK_VERSTRING

#if (JK_VERBETA != 0)
#define JK_EXPOSED_VERSION JK_EXPOSED_VERSION_INT "-beta-" JK_BETASTRING
#else
#undef JK_VERBETA
#define JK_VERBETA 255
#if (JK_VERRC != 0)
#define JK_EXPOSED_VERSION JK_EXPOSED_VERSION_INT "-rc-" JK_RCSTRING
#elif (JK_VERISRELEASE == 1)
#define JK_EXPOSED_VERSION JK_EXPOSED_VERSION_INT
#else
#define JK_EXPOSED_VERSION JK_EXPOSED_VERSION_INT "-dev"
#endif
#endif
#define JK_FULL_EXPOSED_VERSION JK_EXPOSED_VERSION

#define JK_MAKEVERSION(major, minor, fix, beta) \
            (((major) << 24) + ((minor) << 16) + ((fix) << 8) + (beta))

#define JK_VERSION JK_MAKEVERSION(JK_VERMAJOR, JK_VERMINOR, JK_VERFIX, JK_VERBETA)

/** Properly quote a value as a string in the C preprocessor */
#define JK_STRINGIFY(n) JK_STRINGIFY_HELPER(n)
/** Helper macro for JK_STRINGIFY */
#define JK_STRINGIFY_HELPER(n) #n
#define JK_VERSTRING \
            JK_STRINGIFY(JK_VERMAJOR) "." \
            JK_STRINGIFY(JK_VERMINOR) "." \
            JK_STRINGIFY(JK_VERFIX)

/* macro for Win32 .rc files using numeric csv representation */
#define JK_VERSIONCSV JK_VERMAJOR ##, \
                    ##JK_VERMINOR ##, \
                    ##JK_VERFIX


#endif /* __JK_VERSION_H */

