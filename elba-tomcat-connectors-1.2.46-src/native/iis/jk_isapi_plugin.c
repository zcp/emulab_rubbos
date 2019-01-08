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
 * Description: ISAPI plugin for IIS/PWS                                   *
 * Author:      Gal Shachor <shachor@il.ibm.com>                           *
 * Author:      Larry Isaacs <larryi@apache.org>                           *
 * Author:      Ignacio J. Ortega <nacho@apache.org>                       *
 * Author:      Mladen Turk <mturk@apache.org>                             *
 * Version:     $Revision: 1840629 $                                       *
 ***************************************************************************/

/*
 * Define WIN32 API we are going to use.
 */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0502
#endif
#include <winsock2.h>
#include <httpext.h>
#include <httpfilt.h>
#include <wininet.h>

#include "jk_global.h"
#include "jk_url.h"
#include "jk_util.h"
#include "jk_map.h"
#include "jk_pool.h"
#include "jk_service.h"
#include "jk_worker.h"
#include "jk_uri_worker_map.h"
#include "jk_shm.h"
#include "jk_ajp13.h"
#include "pcre.h"

#ifndef POSIX_MALLOC_THRESHOLD
#define POSIX_MALLOC_THRESHOLD 10
#endif

#ifndef HSE_REQ_SET_FLUSH_FLAG
#define HSE_REQ_SET_FLUSH_FLAG (HSE_REQ_END_RESERVED + 43)
#endif

#include <strsafe.h>

#define VERSION_STRING "Tomcat/ISAPI/" JK_EXPOSED_VERSION
#define FULL_VERSION_STRING "Tomcat/ISAPI/" JK_FULL_EXPOSED_VERSION
#define SHM_DEF_PREFIX      "JK_"
#define DEFAULT_WORKER_NAME "ajp13"

/*
 * This is default value found inside httpd.conf
 * for MaxClients
 */
#define DEFAULT_WORKER_THREADS  250
#define RES_BUFFER_SIZE         64
#define HDR_BUFFER_SIZE         1024

/*
 * We use special headers to pass values from the filter to the
 * extension. These values are:
 *
 * 1. The real URI before redirection took place
 * 2. The name of the worker to be used.
 * 3. The contents of the Translate header, if any
 *
 */
#define URI_HEADER_NAME_BASE              "TOMCATURI"
#define QUERY_HEADER_NAME_BASE            "TOMCATQUERY"
#define WORKER_HEADER_NAME_BASE           "TOMCATWORKER"
#define WORKER_HEADER_INDEX_BASE          "TOMCATWORKERIDX"
#define TOMCAT_TRANSLATE_HEADER_NAME_BASE "TOMCATTRANSLATE"
#ifndef USE_CGI_HEADERS
#define CONTENT_LENGTH                    "CONTENT-LENGTH:"
#define ALL_HEADERS                       "ALL_RAW"
#else
#define ALL_HEADERS                       "ALL_HTTP"
#endif

/* The HTTP_ form of the header for use in ExtensionProc */
#define HTTP_HEADER_PREFIX       "HTTP_"
#ifdef USE_CGI_HEADERS
#define HTTP_HEADER_PREFIX_LEN   5
#endif

/* The template used to construct our unique headers
 * from the base name and module instance
 */
#define HEADER_TEMPLATE      "%s%p:"
#define HTTP_HEADER_TEMPLATE HTTP_HEADER_PREFIX "%s%p"

static char URI_HEADER_NAME[RES_BUFFER_SIZE];
static char QUERY_HEADER_NAME[RES_BUFFER_SIZE];
static char WORKER_HEADER_NAME[RES_BUFFER_SIZE];
static char TOMCAT_TRANSLATE_HEADER_NAME[RES_BUFFER_SIZE];
static char WORKER_HEADER_INDEX[RES_BUFFER_SIZE];

/* The variants of the special headers after IIS adds
 * "HTTP_" to the front of them
 */
static char HTTP_URI_HEADER_NAME[RES_BUFFER_SIZE];
static char HTTP_QUERY_HEADER_NAME[RES_BUFFER_SIZE];
static char HTTP_WORKER_HEADER_NAME[RES_BUFFER_SIZE];
static char HTTP_WORKER_HEADER_INDEX[RES_BUFFER_SIZE];

#define REGISTRY_LOCATION       "Software\\Apache Software Foundation\\Jakarta Isapi Redirector\\1.0"
#define W3SVC_REGISTRY_KEY      "SYSTEM\\CurrentControlSet\\Services\\W3SVC\\Parameters"
#define EXTENSION_URI_TAG       "extension_uri"

#define URI_SELECT_TAG                "uri_select"
#define URI_SELECT_PARSED_VERB        "parsed"
#define URI_SELECT_UNPARSED_VERB      "unparsed"
#define URI_SELECT_ESCAPED_VERB       "escaped"
#define URI_SELECT_PROXY_VERB         "proxy"
#define URI_REWRITE_TAG               "rewrite_rule_file"
#define SHM_SIZE_TAG                  "shm_size"
#define WORKER_MOUNT_RELOAD_TAG       "worker_mount_reload"
#define STRIP_SESSION_TAG             "strip_session"
#define AUTH_COMPLETE_TAG             "auth_complete"
#define REJECT_UNSAFE_TAG             "reject_unsafe"
#define COLLAPSE_SLASHES_TAG          "collapse_slashes"
#define WATCHDOG_INTERVAL_TAG         "watchdog_interval"
#define ENABLE_CHUNKED_ENCODING_TAG   "enable_chunked_encoding"
#define ERROR_PAGE_TAG                "error_page"
#define FLUSH_PACKETS_TAG             "flush_packets"

#define LOG_ROTATION_TIME_TAG         "log_rotationtime"
#define LOG_FILESIZE_TAG              "log_filesize"

/* HTTP standard headers */
#define TRANSFER_ENCODING_CHUNKED_HEADER_COMPLETE     "Transfer-Encoding: chunked"
#define TRANSFER_ENCODING_CHUNKED_HEADER_COMPLETE_LEN 26
#define TRANSFER_ENCODING_HEADER_NAME                 "Transfer-Encoding"
#define TRANSFER_ENCODING_HEADER_NAME_LEN             17
#define TRANSFER_ENCODING_IDENTITY_VALUE              "identity"
#define TRANSFER_ENCODING_CHUNKED_VALUE               "chunked"
#define TRANSFER_ENCODING_CHUNKED_VALUE_LEN           7

#define CONTENT_LENGTH_HEADER_NAME                    "Content-Length"
#define CONTENT_LENGTH_HEADER_NAME_LEN                14

#define CONNECTION_HEADER_NAME      "Connection"
#define CONNECTION_CLOSE_VALUE      "Close"

#define TRANSLATE_HEADER            "Translate:"
#define TRANSLATE_HEADER_NAME       "Translate"
#define TRANSLATE_HEADER_NAME_LC    "translate"

/* HTTP protocol CRLF */
#define CRLF                        "\r\n"
#define CRLF_LEN                    2
#define NIL                         ""

/* Transfer-Encoding: chunked content trailer */
#define CHUNKED_ENCODING_TRAILER     "0\r\n\r\n"
#define CHUNKED_ENCODING_TRAILER_LEN 5

#define BAD_REQUEST         -1
#define BAD_PATH            -2
#define MAX_SERVERNAME      1024
#define MAX_INSTANCEID      32

char HTML_ERROR_HEAD[] =        "<!--\n"
                                "  Licensed to the Apache Software Foundation (ASF) under one or more\n"
                                "  contributor license agreements.  See the NOTICE file distributed with\n"
                                "  this work for additional information regarding copyright ownership.\n"
                                "  The ASF licenses this file to You under the Apache License, Version 2.0\n"
                                "  (the \"License\"); you may not use this file except in compliance with\n"
                                "  the License.  You may obtain a copy of the License at\n\n"
                                "      http://www.apache.org/licenses/LICENSE-2.0\n\n"
                                "  Unless required by applicable law or agreed to in writing, software\n"
                                "  distributed under the License is distributed on an \"AS IS\" BASIS,\n"
                                "  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.\n"
                                "  See the License for the specific language governing permissions and\n"
                                "  limitations under the License.\n"
                                "  -->\n"
                                "<!DOCTYPE HTML PUBLIC \"-//W3C//DTD HTML 4.0 Transitional//EN\"\n"
                                "\"http://www.w3c.org/TR/REC-html40/loose.dtd\">\n"
                                "<HTML>\n<HEAD>\n"
                                "<META http-equiv=\"Content-Type\" content=\"text/html; charset=ISO-8859-1\">\n"
                                "<STYLE TYPE=\"text/css\">\n"
                                "    body {\n"
                                "       color: #000000;\n"
                                "       background-color: #FFFFFF;\n"
                                "       font-family: Verdana, Tahoma, Arial, Helvetica, sans-serif;\n"
                                "       font-size: 9pt;\n"
                                "       margin: 10px 10px;\n"
                                "    }\n"
                                "    p#footer {\n"
                                "       text-align: right;\n"
                                "       font-size: 80%;\n"
                                "    }\n"
                                "</STYLE>\n";

#define HTML_ERROR_BODY_FMT     "<TITLE>%s!</TITLE>\n</HEAD>\n<BODY>\n<H1>%s!</H1>\n<P>\n%s\n</P>\n"

char HTML_ERROR_TAIL[] =        "<P>\n<BR/>&nbsp;<BR/>&nbsp;<BR/>&nbsp;<BR/>&nbsp;\n"
                                FULL_VERSION_STRING "\n"
                                "<BR/>&nbsp;\n"
                                "<HR/>\n"
                                "<P id=\"footer\">\n"
                                "Copyright &copy; 1999-2018 Apache Software Foundation<BR/>\n"
                                "All Rights Reserved\n"
                                "</P>\n</BODY>\n</HTML>\n";

static struct error_reasons {
    int status;
    const char *reason;
    const char *title;
    const char *description;
} error_reasons[] = {
    { 100,
      "Continue",
      NULL,
      NULL
    },
    { 101,
      "Switching Protocols",
      NULL,
      NULL
    },
    { 200,
      "OK",
      NULL,
      NULL
    },
    { 201,
      "Created",
      NULL,
      NULL
    },
    { 202,
      "Accepted",
      NULL,
      NULL
    },
    { 203,
      "Non-Authoritative Information",
      NULL,
      NULL
    },
    { 204,
      "No Content",
      NULL,
      NULL
    },
    { 205,
      "Reset Content",
      NULL,
      NULL
    },
    { 206,
      "Partial Content",
      NULL,
      NULL
    },
    { 300,
      "Multiple Choices",
      NULL,
      NULL
    },
    { 301,
      "Moved Permanently",
      NULL,
      NULL
    },
    { 302,
      "Moved Temporarily",
      NULL,
      NULL
    },
    { 303,
      "See Other",
      NULL,
      NULL
    },
    { 304,
      "Not Modified",
      NULL,
      NULL
    },
    { 305,
      "Use Proxy",
      NULL,
      NULL
    },
    { 400,
      "Bad Request",
      "Bad Request",
      "Your browser (or proxy) sent a request that "
      "this server could not understand."
    },
    { 401,
      "Unauthorized",
      "Access is denied due to invalid credentials",
      "You do not have permission to view this directory or "
      "page using the credentials that you supplied."
    },
    { 402,
      "Payment Required",
      NULL,
      NULL
    },
    { 403,
      "Forbidden",
      "Access is denied",
      "You do not have permission to view this directory or page "
      "using the credentials that you supplied."
    },
    { 404,
      "Not Found",
      "The requested URL was not found on this server",
      "If you entered the URL manually please check your "
      "spelling and try again."
    },
    { 405,
      "Method Not Allowed",
      "HTTP method used to access this page is not allowed",
      "The page you are looking for cannot be displayed because an "
      "invalid method (HTTP method) was used to attempt access."
    },
    { 406,
      "Not Acceptable",
      "Client browser does not accept the MIME type of the requested page",
      "The page you are looking for cannot be opened by your browser "
      "because it has a file name extension that your browser "
      "does not accept."
    },
    { 407,
      "Proxy Authentication Required",
      NULL,
      "The client must first authenticate itself with the proxy."
    },
    { 408,
      "Request Timeout",
      NULL,
      "The client did not produce a request within the time "
      "that the server was prepared to wait."
    },
    { 409,
      "Conflict",
      NULL,
      "The request could not be completed due to a conflict with "
      "the current state of the resource."
    },
    { 410,
      "Gone",
      NULL,
      "The requested resource is no longer available at the "
      "server and no forwarding address is known."
    },
    { 411,
      "Length Required",
      NULL,
      "The server refuses to accept the request without a "
      "defined Content-Length."
    },
    { 412,
      "Precondition Failed",
      NULL,
      "The precondition given in one or more of the request "
      "header fields evaluated to false when it was tested on the server."
    },
    { 413,
      "Request Entity Too Large",
      NULL,
      "The HTTP method does not allow the data transmitted, "
      "or the data volume exceeds the capacity limit."
    },
    { 414,
      "Request-URI Too Long",
      "Submitted URI too large",
      "The length of the requested URL exceeds the capacity limit "
      "for this server. The request cannot be processed."
    },
    { 415,
      "Unsupported Media Type",
      NULL,
      "The server is refusing to service the request because the "
      "entity of the request is in a format not supported by the "
      "requested resource for the requested method."
    },
    { 500,
      "Internal Server Error",
      NULL,
      "The server encountered an internal error and was "
      "unable to complete your request."
    },
    { 501,
      "Not Implemented",
      NULL,
      "The server does not support the functionality required "
      "to fulfill the request."
    },
    { 502,
      "Bad Gateway",
      NULL,
      "There is a problem with the page you are looking for, "
      "and it cannot be displayed. When the Web server (while "
      "acting as a gateway or proxy) contacted the upstream content "
      "server, it received an invalid response from the content server."
    },
    { 503,
      "Service Unavailable",
      "Service Temporarily Unavailable",
      "The server is temporarily unable to service your "
      "request due to maintenance downtime or capacity problems. "
      "Please try again later."
    },
    { 504,
      "Gateway Timeout",
      NULL,
      "The server, while acting as a gateway or proxy, "
      "did not receive a timely response from the upstream server."
    },
    { 505,
      "HTTP Version Not Supported",
      NULL,
      "The server does not support, or refuses to support, the "
      "HTTP protocol version that was used in the request message."
    },
    { 0,
      NULL,
      NULL,
      NULL
    }
};



#define STRNULL_FOR_NULL(x) ((x) ? (x) : "(null)")
#define JK_TOLOWER(x)   ((char)tolower((BYTE)(x)))

#define ISIZEOF(X)      (int)sizeof(X)

#define GET_SERVER_VARIABLE_VALUE(name, place, def)         \
  do {                                                      \
    (place) = dup_server_value(private_data->lpEcb,         \
                               (name),                      \
                               &private_data->p,            \
                               def);                        \
  } while(0)

#define GET_SERVER_VARIABLE_VALUE_INT(name, place, def)     \
  do {                                                      \
    char _tb[RES_BUFFER_SIZE];                              \
    if (get_server_value(private_data->lpEcb,               \
                        (name),                             \
                        _tb,                                \
                        RES_BUFFER_SIZE)) {                 \
        (place) = atoi(_tb);                                \
        if (((place) == 0) && (errno == EINVAL ||           \
                               errno == ERANGE)) {          \
            (place) = def;                                  \
        }                                                   \
    } else {                                                \
        (place) = def;                                      \
  } } while(0)

static char dll_file_path[MAX_PATH];
static char ini_file_name[MAX_PATH];
static char ini_mutex_name[MAX_PATH];
static int using_ini_file = JK_FALSE;
static HANDLE init_cs = NULL;
static volatile int is_inited = JK_FALSE;
static volatile int is_mapread = JK_FALSE;
static BOOL  dll_process_detach = FALSE;
static jk_uri_worker_map_t *uw_map = NULL;
static jk_map_t *workers_map = NULL;
static jk_map_t *rewrite_map = NULL;
static jk_map_t *rregexp_map = NULL;
static jk_map_t *jk_environment_map = NULL;

static jk_logger_t *logger = NULL;
static JK_CRIT_SEC log_cs;
static char *CONTENT_TYPE = "Content-Type:text/html\r\n\r\n";

static char extension_uri[INTERNET_MAX_URL_LENGTH];
static char log_file[MAX_PATH * 2];
static char log_file_effective[MAX_PATH * 2];
static int  log_level = JK_LOG_DEF_LEVEL;
static long log_rotationtime = 0;
static time_t log_next_rotate_time = 0;
static ULONGLONG log_filesize = 0;

static char shm_loaded_name[MAX_PATH] = {0};
static char worker_file[MAX_PATH * 2];
static char worker_mount_file[MAX_PATH * 2] = {0};
static int  worker_mount_reload = JK_URIMAP_DEF_RELOAD;
static char rewrite_rule_file[MAX_PATH * 2] = {0};
static int  shm_config_size = -1;
static int  strip_session = 0;
static int  use_auth_notification_flags = 1;
static int  chunked_encoding_enabled = JK_FALSE;
static int  reject_unsafe = 0;
static volatile int  watchdog_interval = 0;
static HANDLE watchdog_handle = NULL;
static char error_page_buf[INTERNET_MAX_URL_LENGTH] = {0};
static char *error_page = NULL;
static int flush_packets = JK_FALSE;

#define URI_SELECT_OPT_PARSED       0
#define URI_SELECT_OPT_UNPARSED     1
#define URI_SELECT_OPT_ESCAPED      2
#define URI_SELECT_OPT_PROXY        3

static int uri_select_option = URI_SELECT_OPT_PROXY;

static jk_worker_env_t worker_env;

typedef struct isapi_private_data_t isapi_private_data_t;
struct isapi_private_data_t
{
    jk_pool_t p;

    unsigned int bytes_read_so_far;
    int chunk_content;          /* Whether we're responding with Transfer-Encoding: chunked content */
    char *err_hdrs;
    LPEXTENSION_CONTROL_BLOCK lpEcb;
};

typedef struct isapi_log_data_t isapi_log_data_t;
struct isapi_log_data_t {
    char uri[INTERNET_MAX_URL_LENGTH];
    char query[INTERNET_MAX_URL_LENGTH];
    int request_matched;        /* Whether this request (within a multi-request connection)
                                   was handled and needs the log values adjusted */
};

typedef struct iis_info_t iis_info_t;
struct iis_info_t {
    int major;                  /* The major version */
    int minor;                  /* The minor version */
    DWORD filter_notify_event;  /* The primary filter SF_NOTIFY_* event */
};

static iis_info_t iis_info;

static int JK_METHOD start_response(jk_ws_service_t *s,
                                    int status,
                                    const char *reason,
                                    const char *const *header_names,
                                    const char *const *header_values,
                                    unsigned int num_of_headers);

static int JK_METHOD iis_read(jk_ws_service_t *s,
                          void *b, unsigned int l, unsigned int *a);

static int JK_METHOD iis_write(jk_ws_service_t *s, const void *b, unsigned int l);

static int JK_METHOD iis_done(jk_ws_service_t *s);

static int init_ws_service(isapi_private_data_t * private_data,
                           jk_ws_service_t *s, char **worker_name);

static int init_jk(char *serverName);


static int JK_METHOD iis_log_to_file(jk_logger_t *l, int level,
                                    int used, char *what);

static BOOL initialize_extension(void);

static int read_registry_init_data(void);

static int get_config_parameter(LPVOID src, const char *tag,
                                char *val, size_t sz);

static int get_config_bool(LPVOID src, const char *tag, int def);

static int get_config_int(LPVOID src, const char *tag, int def);

static int get_registry_config_parameter(HKEY hkey,
                                         const char *tag, char *b, size_t sz);

static int get_registry_config_number(HKEY hkey, const char *tag,
                                      int *val);


static BOOL get_server_value(LPEXTENSION_CONTROL_BLOCK lpEcb,
                             char *name,
                             char *buf, size_t bufsz);

static char *dup_server_value(LPEXTENSION_CONTROL_BLOCK lpEcb,
                              const char *name, jk_pool_t *p,
                              const char *def);

static int base64_encode_cert_len(int len);

static int base64_encode_cert(char *encoded,
                              const char *string, int len);

static int get_iis_info(iis_info_t *info);

static int isapi_write_client(isapi_private_data_t *p, const char *buf, unsigned int write_length);

static char *path_merge(const char *root, const char *path);

static int is_path_relative(const char *path);

static char x2c(const char *what)
{
    register char digit;

    digit =
        ((what[0] >= 'A') ? ((what[0] & 0xdf) - 'A') + 10 : (what[0] - '0'));
    digit *= 16;
    digit +=
        (what[1] >= 'A' ? ((what[1] & 0xdf) - 'A') + 10 : (what[1] - '0'));
    return (digit);
}

static int unescape_url(char *url)
{
    register int x, y, badesc, badpath;

    badesc = 0;
    badpath = 0;
    for (x = 0, y = 0; url[y]; ++x, ++y) {
        if (url[y] != '%')
            url[x] = url[y];
        else {
            if (!isxdigit(url[y + 1]) || !isxdigit(url[y + 2])) {
                badesc = 1;
                url[x] = '%';
            }
            else {
                url[x] = x2c(&url[y + 1]);
                y += 2;
                if (url[x] == '/' || url[x] == '\0')
                    badpath = 1;
            }
        }
    }
    url[x] = '\0';
    if (badesc)
        return BAD_REQUEST;
    else if (badpath)
        return BAD_PATH;
    else
        return 0;
}

/* Apache code to escape a URL */

#define T_OS_ESCAPE_PATH    (4)

static const BYTE test_char_table[256] = {
     0, 14, 14, 14, 14, 14, 14, 14, 14, 14, 15, 14, 14, 14, 14, 14,
    14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14,
    14,  0,  7,  6,  1,  6,  1,  1,  9,  9,  1,  0,  8,  0,  0, 10,
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  8, 15, 15,  8, 15, 15,
     8,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, 15, 15, 15,  7,  0,
     7,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, 15,  7, 15,  1, 14,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6
};

#define TEST_CHAR(c, f) (test_char_table[(unsigned int)(c)] & (f))

static const char c2x_table[] = "0123456789abcdef";

static BYTE *c2x(unsigned int what, BYTE *where)
{
    *where++ = '%';
    *where++ = c2x_table[what >> 4];
    *where++ = c2x_table[what & 0xf];
    return where;
}

static const char *status_reason(int status)
{
    struct error_reasons *r;

    r = error_reasons;
    while (r->status <= status) {
        if (r->status == status)
            return r->reason;
        else
            r++;
    }
    return "No Reason";
}

static const char *status_title(int status)
{
    struct error_reasons *r;

    r = error_reasons;
    while (r->status <= status) {
        if (r->status == status) {
            if (r->title)
                return r->title;
            else
                return r->reason;
        }
        else
            r++;
    }
    return "Unknown Error";
}

static const char *status_description(int status)
{
    struct error_reasons *r;

    r = error_reasons;
    while (r->status <= status) {
        if (r->status == status) {
            if (r->description)
                return r->description;
            else
                return r->reason;
        }
        else
            r++;
    }
    return "Unknown Error";
}

static int escape_url(const char *path, char *dest, int destsize)
{
    const BYTE *s = (const BYTE *)path;
    BYTE *d = (BYTE *)dest;
    BYTE *e = d + destsize - 1;
    BYTE *ee = d + destsize - 3;

    while (*s) {
        if (TEST_CHAR(*s, T_OS_ESCAPE_PATH)) {
            if (d >= ee)
                return JK_FALSE;
            d = c2x(*s, d);
        }
        else {
            if (d >= e)
                return JK_FALSE;
            *d++ = *s;
        }
        ++s;
    }
    *d = '\0';
    return JK_TRUE;
}

/*
 * Find the first occurrence of find in s.
 */
static char *stristr(const char *s, const char *find)
{
    char c, sc;
    size_t len;

    if ((c = JK_TOLOWER(*find++)) != 0) {
        len = strlen(find);
        do {
            do {
                if ((sc = JK_TOLOWER(*s++)) == 0)
                    return (NULL);
            } while (sc != c);
        } while (strnicmp(s, find, len) != 0);
        s--;
    }
    return ((char *)s);
}

static void write_error_response(PHTTP_FILTER_CONTEXT pfc, int err)
{
    char status[1024];
    char body[8192] = "";
    DWORD len;

    /* reject !!! */
    pfc->AddResponseHeaders(pfc, CONTENT_TYPE, 0);
    StringCbPrintf(status, sizeof(status), "%d %s", err, status_reason(err));
    pfc->ServerSupportFunction(pfc,
                               SF_REQ_SEND_RESPONSE_HEADER,
                               status, 0, 0);
    len = ISIZEOF(HTML_ERROR_HEAD) - 1;
    pfc->WriteClient(pfc, HTML_ERROR_HEAD, &len,
                     HSE_IO_SYNC);
    StringCbPrintf(body, sizeof(body), HTML_ERROR_BODY_FMT,
                   status_reason(err), status_title(err),
                   status_description(err));
    len = lstrlenA(body);
    pfc->WriteClient(pfc, body, &len,
                     HSE_IO_SYNC);
    len = ISIZEOF(HTML_ERROR_TAIL) - 1;
    pfc->WriteClient(pfc, HTML_ERROR_TAIL, &len,
                     HSE_IO_SYNC);
}

static void write_error_message(LPEXTENSION_CONTROL_BLOCK lpEcb, int err, const char *err_hdrs)
{
    DWORD len;
    char status[1024];
    char body[8192] = "";

    if (error_page) {
        char error_page_url[INTERNET_MAX_URL_LENGTH] = "";
        DWORD len_of_error_page;
        StringCbPrintf(error_page_url, INTERNET_MAX_URL_LENGTH,
                       error_page, err);
        len_of_error_page = lstrlenA(error_page_url);
        if (!lpEcb->ServerSupportFunction(lpEcb->ConnID,
                                          HSE_REQ_SEND_URL_REDIRECT_RESP,
                                          error_page_url,
                                          &len_of_error_page,
                                          NULL)) {
            lpEcb->dwHttpStatusCode = err;
        }
        else {
            return;
        }
    }
    lpEcb->dwHttpStatusCode = err;
    StringCbPrintf(status, sizeof(status), "%d %s", err, status_reason(err));
    /* XXX: Should we allow something beside 401?
     */
    if (err_hdrs && err == 401) {
        /* Include extra error headers */
        HRESULT hr = StringCbCopy(body, sizeof(body), err_hdrs);
        if (FAILED(hr) || strlen(body) > (8191 - strlen(CONTENT_TYPE))) {
            /* Header is too long.
             */
            jk_log(logger, JK_LOG_WARNING,
                   "error header too long (%d bytes requested).",
                   lstrlenA(err_hdrs));
            body[0] = '\0';
        }
    }
    StringCbCat(body, sizeof(body), CONTENT_TYPE);
    lpEcb->ServerSupportFunction(lpEcb->ConnID,
                                 HSE_REQ_SEND_RESPONSE_HEADER,
                                 status,
                                 0,
                                 (LPDWORD)body);
    /* First write the HEAD */
    len = ISIZEOF(HTML_ERROR_HEAD) - 1;
    lpEcb->WriteClient(lpEcb->ConnID,
                       HTML_ERROR_HEAD, &len,
                       HSE_IO_SYNC);
    StringCbPrintf(body, sizeof(body), HTML_ERROR_BODY_FMT,
                   status_reason(err), status_title(err),
                   status_description(err));
    len = lstrlenA(body);
    lpEcb->WriteClient(lpEcb->ConnID,
                       body, &len,
                       HSE_IO_SYNC);
    len = ISIZEOF(HTML_ERROR_TAIL) - 1;
    lpEcb->WriteClient(lpEcb->ConnID,
                       HTML_ERROR_TAIL, &len,
                       HSE_IO_SYNC);

}


static int JK_METHOD start_response(jk_ws_service_t *s,
                                    int status,
                                    const char *reason,
                                    const char *const *header_names,
                                    const char *const *header_values,
                                    unsigned int num_of_headers)
{
    int rv = JK_TRUE;
    isapi_private_data_t *p;

    JK_TRACE_ENTER(logger);
    if (status < 100 || status > 1000) {
        jk_log(logger, JK_LOG_ERROR,
               "invalid status %d",
               status);
        JK_TRACE_EXIT(logger);
        return JK_FALSE;
    }
    if (s == NULL || s->ws_private == NULL) {
        JK_LOG_NULL_PARAMS(logger);
        JK_TRACE_EXIT(logger);
        return JK_FALSE;
    }
    p = s->ws_private;

    /* If we use proxy error pages, still pass
     * through context headers needed for special status codes.
     */
    if (s->extension.use_server_error_pages &&
        status >= s->extension.use_server_error_pages) {
        if (status == JK_HTTP_UNAUTHORIZED) {
            int found = JK_FALSE;
            unsigned int h;
            for (h = 0; h < num_of_headers; h++) {
                if (!strcasecmp(header_names[h], "WWW-Authenticate")) {
                    p->err_hdrs = jk_pool_strcatv(&p->p,
                                        "WWW-Authenticate:",
                                        header_values[h], CRLF, NULL);
                    found = JK_TRUE;
                }
            }
            if (found == JK_FALSE) {
                jk_log(logger, JK_LOG_INFO,
                       "origin server sent 401 without"
                       " WWW-Authenticate header");
            }
        }
        return JK_TRUE;
    }

    if (!s->response_started) {
        char *status_str = NULL;
        char *headers_str = NULL;
        BOOL keep_alive = FALSE;     /* Whether the downstream or us can supply content length */
        BOOL rc;
        size_t i, len_of_headers = 0;

        s->response_started = JK_TRUE;

        if (JK_IS_DEBUG_LEVEL(logger)) {
            jk_log(logger, JK_LOG_DEBUG, "Starting response for URI '%s' (protocol %s)",
                   s->req_uri, s->protocol);
        }

        /*
         * Create the status line
         */
        if (!reason) {
            reason = status_reason(status);
        }
        status_str = (char *)malloc((6 + strlen(reason)));
        StringCbPrintf(status_str, 6 + strlen(reason), "%d %s", status, reason);

        if (chunked_encoding_enabled) {
            /* Check if we've got an HTTP/1.1 response */
            if (!strcasecmp(s->protocol, "HTTP/1.1")) {
                keep_alive = TRUE;
                /* Chunking only when HTTP/1.1 client and enabled */
                p->chunk_content = JK_TRUE;
            }
        }

        /*
         * Create response headers string
         */

        /* Calculate length of headers block */
        for (i = 0; i < num_of_headers; i++) {
            len_of_headers += strlen(header_names[i]);
            len_of_headers += strlen(header_values[i]);
            len_of_headers += 4;   /* extra for colon, space and crlf */
        }

        /*
         * Exclude status codes that MUST NOT include message bodies
         */
        if ((status == 204) || (status == 205) || (status == 304)) {
            p->chunk_content = JK_FALSE;
            /* Keep alive is still possible */
            if (JK_IS_DEBUG_LEVEL(logger))
                jk_log(logger, JK_LOG_DEBUG, "Response status %d implies no message body", status );
        }
        if (p->chunk_content) {
            for (i = 0; i < num_of_headers; i++) {
                /* Check the downstream response to see whether
                 * it's appropriate to chunk the response content
                 * and whether it supports keeping the connection open.

                 * This implements the rules for HTTP/1.1 message length determination
                 * with the exception of multipart/byteranges media types.
                 * http://www.w3.org/Protocols/rfc2616/rfc2616-sec4.html#sec4.4
                 */
                if (!strcasecmp(CONTENT_LENGTH_HEADER_NAME, header_names[i])) {
                    p->chunk_content = JK_FALSE;
                    if (JK_IS_DEBUG_LEVEL(logger))
                        jk_log(logger, JK_LOG_DEBUG, "Response specifies Content-Length" );
                }
                else if (!strcasecmp(CONNECTION_HEADER_NAME, header_names[i])
                        && !strcasecmp(CONNECTION_CLOSE_VALUE, header_values[i])) {
                    keep_alive = FALSE;
                    p->chunk_content = JK_FALSE;
                    if (JK_IS_DEBUG_LEVEL(logger))
                        jk_log(logger, JK_LOG_DEBUG, "Response specifies Connection: Close" );
                }
                else if (!strcasecmp(TRANSFER_ENCODING_HEADER_NAME, header_names[i])
                        && !strcasecmp(TRANSFER_ENCODING_IDENTITY_VALUE, header_values[i])) {
                    /* HTTP states that this must include 'chunked' as the last value.
                        * 'identity' is the same as absence of the header */
                    p->chunk_content = JK_FALSE;
                    if (JK_IS_DEBUG_LEVEL(logger))
                        jk_log(logger, JK_LOG_DEBUG, "Response specifies Transfer-Encoding" );
                }
            }

            /* Provide room in the buffer for the Transfer-Encoding header if we use it. */
            len_of_headers += TRANSFER_ENCODING_CHUNKED_HEADER_COMPLETE_LEN + 2;
        }

        /* Allocate and init the headers string */
        len_of_headers += 3;       /* crlf and terminating null char */
        headers_str = (char *)malloc(len_of_headers);
        headers_str[0] = '\0';

        /* Copy headers into headers block for sending */
        for (i = 0; i < num_of_headers; i++) {
            StringCbCat(headers_str, len_of_headers, header_names[i]);
            StringCbCat(headers_str, len_of_headers, ": ");
            StringCbCat(headers_str, len_of_headers, header_values[i]);
            StringCbCat(headers_str, len_of_headers, CRLF);
        }

        if (p->chunk_content) {
            /* Configure the response if chunked encoding is used */
            if (JK_IS_DEBUG_LEVEL(logger))
                jk_log(logger, JK_LOG_DEBUG, "Using Transfer-Encoding: chunked");

            /** We will supply the transfer-encoding to allow IIS to keep the connection open */
            keep_alive = TRUE;

            /* Indicate to the client that the content will be chunked
            - We've already reserved space for this */
            StringCbCat(headers_str, len_of_headers, TRANSFER_ENCODING_CHUNKED_HEADER_COMPLETE);
            StringCbCat(headers_str, len_of_headers, CRLF);
        }

        /* Terminate the headers */
        StringCbCat(headers_str, len_of_headers, CRLF);

        if (JK_IS_DEBUG_LEVEL(logger))
            jk_log(logger, JK_LOG_DEBUG, "%ssing Keep-Alive", (keep_alive ? "U" : "Not u"));

        if (keep_alive) {
            HSE_SEND_HEADER_EX_INFO hi;

            /* Fill in the response */
            hi.pszStatus = status_str;
            hi.pszHeader = headers_str;
            hi.cchStatus = lstrlenA(status_str);
            hi.cchHeader = lstrlenA(headers_str);

            /*
             * Using the extended form of the API means we have to get this right,
             * i.e. IIS won't keep connections open if there's a Content-Length and close them if there isn't.
             */
            hi.fKeepConn = keep_alive;

            /* Send the response to the client */
            rc = p->lpEcb->ServerSupportFunction(p->lpEcb->ConnID,
                                                  HSE_REQ_SEND_RESPONSE_HEADER_EX,
                                                  &hi,
                                                  NULL, NULL);
        }
        else {
            /* Old style response - forces Connection: close if Tomcat response doesn't
               specify necessary details to allow keep alive */
            rc = p->lpEcb->ServerSupportFunction(p->lpEcb->ConnID,
                                                  HSE_REQ_SEND_RESPONSE_HEADER,
                                                  status_str,
                                                  0,
                                                  (LPDWORD)headers_str);
        }

        if (!rc) {
            jk_log(logger, JK_LOG_ERROR,
                   "HSE_REQ_SEND_RESPONSE_HEADER%s failed with error=%d (0x%08x)",
                   (keep_alive ? "_EX" : ""), GetLastError(), GetLastError());
            rv = JK_FALSE;
        }
        if (headers_str)
            free(headers_str);
        if (status_str)
            free(status_str);
    }
    JK_TRACE_EXIT(logger);
    return rv;
}

static int JK_METHOD iis_read(jk_ws_service_t *s,
                              void *b, unsigned int l, unsigned int *a)
{
    isapi_private_data_t *p;

    JK_TRACE_ENTER(logger);
    if (s == NULL || s->ws_private == NULL) {
        JK_LOG_NULL_PARAMS(logger);
        JK_TRACE_EXIT(logger);
        return JK_FALSE;
    }
    if (a == NULL  || b == NULL) {
        /* XXX: Do we really need those useless checks?
         */
        JK_LOG_NULL_PARAMS(logger);
        JK_TRACE_EXIT(logger);
        return JK_FALSE;

    }
    p = s->ws_private;
    if (JK_IS_DEBUG_LEVEL(logger)) {
        jk_log(logger, JK_LOG_DEBUG,
               "Preparing to read %d bytes. "
               "ECB reports %d bytes total, with %d available.",
               l, p->lpEcb->cbTotalBytes, p->lpEcb->cbAvailable);
    }

    *a = 0;
    if (l) {
        char *buf = b;
        DWORD already_read = p->lpEcb->cbAvailable - p->bytes_read_so_far;

        if (already_read >= l) {
            if (JK_IS_DEBUG_LEVEL(logger)) {
                jk_log(logger, JK_LOG_DEBUG,
                       "Already read %d bytes - supplying %d bytes from buffer",
                       already_read, l);
            }
            memcpy(buf, p->lpEcb->lpbData + p->bytes_read_so_far, l);
            p->bytes_read_so_far += l;
            *a = l;
        }
        else {
            /*
             * Try to copy what we already have
             */
            if (already_read > 0) {
                if (JK_IS_DEBUG_LEVEL(logger)) {
                    jk_log(logger, JK_LOG_DEBUG,
                           "Supplying %d bytes from buffer",
                           already_read);
                }
                memcpy(buf, p->lpEcb->lpbData + p->bytes_read_so_far,
                       already_read);
                buf += already_read;
                l -= already_read;
                p->bytes_read_so_far = p->lpEcb->cbAvailable;

                *a = already_read;
            }

            /*
             * Now try to read from the client ...
             */
            if (JK_IS_DEBUG_LEVEL(logger)) {
                jk_log(logger, JK_LOG_DEBUG,
                       "Attempting to read %d bytes from client", l);
            }
            if (p->lpEcb->ReadClient(p->lpEcb->ConnID, buf, &l)) {
                /* ReadClient will succeed with dwSize == 0 for last chunk
                   if request chunk encoded */
                *a += l;
            }
            else {
                jk_log(logger, JK_LOG_ERROR,
                       "ReadClient failed with %d (0x%08x)", GetLastError(), GetLastError());
                JK_TRACE_EXIT(logger);
                return JK_FALSE;
            }
        }
    }
    JK_TRACE_EXIT(logger);
    return JK_TRUE;
}

/*
 * Writes a buffer to the ISAPI response.
 */
static int isapi_write_client(isapi_private_data_t *p, const char *buf, unsigned int write_length)
{
    unsigned int written = 0;
    DWORD try_to_write = 0;

    JK_TRACE_ENTER(logger);

    if (JK_IS_DEBUG_LEVEL(logger))
        jk_log(logger, JK_LOG_DEBUG, "Writing %d bytes of data to client", write_length);

    while (written < write_length) {
        try_to_write = write_length - written;
        if (!p->lpEcb->WriteClient(p->lpEcb->ConnID,
                                   (LPVOID)(buf + written), &try_to_write, HSE_IO_SYNC)) {
            jk_log(logger, JK_LOG_ERROR,
                   "WriteClient failed with %d (0x%08x)", GetLastError(), GetLastError());
            JK_TRACE_EXIT(logger);
            return JK_FALSE;
        }
        written += try_to_write;
        if (JK_IS_DEBUG_LEVEL(logger))
            jk_log(logger, JK_LOG_DEBUG, "Wrote %d bytes of data successfully", try_to_write);
    }
    JK_TRACE_EXIT(logger);
    return JK_TRUE;
}

/*
 * Write content to the response.
 * If chunked encoding has been enabled and the client supports it
 *(and it's appropriate for the response), then this will write a
 * single "Transfer-Encoding: chunked" chunk
 */
static int JK_METHOD iis_write(jk_ws_service_t *s, const void *b, unsigned int l)
{
    isapi_private_data_t *p;
    const char *buf = (const char *)b;

    JK_TRACE_ENTER(logger);
    if (!l) {
        JK_TRACE_EXIT(logger);
        return JK_TRUE;
    }
    if (s == NULL || s->ws_private == NULL || b == NULL) {
        JK_LOG_NULL_PARAMS(logger);
        JK_TRACE_EXIT(logger);
        return JK_FALSE;
    }
    p = s->ws_private;

    if (!s->response_started) {
        start_response(s, 200, NULL, NULL, NULL, 0);
    }

    if (p->chunk_content == JK_FALSE) {
        if (isapi_write_client(p, buf, l) == JK_FALSE) {
            JK_TRACE_EXIT(logger);
            return JK_FALSE;
        }
    }
    else {
        char chunk_header[RES_BUFFER_SIZE];

        /* Construct chunk header : HEX CRLF*/
        StringCbPrintf(chunk_header, RES_BUFFER_SIZE, "%X%s", l, CRLF);

        if (iis_info.major >= 6) {
            HSE_RESPONSE_VECTOR response_vector;
            HSE_VECTOR_ELEMENT response_elements[3];

            response_elements[0].ElementType = HSE_VECTOR_ELEMENT_TYPE_MEMORY_BUFFER;
            response_elements[0].pvContext = chunk_header;
            response_elements[0].cbOffset = 0;
            response_elements[0].cbSize = strlen(chunk_header);

            response_elements[1].ElementType = HSE_VECTOR_ELEMENT_TYPE_MEMORY_BUFFER;
            response_elements[1].pvContext = (PVOID)buf;
            response_elements[1].cbOffset = 0;
            response_elements[1].cbSize = l;

            response_elements[2].ElementType = HSE_VECTOR_ELEMENT_TYPE_MEMORY_BUFFER;
            response_elements[2].pvContext = CRLF;
            response_elements[2].cbOffset = 0;
            response_elements[2].cbSize = CRLF_LEN;

            response_vector.dwFlags = HSE_IO_SYNC;
            response_vector.pszStatus = NULL;
            response_vector.pszHeaders = NULL;
            response_vector.nElementCount = 3;
            response_vector.lpElementArray = response_elements;

            if (JK_IS_DEBUG_LEVEL(logger))
                jk_log(logger, JK_LOG_DEBUG,
                       "Using vector write for chunk encoded %d byte chunk", l);

            if (!p->lpEcb->ServerSupportFunction(p->lpEcb->ConnID,
                HSE_REQ_VECTOR_SEND,
                &response_vector,
                NULL, NULL)) {
                    jk_log(logger, JK_LOG_ERROR,
                           "Vector write of chunk encoded response failed with %d (0x%08x)",
                           GetLastError(), GetLastError());
                    JK_TRACE_EXIT(logger);
                    return JK_FALSE;
            }
        }
        else {
            /* Write chunk header */
            if (JK_IS_DEBUG_LEVEL(logger))
                jk_log(logger, JK_LOG_DEBUG,
                "Using chunked encoding - writing chunk header for %d byte chunk", l);

            if (!isapi_write_client(p, chunk_header, lstrlenA(chunk_header))) {
                jk_log(logger, JK_LOG_ERROR, "WriteClient for chunk header failed");
                JK_TRACE_EXIT(logger);
                return JK_FALSE;
            }

            /* Write chunk body (or simple body block) */
            if (JK_IS_DEBUG_LEVEL(logger)) {
                jk_log(logger, JK_LOG_DEBUG, "Writing %s of size %d",
                       (p->chunk_content ? "chunk body" : "simple response"), l);
            }
            if (!isapi_write_client(p, buf, l)) {
                jk_log(logger, JK_LOG_ERROR, "WriteClient for response body chunk failed");
                JK_TRACE_EXIT(logger);
                return JK_FALSE;
            }
            /* Write chunk trailer */
            if (JK_IS_DEBUG_LEVEL(logger)) {
                jk_log(logger, JK_LOG_DEBUG, "Using chunked encoding - writing chunk trailer");
            }

            if (!isapi_write_client(p, CRLF, CRLF_LEN)) {
                jk_log(logger, JK_LOG_ERROR, "WriteClient for chunk trailer failed");
                JK_TRACE_EXIT(logger);
                return JK_FALSE;
            }
        }
    }
    JK_TRACE_EXIT(logger);
    return JK_TRUE;
}

/**
 * In the case of a Transfer-Encoding: chunked response, this will write the terminator chunk.
 */
static int JK_METHOD iis_done(jk_ws_service_t *s)
{
    isapi_private_data_t *p;

    JK_TRACE_ENTER(logger);
    if (s == NULL || s->ws_private == NULL) {
        JK_LOG_NULL_PARAMS(logger);
        JK_TRACE_EXIT(logger);
        return JK_FALSE;
    }
    p = s->ws_private;
    if (p->chunk_content == JK_FALSE) {
        JK_TRACE_EXIT(logger);
        return JK_TRUE;
    }

    /* Write last chunk + terminator */
    if (iis_info.major >= 6) {
        HSE_RESPONSE_VECTOR response_vector;
        HSE_VECTOR_ELEMENT response_elements[1];

        response_elements[0].ElementType = HSE_VECTOR_ELEMENT_TYPE_MEMORY_BUFFER;
        response_elements[0].pvContext = CHUNKED_ENCODING_TRAILER;
        response_elements[0].cbOffset = 0;
        response_elements[0].cbSize = CHUNKED_ENCODING_TRAILER_LEN;

        /* HSE_IO_FINAL_SEND lets IIS process the response to the client before we return */
        response_vector.dwFlags = HSE_IO_SYNC | HSE_IO_FINAL_SEND;
        response_vector.pszStatus = NULL;
        response_vector.pszHeaders = NULL;
        response_vector.nElementCount = 1;
        response_vector.lpElementArray = response_elements;

        if (JK_IS_DEBUG_LEVEL(logger))
            jk_log(logger, JK_LOG_DEBUG,
                   "Using vector write to terminate chunk encoded response.");

        if (!p->lpEcb->ServerSupportFunction(p->lpEcb->ConnID,
                                             HSE_REQ_VECTOR_SEND,
                                             &response_vector,
                                             NULL, NULL)) {
                jk_log(logger, JK_LOG_ERROR,
                       "Vector termination of chunk encoded response failed with %d (0x%08x)",
                       GetLastError(), GetLastError());
                JK_TRACE_EXIT(logger);
                return JK_FALSE;
        }
    }
    else {
        if (JK_IS_DEBUG_LEVEL(logger))
            jk_log(logger, JK_LOG_DEBUG, "Terminating chunk encoded response");

        if (!isapi_write_client(p, CHUNKED_ENCODING_TRAILER, CHUNKED_ENCODING_TRAILER_LEN)) {
            jk_log(logger, JK_LOG_ERROR,
                   "WriteClient for chunked response terminator failed with %d (0x%08x)",
                   GetLastError(), GetLastError());
            JK_TRACE_EXIT(logger);
            return JK_FALSE;
        }
    }

    JK_TRACE_EXIT(logger);
    return JK_TRUE;
}

BOOL WINAPI GetFilterVersion(PHTTP_FILTER_VERSION pVer)
{
    BOOL rv = TRUE;
    ULONG http_filter_revision = HTTP_FILTER_REVISION;

    pVer->dwFilterVersion = pVer->dwServerFilterVersion;

    if (pVer->dwFilterVersion > http_filter_revision) {
        pVer->dwFilterVersion = http_filter_revision;
    }
    WaitForSingleObject(init_cs, INFINITE);
    if (!is_inited) {
        rv = initialize_extension();
    }
    ReleaseMutex(init_cs);
    if (iis_info.major < 5 || (iis_info.major == 5 && iis_info.minor < 1)) {
        SetLastError(ERROR_OLD_WIN_VERSION);
        return FALSE;
    }
    pVer->dwFlags = SF_NOTIFY_ORDER_HIGH |
                    SF_NOTIFY_SECURE_PORT |
                    SF_NOTIFY_NONSECURE_PORT |
                    SF_NOTIFY_LOG |
                    iis_info.filter_notify_event;

    StringCbCopy(pVer->lpszFilterDesc, SF_MAX_FILTER_DESC_LEN, (VERSION_STRING));
    return rv;
}


/** The max number of regex captures that can be expanded by ap_pregsub */
#define AP_MAX_REG_MATCH 10

/* The structure representing a compiled regular expression. */
typedef struct {
    void *re_pcre;
    const char *fake;
} ap_regex_t;

/* The structure in which a captured offset is returned. */
typedef struct {
    int rm_so;
    int rm_eo;
} ap_regmatch_t;


/*************************************************
 *           Free store held by a regex          *
 *************************************************/

static void ap_regfree(ap_regex_t *preg)
{
    (pcre_free)(preg->re_pcre);
}


/*************************************************
 *            Compile a regular expression       *
 *************************************************/

/*
Arguments:
  preg        points to a structure for recording the compiled expression
  pattern     the pattern to compile

Returns:      JK_TRUE on success
              JK_FALSE on failure
*/

static int ap_regcomp(ap_regex_t *preg, const char *pattern)
{
    const char *errorptr;
    int erroffset;

    preg->re_pcre = pcre_compile(pattern, 0, &errorptr, &erroffset, NULL);

    if (preg->re_pcre == NULL)
        return JK_FALSE;

    return JK_TRUE;
}

/*************************************************
 *              Match a regular expression       *
 *************************************************/

/* Unfortunately, PCRE requires 3 ints of working space for each captured
substring, so we have to get and release working store instead of just using
the POSIX structures as was done in earlier releases when PCRE needed only 2
ints. However, if the number of possible capturing brackets is small, use a
block of store on the stack, to reduce the use of malloc/free. The threshold is
in a macro that can be changed at configure time. */

static int ap_regexec(const ap_regex_t *preg, const char *string,
                      int nmatch, ap_regmatch_t pmatch[])
{
    int rc;
    int *ovector = NULL;
    int small_ovector[POSIX_MALLOC_THRESHOLD * 3];
    int allocated_ovector = 0;

    if (nmatch > 0) {
        if (nmatch <= POSIX_MALLOC_THRESHOLD) {
            ovector = &(small_ovector[0]);
        }
        else {
            ovector = (int *)malloc(sizeof(int) * nmatch * 3);
            if (ovector == NULL)
                return -1;
            allocated_ovector = 1;
        }
    }

    rc = pcre_exec((const pcre *)preg->re_pcre, NULL, string,
                   lstrlenA(string),
                    0, 0, ovector, nmatch * 3);

    if (rc == 0)
        rc = nmatch;    /* All captured slots were filled in */
    if (rc >= 0) {
        int i;
        for (i = 0; i < rc; i++) {
            pmatch[i].rm_so = ovector[i*2];
            pmatch[i].rm_eo = ovector[i*2+1];
        }
        for (; i < nmatch; i++)
            pmatch[i].rm_so = pmatch[i].rm_eo = -1;
    }
    if (allocated_ovector)
        free(ovector);
    return rc;
}

/* This function substitutes for $0-$9, filling in regular expression
 * submatches. Pass it the same nmatch and pmatch arguments that you
 * passed ap_regexec(). pmatch should not be greater than the maximum number
 * of subexpressions - i.e. one more than the re_nsub member of ap_regex_t.
 *
 * input should be the string with the $-expressions, source should be the
 * string that was matched against.
 *
 * It returns the substituted string, or NULL on error.
 *
 * Parts of this code are based on Henry Spencer's regsub(), from his
 * AT&T V8 regexp package.
 */

static char *ap_pregsub(const char *input,
                        const char *source, int nmatch,
                        ap_regmatch_t pmatch[])
{
    const char *src = input;
    char *dest, *dst;
    char c;
    int no;
    int len;

    if (!source)
        return NULL;
    if (!nmatch)
        return strdup(src);

    /* First pass, find the size */
    len = 0;

    while ((c = *src++) != '\0') {
        if (c == '&')
            no = 0;
        else if (c == '$' && isdigit((unsigned char)*src))
            no = *src++ - '0';
        else
            no = 10;

        if (no > 9) {                /* Ordinary character. */
            if (c == '\\' && (*src == '$' || *src == '&'))
                c = *src++;
            len++;
        }
        else if (no < nmatch && pmatch[no].rm_so < pmatch[no].rm_eo) {
            len += pmatch[no].rm_eo - pmatch[no].rm_so;
        }

    }

    dest = dst = calloc(1, len + 1);

    /* Now actually fill in the string */

    src = input;

    while ((c = *src++) != '\0') {
        if (c == '&')
            no = 0;
        else if (c == '$' && isdigit((unsigned char)*src))
            no = *src++ - '0';
        else
            no = 10;

        if (no > 9) {                /* Ordinary character. */
            if (c == '\\' && (*src == '$' || *src == '&'))
                c = *src++;
            *dst++ = c;
        }
        else if (no < nmatch && pmatch[no].rm_so < pmatch[no].rm_eo) {
            len = pmatch[no].rm_eo - pmatch[no].rm_so;
            memcpy(dst, source + pmatch[no].rm_so, len);
            dst += len;
        }

    }
    *dst = '\0';
    return dest;
}

static char *simple_rewrite(jk_pool_t *p, const char *uri)
{
    if (rewrite_map) {
        int i;
        for (i = 0; i < jk_map_size(rewrite_map); i++) {
            size_t len;
            const char *src = jk_map_name_at(rewrite_map, i);
            if (*src == '~')
                continue;   /* Skip regexp rewrites */
            len = strlen(src);
            if (strncmp(uri, src, len) == 0) {
                return jk_pool_strcat(p, jk_map_value_at(rewrite_map, i), uri + len);
            }
        }
    }
    return NULL;
}

static char *rregex_rewrite(jk_pool_t *p, const char *uri)
{
    int nmatch;
    ap_regmatch_t regm[AP_MAX_REG_MATCH];

    if (rregexp_map) {
        int i;
        for (i = 0; i < jk_map_size(rregexp_map); i++) {
            ap_regex_t *regexp = (ap_regex_t *)jk_map_value_at(rregexp_map, i);
            nmatch = ap_regexec(regexp, uri, AP_MAX_REG_MATCH, regm);
            if (nmatch > 0) {
                char *subs;
                if (nmatch > AP_MAX_REG_MATCH) {
                    nmatch = AP_MAX_REG_MATCH;
                }
                subs = ap_pregsub(regexp->fake, uri, nmatch, regm);
                if (subs) {
                    char *buf, *ptr;
                    size_t orgsz = strlen(uri);
                    size_t subsz = strlen(subs);
                    size_t lefts = orgsz - regm[0].rm_eo;

                    ptr = buf = jk_pool_alloc(p, regm[0].rm_so + subsz + lefts + 1);
                    memcpy(buf, uri, regm[0].rm_so);
                    ptr += regm[0].rm_so;
                    memcpy(ptr, subs, subsz);
                    ptr += subsz;
                    memcpy(ptr, uri + regm[0].rm_eo, lefts);
                    ptr[lefts] = '\0';
                    return buf;
                }
            }
        }
    }
    return NULL;
}

static __inline void clear_header(PHTTP_FILTER_PREPROC_HEADERS pfp,
                                 PHTTP_FILTER_CONTEXT pfc,
                                 LPSTR lpszName)
{
    pfp->SetHeader(pfc, lpszName, NIL);
}

static __inline LPSTR get_pheader(jk_pool_t *pool,
                                  PHTTP_FILTER_PREPROC_HEADERS pfp,
                                  PHTTP_FILTER_CONTEXT pfc,
                                  LPSTR lpszName, LPSTR lpszBuf, size_t bsize)
{
    LPSTR rv    = lpszBuf;
    DWORD dwLen = (DWORD)bsize;

    if (!pfp->GetHeader(pfc, lpszName, lpszBuf, &dwLen)) {
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER)
            return NULL;
        if ((rv = jk_pool_alloc(pool, dwLen)) == NULL)
            return NULL;
        /* Try again with dynamic buffer */
        if (!pfp->GetHeader(pfc, lpszName, rv, &dwLen))
            return NULL;
    }
    return rv;
}

static DWORD handle_notify_event(PHTTP_FILTER_CONTEXT pfc,
                                 PHTTP_FILTER_PREPROC_HEADERS pfp)
{
    int rc;
    DWORD rv = SF_STATUS_REQ_NEXT_NOTIFICATION;
    const char *worker = NULL;
    rule_extension_t *extensions;
    int worker_index = -1;
    int port = 0;
    jk_pool_atom_t pbuf[HUGE_POOL_SIZE];
    jk_pool_t pool;

    char *uri_undec  = NULL;
    char *cleanuri  = NULL;
    char *uri = NULL;
    char *host = NULL;
    char *translate = NULL;
    char szHB[HDR_BUFFER_SIZE] = "/";
    char szUB[HDR_BUFFER_SIZE] = "";
    char szTB[16] = "";
    char szPB[16] = "";
    char swindex[32] = "";
    char *query;
    DWORD len;

    if (JK_IS_DEBUG_LEVEL(logger))
        jk_log(logger, JK_LOG_DEBUG, "Filter started");
    jk_open_pool(&pool, pbuf, sizeof(jk_pool_atom_t) * HUGE_POOL_SIZE);
    /*
     * Just in case somebody set these headers in the request!
     */
    clear_header(pfp, pfc, URI_HEADER_NAME);
    clear_header(pfp, pfc, QUERY_HEADER_NAME);
    clear_header(pfp, pfc, WORKER_HEADER_NAME);
    clear_header(pfp, pfc, WORKER_HEADER_INDEX);
    clear_header(pfp, pfc, TOMCAT_TRANSLATE_HEADER_NAME);

    /*
     * Suppress logging of original uri/query when we don't map a URL
     */
    if (pfc->pFilterContext) {
        isapi_log_data_t *ld = (isapi_log_data_t *)pfc->pFilterContext;
        ld->request_matched = JK_FALSE;
    }
    uri = get_pheader(&pool, pfp, pfc, "url", szUB, sizeof(szUB));
    if (uri == NULL) {
        jk_log(logger, JK_LOG_ERROR,
               "error while getting the url");
        return SF_STATUS_REQ_ERROR;
    }
    if (*uri == '\0') {
        /* Empty url */
        return SF_STATUS_REQ_NEXT_NOTIFICATION;
    }
    query = strchr(uri, '?');
    if (query) {
        *query++ = '\0';
        if (*query)
            query = jk_pool_strdup(&pool, query);
        else
            query = NULL;
    }

    /* Duplicate unparsed uri */
	uri_undec = jk_pool_strdup(&pool, uri);

    rc = unescape_url(uri);
    if (rc == BAD_REQUEST) {
        jk_log(logger, JK_LOG_ERROR,
               "[%s] contains one or more invalid escape sequences.",
               uri);
        write_error_response(pfc, 400);
        rv = SF_STATUS_REQ_FINISHED;
        goto cleanup;
    }
    if (rc == BAD_PATH) {
        jk_log(logger, JK_LOG_EMERG,
               "[%s] contains forbidden escape sequences.",
               uri);
        write_error_response(pfc, 404);
        rv = SF_STATUS_REQ_FINISHED;
        goto cleanup;
    }
    cleanuri = jk_pool_strdup(&pool, uri);
    if (jk_servlet_normalize(cleanuri, logger)) {
        write_error_response(pfc, 404);
        rv = SF_STATUS_REQ_FINISHED;
        goto cleanup;
    }
    len = ISIZEOF(szHB) - 1;
    if (pfc->GetServerVariable(pfc, "SERVER_NAME", &szHB[1], &len) && len > 1) {
        len = ISIZEOF(szPB);
        if (pfc->GetServerVariable(pfc, "SERVER_PORT", szPB, &len))
            port = atoi(szPB);
        if (port != 0 && port != 80 && port != 443) {
            host = jk_pool_strcatv(&pool, szHB, ":", szPB, NULL);
        }
        else
            host = szHB;
    }
    worker = map_uri_to_worker_ext(uw_map, cleanuri, host,
                                   &extensions, &worker_index, logger);
    /*
     * Check if somebody is feading us with his own TOMCAT data headers.
     * We reject such postings !
     */
    if (worker) {
        char *forwardURI;
        char *rewriteURI;
        isapi_log_data_t *ld;
        BOOL rs;

        /* This is a servlet, should redirect ... */
        if (JK_IS_DEBUG_LEVEL(logger))
            jk_log(logger, JK_LOG_DEBUG,
                "[%s] is a servlet url - should redirect to %s",
                uri, worker);

        /* get URI we should forward */
        if (uri_select_option == URI_SELECT_OPT_UNPARSED) {
            /* get original unparsed URI */
            forwardURI = uri_undec;
        }
        else if (uri_select_option == URI_SELECT_OPT_ESCAPED) {
            size_t elen  = strlen(uri) * 3 + 1;
            char *escuri = jk_pool_alloc(&pool, elen);
            if (!escape_url(uri, escuri, (int)elen)) {
                jk_log(logger, JK_LOG_ERROR,
                       "[%s] re-encoding request exceeds maximum buffer size.",
                       uri);
                write_error_response(pfc, 400);
                rv = SF_STATUS_REQ_FINISHED;
                goto cleanup;
            }
            if (JK_IS_DEBUG_LEVEL(logger))
                jk_log(logger, JK_LOG_DEBUG,
                       "fowarding escaped URI [%s]",
                       escuri);
            forwardURI = escuri;
        }
        else if (uri_select_option == URI_SELECT_OPT_PROXY) {
            size_t elen  = strlen(uri) * 3 + 1;
            char *escuri = jk_pool_alloc(&pool, elen);
            if (!jk_canonenc(uri, escuri, (int)elen)) {
                jk_log(logger, JK_LOG_ERROR,
                       "[%s] re-encoding request exceeds maximum buffer size.",
                       uri);
                write_error_response(pfc, 400);
                rv = SF_STATUS_REQ_FINISHED;
                goto cleanup;
            }
            if (JK_IS_DEBUG_LEVEL(logger))
                jk_log(logger, JK_LOG_DEBUG,
                       "fowarding escaped URI [%s]",
                       escuri);
            forwardURI = escuri;
        }
        else {
            forwardURI = uri;
        }
        /* Do a simple rewrite .
         * Note that URI can be escaped, so thus the rule has
         * to be in that case.
         *
         * TODO: Add more advanced regexp rewrite.
         */
        rewriteURI = simple_rewrite(&pool, forwardURI);
        if (rewriteURI == NULL)
            rewriteURI = rregex_rewrite(&pool, forwardURI);
        if (rewriteURI) {
            if (JK_IS_DEBUG_LEVEL(logger)) {
                jk_log(logger, JK_LOG_DEBUG,
                       "rewritten URI [%s]->[%s]",
                       forwardURI, rewriteURI);
            }
            forwardURI = rewriteURI;
        }
        itoa(worker_index, swindex, 10);
        rs = pfp->AddHeader(pfc, URI_HEADER_NAME, forwardURI);
        if (rs && query)
            rs = pfp->AddHeader(pfc, QUERY_HEADER_NAME, query);
        rs = rs && pfp->AddHeader(pfc, WORKER_HEADER_NAME, (LPSTR)worker);
        rs = rs && pfp->AddHeader(pfc, WORKER_HEADER_INDEX, swindex);
        rs = rs && pfp->SetHeader(pfc, "url", extension_uri);

        if (!rs) {
            jk_log(logger, JK_LOG_ERROR,
                   "error while adding request headers");
            SetLastError(ERROR_INVALID_PARAMETER);
            rv = SF_STATUS_REQ_ERROR;
            goto cleanup;
        }
        translate = get_pheader(&pool, pfp, pfc, TRANSLATE_HEADER,
                                szTB, sizeof(szTB));
        /* Move Translate: header to a temporary header so
         * that the extension proc will be called.
         * This allows the servlet to handle 'Translate: f'.
         */
        if (translate && &translate) {
            if (!pfp->AddHeader(pfc, TOMCAT_TRANSLATE_HEADER_NAME, translate)) {
                jk_log(logger, JK_LOG_ERROR,
                       "error while adding Tomcat-Translate headers");
                rv = SF_STATUS_REQ_ERROR;
                goto cleanup;
            }
            clear_header(pfp, pfc, TRANSLATE_HEADER);
        }
        ld = (isapi_log_data_t *)pfc->pFilterContext;
        if (ld == NULL) {
            ld = (isapi_log_data_t *)pfc->AllocMem(pfc, sizeof(isapi_log_data_t), 0);
            if (ld == NULL) {
                jk_log(logger, JK_LOG_ERROR,
                       "error while allocating memory");
                SetLastError(ERROR_NOT_ENOUGH_MEMORY);
                rv = SF_STATUS_REQ_ERROR;
                goto cleanup;
            }
            pfc->pFilterContext = ld;
        }
        memset(ld, 0, sizeof(isapi_log_data_t));
        StringCbCopy(ld->uri, sizeof(ld->uri), forwardURI);
        if (query)
            StringCbCopy(ld->query, sizeof(ld->query), query);
        ld->request_matched = JK_TRUE;
        if (JK_IS_DEBUG_LEVEL(logger)) {
            jk_log(logger, JK_LOG_DEBUG,
                   "forwarding to : %s", extension_uri);
            jk_log(logger, JK_LOG_DEBUG,
                   "forward URI   : %s%s", URI_HEADER_NAME, forwardURI);
            if (query)
                jk_log(logger, JK_LOG_DEBUG,
                       "forward query : %s%s", QUERY_HEADER_NAME, query);
            jk_log(logger, JK_LOG_DEBUG,
                   "forward worker: %s%s", WORKER_HEADER_NAME, worker);
            jk_log(logger, JK_LOG_DEBUG,
                   "worker index  : %s%s", WORKER_HEADER_INDEX, swindex);
        }
    }
    else {
        if (JK_IS_DEBUG_LEVEL(logger))
            jk_log(logger, JK_LOG_DEBUG, "[%s] is not a servlet url", uri_undec);
        if (strip_session) {
        	if (jk_strip_session_id(uri_undec, JK_PATH_SESSION_IDENTIFIER, logger)) {
        		pfp->SetHeader(pfc, "url", uri_undec);
        	}
        }
    }
cleanup:
    jk_close_pool(&pool);
    return rv;
}

DWORD WINAPI HttpFilterProc(PHTTP_FILTER_CONTEXT pfc,
                            DWORD dwNotificationType, LPVOID pvNotification)
{
    /* Initialise jk */
    EnterCriticalSection(&log_cs);
    if (is_inited && !is_mapread) {
        char serverName[MAX_SERVERNAME] = "";
        char instanceId[MAX_INSTANCEID] = "";
        DWORD dwLen = MAX_SERVERNAME - MAX_INSTANCEID - 1;

        if (pfc->GetServerVariable(pfc, "SERVER_NAME", serverName, &dwLen)) {
            dwLen = MAX_INSTANCEID;
            if (pfc->GetServerVariable(pfc, "INSTANCE_ID", instanceId, &dwLen)) {
                if (dwLen > 1) {
                    StringCbCat(serverName, MAX_SERVERNAME, "_");
                    StringCbCat(serverName, MAX_SERVERNAME, instanceId);
                }
            }
            if (!is_mapread) {
                WaitForSingleObject(init_cs, INFINITE);
                if (!is_mapread)
                    is_mapread = init_jk(serverName);
                ReleaseMutex(init_cs);
            }
        }
        /* If we can't read the map we become dormant */
        if (!is_mapread)
            is_inited = JK_FALSE;
    }
    LeaveCriticalSection(&log_cs);
    if (!is_inited) {
        /* In case the initialization failed
         * return error. This will make entire IIS
         * unusable like with Apache servers
         */
         SetLastError(ERROR_INVALID_FUNCTION);
         return SF_STATUS_REQ_ERROR;
    }
    if (iis_info.filter_notify_event == dwNotificationType) {
        return handle_notify_event(pfc, pvNotification);
    }
    else if (dwNotificationType == SF_NOTIFY_LOG) {
        if (pfc->pFilterContext) {
            isapi_log_data_t *ld = (isapi_log_data_t *)pfc->pFilterContext;
            if (ld->request_matched) {
                HTTP_FILTER_LOG  *pl = (HTTP_FILTER_LOG *)pvNotification;
                pl->pszTarget = ld->uri;
                pl->pszParameters = ld->query;
            }
        }
    }
    return SF_STATUS_REQ_NEXT_NOTIFICATION;
}


BOOL WINAPI GetExtensionVersion(HSE_VERSION_INFO * pVer)
{
    BOOL rv = TRUE;

    pVer->dwExtensionVersion = MAKELONG(HSE_VERSION_MINOR, HSE_VERSION_MAJOR);

    StringCbCopy(pVer->lpszExtensionDesc, HSE_MAX_EXT_DLL_NAME_LEN, (VERSION_STRING));


    WaitForSingleObject(init_cs, INFINITE);
    if (!is_inited) {
        rv = initialize_extension();
    }
    ReleaseMutex(init_cs);

    return rv;
}

DWORD WINAPI HttpExtensionProc(LPEXTENSION_CONTROL_BLOCK lpEcb)
{
    DWORD rc = HSE_STATUS_ERROR;
    isapi_private_data_t private_data;
    jk_ws_service_t s;
    jk_pool_atom_t buf[SMALL_POOL_SIZE];
    char *worker_name;

    lpEcb->dwHttpStatusCode = HTTP_STATUS_SERVER_ERROR;

    JK_TRACE_ENTER(logger);

    /* Initialise jk */
    EnterCriticalSection(&log_cs);
    if (is_inited && !is_mapread) {
        char serverName[MAX_SERVERNAME] = "";
        char instanceId[MAX_INSTANCEID] = "";
        DWORD dwLen = MAX_SERVERNAME - MAX_INSTANCEID - 1;

        if (lpEcb->GetServerVariable(lpEcb->ConnID, "SERVER_NAME",
                                     serverName, &dwLen)) {
            dwLen = MAX_INSTANCEID;
            if (lpEcb->GetServerVariable(lpEcb->ConnID, "INSTANCE_ID",
                                         instanceId, &dwLen)) {
                if (dwLen > 1) {
                    StringCbCat(serverName, MAX_SERVERNAME, "_");
                    StringCbCat(serverName, MAX_SERVERNAME, instanceId);
                }
            }
            if (!is_mapread) {
                WaitForSingleObject(init_cs, INFINITE);
                if (!is_mapread)
                    is_mapread = init_jk(serverName);
                ReleaseMutex(init_cs);
            }
        }
        if (!is_mapread)
            is_inited = JK_FALSE;
    }
    LeaveCriticalSection(&log_cs);

    if (!is_inited) {
        jk_log(logger, JK_LOG_ERROR, "not initialized");
        JK_TRACE_EXIT(logger);
        return HSE_STATUS_ERROR;
    }

    if (!watchdog_interval)
        wc_maintain(logger);
    jk_init_ws_service(&s);
    jk_open_pool(&private_data.p, buf, sizeof(buf));

    private_data.bytes_read_so_far = 0;
    private_data.lpEcb = lpEcb;
    private_data.chunk_content = JK_FALSE;

    s.ws_private = &private_data;
    s.pool = &private_data.p;

    if (flush_packets) {
        lpEcb->ServerSupportFunction(lpEcb->ConnID, HSE_REQ_SET_FLUSH_FLAG,
                (LPVOID) TRUE, NULL, NULL);
    }

    if (init_ws_service(&private_data, &s, &worker_name)) {
        jk_endpoint_t *e = NULL;
        jk_worker_t *worker = wc_get_worker_for_name(worker_name, logger);

        if (!worker) {
            jk_log(logger, JK_LOG_ERROR,
                   "could not get a worker for name %s",
                   worker_name);
            jk_close_pool(&private_data.p);
            JK_TRACE_EXIT(logger);
            return rc;
        }
        if (JK_IS_DEBUG_LEVEL(logger))
            jk_log(logger, JK_LOG_DEBUG,
                   "got a worker for name %s", worker_name);
        if (worker->get_endpoint(worker, &e, logger)) {
            int is_error = JK_HTTP_SERVER_ERROR;
            int result;
            if ((result = e->service(e, &s, logger, &is_error)) > 0) {
                if (s.extension.use_server_error_pages &&
                    s.http_response_status >= s.extension.use_server_error_pages) {
                    if (JK_IS_DEBUG_LEVEL(logger))
                        jk_log(logger, JK_LOG_DEBUG, "Forwarding status=%d"
                               " for worker=%s",
                               s.http_response_status, worker_name);
                    lpEcb->dwHttpStatusCode = s.http_response_status;
                    write_error_message(lpEcb, s.http_response_status,
                                        private_data.err_hdrs);
                }
                else {
                    rc = HSE_STATUS_SUCCESS;
                    lpEcb->dwHttpStatusCode = s.http_response_status;
                    if (JK_IS_DEBUG_LEVEL(logger))
                        jk_log(logger, JK_LOG_DEBUG,
                               "service() returned OK");
                }
            }
            else {
                if ((result == JK_CLIENT_ERROR) && (is_error == JK_HTTP_OK)) {
                    jk_log(logger, JK_LOG_INFO,
                           "service() failed because client aborted connection");
                }
                else {
                    jk_log(logger, JK_LOG_ERROR,
                           "service() failed with http error %d", is_error);
                }
                lpEcb->dwHttpStatusCode = is_error;
                write_error_message(lpEcb, is_error, private_data.err_hdrs);
            }
            e->done(&e, logger);
        }
        else {
            int is_error = JK_HTTP_SERVER_BUSY;
            jk_log(logger, JK_LOG_ERROR,
                "Failed to obtain an endpoint to service request - "
                "your connection_pool_size is probably less than the threads in your web server!");
            lpEcb->dwHttpStatusCode = is_error;
            write_error_message(lpEcb, is_error, private_data.err_hdrs);
        }
    }
    else {
        jk_log(logger, JK_LOG_ERROR,
            "failed to init service for request.");
    }
    jk_close_pool(&private_data.p);
    JK_TRACE_EXIT(logger);

    return rc;
}

BOOL WINAPI TerminateExtension(DWORD dwFlags)
{
    return TerminateFilter(dwFlags);
}

BOOL WINAPI TerminateFilter(DWORD dwFlags)
{
    UNREFERENCED_PARAMETER(dwFlags);

    if (dll_process_detach) {
        /* Simple case */
        if (!is_inited)
            return TRUE;
        watchdog_interval = 0;
        if (watchdog_handle) {
            WaitForSingleObject(watchdog_handle, INFINITE);
            CloseHandle(watchdog_handle);
            watchdog_handle = NULL;
        }
        jk_shm_close(logger);
        if (logger)
            jk_close_file_logger(&logger);
        return TRUE;
    }
    EnterCriticalSection(&log_cs);
    if (!is_inited) {
        LeaveCriticalSection(&log_cs);
        return TRUE;
    }
    WaitForSingleObject(init_cs, INFINITE);
    if (is_inited) {
        is_inited = JK_FALSE;
        jk_log(logger, JK_LOG_INFO, "%s stopping", (FULL_VERSION_STRING));
        watchdog_interval = 0;
        if (watchdog_handle) {
            WaitForSingleObject(watchdog_handle, INFINITE);
            CloseHandle(watchdog_handle);
            watchdog_handle = NULL;
        }
        wc_close(logger);
        if (is_mapread) {
            uri_worker_map_free(&uw_map, logger);
            is_mapread = JK_FALSE;
        }
        if (workers_map) {
            jk_map_free(&workers_map);
        }
        if (rewrite_map) {
            jk_map_free(&rewrite_map);
        }
        if (rregexp_map) {
            int i;
            for (i = 0; i < jk_map_size(rregexp_map); i++) {
                ap_regex_t *regexp = (ap_regex_t *)jk_map_value_at(rregexp_map, i);
                if (regexp) {
                    ap_regfree(regexp);
                    free(regexp);
                }
            }
            jk_map_free(&rregexp_map);
        }
        jk_shm_close(logger);
        if (logger) {
            jk_close_file_logger(&logger);
        }
    }
    ReleaseMutex(init_cs);
    LeaveCriticalSection(&log_cs);
    return TRUE;
}


BOOL WINAPI DllMain(HINSTANCE hInst,    /* Instance Handle of the DLL           */
                    ULONG ulReason,     /* Reason why NT called this DLL        */
                    LPVOID lpReserved)  /* Reserved parameter for future use    */
{
    char fname[MAX_PATH];
    UNREFERENCED_PARAMETER(lpReserved);

    switch (ulReason) {
    case DLL_PROCESS_ATTACH:
        if (GetModuleFileName(hInst, fname, sizeof(fname) - 12)) {
            char *p = strrchr(fname, '.');
            if (p) {
                *p = '\0';
                StringCbCopy(ini_file_name, MAX_PATH, fname);
                StringCbCat(ini_file_name, MAX_PATH, ".properties");
                StringCbCopy(ini_mutex_name, MAX_PATH, "Local\\JK_");
                StringCbCat(ini_mutex_name, MAX_PATH, fname);
                StringCbCat(ini_mutex_name, MAX_PATH, "_mutex");
                p = &ini_mutex_name[10];
                while (*p) {
                    if (!isalnum((unsigned char)*p))
                        *p = '_';
                    else
                        *p = toupper(*p);
                    p++;
                }
            }
            else {
                /* Cannot obtain file name ? */
                return FALSE;
            }
            if ((p = strrchr(fname, '\\'))) {
                *p++ = '\0';
                StringCbCopy(dll_file_path, MAX_PATH, fname);
                jk_map_alloc(&jk_environment_map);
                jk_map_add(jk_environment_map, "JKISAPI_PATH", dll_file_path);
                jk_map_add(jk_environment_map, "JKISAPI_NAME", p);
            }
            else {
                /* Cannot obtain file name ? */
                return JK_FALSE;
            }
        }
        else {
            return JK_FALSE;
        }
        /* Construct redirector headers to use for this redirector instance */
        StringCbPrintf(URI_HEADER_NAME, RES_BUFFER_SIZE, HEADER_TEMPLATE, URI_HEADER_NAME_BASE, hInst);
        StringCbPrintf(QUERY_HEADER_NAME, RES_BUFFER_SIZE, HEADER_TEMPLATE, QUERY_HEADER_NAME_BASE, hInst);
        StringCbPrintf(WORKER_HEADER_NAME, RES_BUFFER_SIZE, HEADER_TEMPLATE, WORKER_HEADER_NAME_BASE, hInst);
        StringCbPrintf(WORKER_HEADER_INDEX, RES_BUFFER_SIZE, HEADER_TEMPLATE, WORKER_HEADER_INDEX_BASE, hInst);
        StringCbPrintf(TOMCAT_TRANSLATE_HEADER_NAME, RES_BUFFER_SIZE, HEADER_TEMPLATE, TOMCAT_TRANSLATE_HEADER_NAME_BASE, hInst);

        /* Construct the HTTP_ headers that will be seen in ExtensionProc */
        StringCbPrintf(HTTP_URI_HEADER_NAME, RES_BUFFER_SIZE, HTTP_HEADER_TEMPLATE, URI_HEADER_NAME_BASE, hInst);
        StringCbPrintf(HTTP_QUERY_HEADER_NAME, RES_BUFFER_SIZE, HTTP_HEADER_TEMPLATE, QUERY_HEADER_NAME_BASE, hInst);
        StringCbPrintf(HTTP_WORKER_HEADER_NAME, RES_BUFFER_SIZE, HTTP_HEADER_TEMPLATE, WORKER_HEADER_NAME_BASE, hInst);
        StringCbPrintf(HTTP_WORKER_HEADER_INDEX, RES_BUFFER_SIZE, HTTP_HEADER_TEMPLATE, WORKER_HEADER_INDEX_BASE, hInst);

        InitializeCriticalSection(&log_cs);
        init_cs = CreateMutex(jk_get_sa_with_null_dacl(), FALSE, ini_mutex_name);
        if (init_cs == NULL && GetLastError() == ERROR_ALREADY_EXISTS)
            init_cs = OpenMutex(MUTEX_ALL_ACCESS, FALSE, ini_mutex_name);
        if (init_cs == NULL)
            return JK_FALSE;
    break;
    case DLL_PROCESS_DETACH:
        dll_process_detach = TRUE;
        __try {
            TerminateFilter(HSE_TERM_MUST_UNLOAD);
        }
        __except(1) {
        }
        if (init_cs != NULL)
            CloseHandle(init_cs);
        DeleteCriticalSection(&log_cs);
        break;

    default:
        break;
    }

    return TRUE;
}

static DWORD WINAPI watchdog_thread(void *param)
{
    int i;
    if (JK_IS_DEBUG_LEVEL(logger)) {
        jk_log(logger, JK_LOG_DEBUG,
               "Watchdog thread initialized with %u second interval",
               watchdog_interval);
    }
    while (watchdog_interval) {
        for (i = 0; i < (watchdog_interval * 10); i++) {
            if (!watchdog_interval)
                break;
            Sleep(100);
        }
        if (!watchdog_interval)
            break;
        if (JK_IS_DEBUG_LEVEL(logger)) {
            jk_log(logger, JK_LOG_DEBUG,
                   "Watchdog thread running");
        }
        if (worker_mount_file[0]) {
            jk_shm_lock();
            uri_worker_map_update(uw_map, 0, logger);
            jk_shm_unlock();
        }
        wc_maintain(logger);
    }
    if (JK_IS_DEBUG_LEVEL(logger)) {
        jk_log(logger, JK_LOG_DEBUG,
               "Watchdog thread finished");
    }
    return 0;
}

/*
 * Reinitializes the logger, formatting the log file name if rotation is enabled,
 * and calculating the next rotation time if applicable.
 */
static int init_logger(int rotate)
{
    int rc = JK_TRUE;
    int log_open = rotate;  /* log is assumed open if a rotate is requested */
    char *log_file_name;
    char log_file_name_buf[MAX_PATH*2];
    jk_logger_t *org = NULL;

    /* If log rotation is enabled, format the log filename */
    if ((log_rotationtime > 0) || (log_filesize > 0)) {
        time_t t;
        t = time(NULL);

        if (log_rotationtime > 0) {
            /* Align time to rotationtime intervals */
            t = (t / log_rotationtime) * log_rotationtime;

            /* Calculate rotate time */
            log_next_rotate_time = t + log_rotationtime;
        }

        log_file_name = log_file_name_buf;
        if (strchr(log_file, '%')) {
            struct tm *tm_now;

            /* If there are % format patterns in the log file name,
             * treat it as a strftime format */
            tm_now = localtime(&t);
            strftime(log_file_name, sizeof(log_file_name_buf), log_file, tm_now);
        }
        else {
            /* Otherwise append the number of seconds to the base name */
            StringCbPrintf(log_file_name, sizeof(log_file_name_buf), "%s.%d", log_file, (long)t);
        }
    }
    else {
        log_file_name = log_file;
    }

    /* Close the current log file if required, and the effective log file name has changed */
    if (log_open && strncmp(log_file_name, log_file_effective, strlen(log_file_name)) != 0) {
        FILE* lf = ((jk_file_logger_t* )logger->logger_private)->logfile;
        fprintf(lf, "Log rotated to %s\n", log_file_name);
        fflush(lf);
        org = logger;
        log_open = JK_FALSE;
    }

    if (!log_open) {
        if (jk_open_file_logger(&logger, log_file_name, log_level)) {
            logger->log = iis_log_to_file;

            /* Remember the current log file name for the next potential rotate */
            StringCbCopy(log_file_effective, sizeof(log_file_effective), log_file_name);
            rc = JK_TRUE;
        }
        else {
            logger = org;
            org = NULL;
            rc = JK_FALSE;
        }
    }
    if (org)
        jk_close_file_logger(&org);
    return rc;
}

/*
 * Checks whether the log needs to be rotated. Must be called while holding the log lock.
 * The behaviour here is based on the Apache rotatelogs program.
 * http://httpd.apache.org/docs/2.0/programs/rotatelogs.html
 */
static int JK_METHOD rotate_log_file(void)
{
    int rc = JK_TRUE;
    int rotate = JK_FALSE;

    if (log_rotationtime > 0) {
        time_t t = time(NULL);

        if (t >= log_next_rotate_time) {
            rotate = JK_TRUE;
        }
    }
    else if (log_filesize > 0 && logger) {
        LARGE_INTEGER filesize;
        HANDLE h = (HANDLE)_get_osfhandle(fileno(((jk_file_logger_t *)(logger)->logger_private)->logfile));
        GetFileSizeEx(h, &filesize);

        if ((ULONGLONG)filesize.QuadPart >= log_filesize) {
            rotate = JK_TRUE;
        }
    }
    if (rotate) {
        rc = init_logger(JK_TRUE);
    }
    return rc;
}

/*
 * Log messages to the log file, rotating the log when required.
 */
static int JK_METHOD iis_log_to_file(jk_logger_t *l, int level,
                                    int used, char *what)
{
    int rc = JK_FALSE;

    if (l &&
        (l->level <= level || level == JK_LOG_REQUEST_LEVEL) &&
        l->logger_private && what && used > 0) {
        jk_file_logger_t *p = l->logger_private;
        rc = JK_TRUE;

        if (p->logfile) {
#if 0
            what[used++] = '\r';
#endif
            what[used++] = '\n';
            what[used] = '\0';

            /* Perform logging within critical section to protect rotation */
            EnterCriticalSection(&log_cs);
            if (rotate_log_file() && logger) {
                /* The rotation process will reallocate the jk_logger_t structure, so refetch */
                FILE *rotated = ((jk_file_logger_t *)logger->logger_private)->logfile;
                fputs(what, rotated);
                fflush(rotated);
            }
            LeaveCriticalSection(&log_cs);
        }
    }
    return rc;
}

static int init_jk(char *serverName)
{
    char *p, shm_name[MAX_PATH];
    int i, rc = JK_FALSE;

    init_logger(JK_FALSE);
    /* TODO: Use System logging to notify the user that
     *       we cannot open the configured log file.
     */
    StringCbCopy(shm_name, MAX_PATH, SHM_DEF_PREFIX);
    jk_log(logger, JK_LOG_INFO, "Starting " FULL_VERSION_STRING);
    StringCbCat(shm_name, MAX_PATH, serverName);
    StringCbCat(shm_name, MAX_PATH, "_");
    StringCbCat(shm_name, MAX_PATH, extension_uri + 1);
    if ((p = strrchr(shm_name, '.')))
        *p = '\0';
    jk_set_worker_def_cache_size(DEFAULT_WORKER_THREADS);

    /* Logging the initialization type: registry or properties file in virtual dir
     */
    if (JK_IS_DEBUG_LEVEL(logger)) {
        jk_log(logger, JK_LOG_DEBUG, "Detected IIS version %d.%d", iis_info.major, iis_info.minor);
        if (using_ini_file) {
            jk_log(logger, JK_LOG_DEBUG, "Using ini file %s.", ini_file_name);
        }
        else {
            jk_log(logger, JK_LOG_DEBUG, "Using registry.");
        }

        jk_log(logger, JK_LOG_DEBUG, "Using log file %s.", log_file);
        jk_log(logger, JK_LOG_DEBUG, "Using log level %d.", log_level);
        jk_log(logger, JK_LOG_DEBUG, "Using log rotation time %d seconds.", log_rotationtime);
        jk_log(logger, JK_LOG_DEBUG, "Using log file size %d bytes.", log_filesize);

        jk_log(logger, JK_LOG_DEBUG, "Using extension uri %s.", extension_uri);
        jk_log(logger, JK_LOG_DEBUG, "Using worker file %s.", worker_file);
        jk_log(logger, JK_LOG_DEBUG, "Using worker mount file %s.",
               worker_mount_file);
        jk_log(logger, JK_LOG_DEBUG, "Using rewrite rule file %s.",
               rewrite_rule_file);
        jk_log(logger, JK_LOG_DEBUG, "Using uri select %d.", uri_select_option);
        jk_log(logger, JK_LOG_DEBUG, "Using%s chunked encoding.", (chunked_encoding_enabled ? "" : " no"));

        jk_log(logger, JK_LOG_DEBUG, "Using notification event %s (0x%08x)",
               (iis_info.filter_notify_event == SF_NOTIFY_AUTH_COMPLETE) ?
               "SF_NOTIFY_AUTH_COMPLETE" :
               ((iis_info.filter_notify_event == SF_NOTIFY_PREPROC_HEADERS) ?
               "SF_NOTIFY_PREPROC_HEADERS" : "UNKNOWN"),
               iis_info.filter_notify_event);

        if (error_page) {
            jk_log(logger, JK_LOG_DEBUG, "Using error page '%s'.", error_page);
        }
        jk_log(logger, JK_LOG_DEBUG, "Using uri header %s.", URI_HEADER_NAME);
        jk_log(logger, JK_LOG_DEBUG, "Using query header %s.", QUERY_HEADER_NAME);
        jk_log(logger, JK_LOG_DEBUG, "Using worker header %s.", WORKER_HEADER_NAME);
        jk_log(logger, JK_LOG_DEBUG, "Using worker index %s.", WORKER_HEADER_INDEX);
        jk_log(logger, JK_LOG_DEBUG, "Using translate header %s.", TOMCAT_TRANSLATE_HEADER_NAME);
        jk_log(logger, JK_LOG_DEBUG, "Using a default of %d connections per pool.",
                                     DEFAULT_WORKER_THREADS);
    }

    if ((log_rotationtime > 0) && (log_filesize > 0)) {
        jk_log(logger, JK_LOG_WARNING,
            "%s is defined in configuration, but will be ignored because %s is set. ",
            LOG_FILESIZE_TAG, LOG_ROTATION_TIME_TAG);
    }

    if (rewrite_rule_file[0] && jk_map_alloc(&rewrite_map)) {
        if (jk_map_read_properties(rewrite_map, NULL, rewrite_rule_file,
                                   NULL, JK_MAP_HANDLE_RAW, logger)) {
            if (JK_IS_DEBUG_LEVEL(logger)) {
                jk_log(logger, JK_LOG_DEBUG, "Loaded rewrite rule file %s.",
                       rewrite_rule_file);

            }
            jk_map_alloc(&rregexp_map);
            for (i = 0; i < jk_map_size(rewrite_map); i++) {
                const char *src = jk_map_name_at(rewrite_map, i);
                if (*src == '~') {
                    ap_regex_t *regexp = malloc(sizeof(ap_regex_t));
                    const char *val = jk_map_value_at(rewrite_map, i);
                    /* Skip leading tilde */
                    src++;
                    regexp->fake = val;
                    if (ap_regcomp(regexp, src) == JK_TRUE) {
                        jk_map_add(rregexp_map, src, regexp);
                        if (JK_IS_DEBUG_LEVEL(logger)) {
                            jk_log(logger, JK_LOG_DEBUG,
                                   "Added regular expression rule %s -> %s",
                                   src, regexp->fake);
                        }
                    }
                    else {
                        jk_log(logger, JK_LOG_ERROR,
                               "Unable to compile regular expression %s",
                               src);
                        free(regexp);
                    }
                }
            }
        }
        else {
            jk_map_free(&rewrite_map);
            rewrite_map = NULL;
        }
    }

    if (uri_worker_map_alloc(&uw_map, NULL, logger)) {
        rc = JK_FALSE;
        if (reject_unsafe)
            uw_map->reject_unsafe = 1;
        else
            uw_map->reject_unsafe = 0;
        uw_map->reload = worker_mount_reload;
        if (worker_mount_file[0]) {
            uw_map->fname = worker_mount_file;
            rc = uri_worker_map_load(uw_map, logger);
        }
    }
    if (rc) {
        rc = JK_FALSE;
        if (jk_map_alloc(&workers_map)) {
            if (jk_map_read_properties(workers_map, NULL, worker_file, NULL,
                                       JK_MAP_HANDLE_DUPLICATES, logger)) {
                int rv = -1;

                /* we add the URI->WORKER MAP since workers using AJP14 will feed it */

                if (jk_map_resolve_references(workers_map, "worker.", 1, 1, logger) == JK_FALSE) {
                    jk_log(logger, JK_LOG_ERROR, "Error in resolving configuration references");
                }
                /*
                 * Create named shared memory for each server
                 */
                if (shm_config_size == -1) {
                    shm_config_size = jk_shm_calculate_size(workers_map, logger);
                }
                else if (shm_config_size > 0) {
                    jk_log(logger, JK_LOG_WARNING,
                           "The optimal shared memory size can now be determined automatically.");
                    jk_log(logger, JK_LOG_WARNING,
                           "You can remove the shm_size attribute if you want to use the optimal size.");
                }
                if ((shm_config_size > 0) &&
                    (rv = jk_shm_open(shm_name, shm_config_size, logger)) != 0) {
                    jk_log(logger, JK_LOG_ERROR,
                           "Initializing shm:%s errno=%d. Load balancing workers will not function properly",
                           jk_shm_name(), rv);
                }
                if (rv != 0 && (rv = jk_shm_open(NULL, shm_config_size, logger)) != 0) {
                    /* Do not try to open the worker if we cannot create
                     * the shared memory segment or heap memory.
                     */
                    jk_log(logger, JK_LOG_EMERG,
                           "Initializing shm:%s errno=%d. Cannot continue",
                           jk_shm_name(), rv);
                    jk_map_free(&workers_map);
                    workers_map = NULL;
                    return JK_FALSE;
                }
                else {
                    if (shm_loaded_name[0]) {
                        if (strcmp(shm_loaded_name, shm_name)) {
                            jk_log(logger, JK_LOG_WARNING,
                                   "Loading different shared memory %s. Already loaded %s",
                                   shm_name, shm_loaded_name);
                        }
                    }
                    else {
                        StringCbCopy(shm_loaded_name, MAX_PATH, shm_name);
                    }
                }
                worker_env.uri_to_worker = uw_map;
                worker_env.server_name = serverName;
                worker_env.pool = NULL;

                if (wc_open(workers_map, &worker_env, logger)) {
                    rc = JK_TRUE;
                    uri_worker_map_ext(uw_map, logger);
                    uri_worker_map_switch(uw_map, logger);
                }
                else {
                    jk_shm_close(logger);
                }
            }
            else {
                jk_log(logger, JK_LOG_EMERG,
                       "Unable to read worker file %s. (errno=%d, err=%s)", worker_file, errno, strerror(errno));
            }
            if (rc != JK_TRUE) {
                jk_map_free(&workers_map);
                workers_map = NULL;
            }
        }
    }
    if (rc) {
        if (watchdog_interval) {
            DWORD  wi;
            watchdog_handle = CreateThread(NULL, 0, watchdog_thread,
                                           NULL, 0, &wi);
            if (!watchdog_handle) {
                jk_log(logger, JK_LOG_EMERG, "Error %d (0x%08x) creating Watchdog thread",
                       GetLastError(), GetLastError());
                watchdog_interval = 0;
            }
        }
        jk_log(logger, JK_LOG_INFO, "%s initialized", (FULL_VERSION_STRING));
    }
    return rc;
}

static BOOL initialize_extension(void)
{

    if (read_registry_init_data()) {
        if (get_iis_info(&iis_info) != JK_TRUE) {
            jk_log(logger, JK_LOG_ERROR, "Could not retrieve IIS version from registry");
        }
        else {
            if (use_auth_notification_flags)
                iis_info.filter_notify_event = SF_NOTIFY_AUTH_COMPLETE;
            else
                iis_info.filter_notify_event = SF_NOTIFY_PREPROC_HEADERS;
        }
        is_inited = JK_TRUE;
    }
    return is_inited;
}

int parse_uri_select(const char *uri_select)
{
    if (!strcasecmp(uri_select, URI_SELECT_PARSED_VERB))
        return URI_SELECT_OPT_PARSED;
    if (!strcasecmp(uri_select, URI_SELECT_UNPARSED_VERB))
        return URI_SELECT_OPT_UNPARSED;
    if (!strcasecmp(uri_select, URI_SELECT_ESCAPED_VERB))
        return URI_SELECT_OPT_ESCAPED;
    if (!strcasecmp(uri_select, URI_SELECT_PROXY_VERB))
        return URI_SELECT_OPT_PROXY;
    return -1;
}

static int read_registry_init_data(void)
{
    char tmpbuf[MAX_PATH];
    int ok = JK_FALSE;
    LPVOID src;
    HKEY hkey;
    int remain;
    jk_map_t *map = NULL;

    remain = jk_check_buffer_size();
    if (remain < 0) {
        jk_log(logger, JK_LOG_ERROR,
               "mod_jk: JK_MAX_ATTRIBUTE_NAME_LEN in jk_util.c is too small, "
               "increase by %d", -1 * remain);
    }
    if (jk_map_alloc(&map)) {
        if (jk_map_read_properties(map, jk_environment_map, ini_file_name, NULL,
                                   JK_MAP_HANDLE_DUPLICATES, logger)) {
            using_ini_file = JK_TRUE;
            src = map;
        }
        else {
            jk_map_free(&map);
        }
    }
    if (!using_ini_file) {
        long rc = RegOpenKeyEx(HKEY_LOCAL_MACHINE, REGISTRY_LOCATION,
                               (DWORD)0, KEY_READ, &hkey);
        if (ERROR_SUCCESS != rc) {
            return JK_FALSE;
        }
        else {
            src = hkey;
        }
    }
    if (!get_config_parameter(src, JK_LOG_FILE_TAG, log_file, sizeof(log_file)))
        goto cleanup;
    if (is_path_relative(log_file)) {
        char *fp = path_merge(dll_file_path, log_file);
        if (fp) {
            StringCbCopy(log_file, sizeof(log_file), fp);
            free(fp);
        }
    }
    if (get_config_parameter(src, JK_LOG_LEVEL_TAG, tmpbuf, sizeof(tmpbuf))) {
        log_level = jk_parse_log_level(tmpbuf);
    }
    if (get_config_parameter(src, LOG_ROTATION_TIME_TAG, tmpbuf, sizeof(tmpbuf))) {
        log_rotationtime = atol(tmpbuf);
        if (log_rotationtime < 0) {
            log_rotationtime = 0;
        }
    }
    if (get_config_parameter(src, LOG_FILESIZE_TAG, tmpbuf, sizeof(tmpbuf))) {
        int tl = lstrlenA(tmpbuf);
        if (tl > 0) {
            /* rotatelogs has an 'M' suffix on filesize, which we optionally support for consistency */
            if (tmpbuf[tl - 1] == 'M') {
                tmpbuf[tl - 1] = '\0';
            }
            log_filesize = atol(tmpbuf);
            if (log_filesize < 0) {
                log_filesize = 0;
            }
            /* Convert to MB as per Apache rotatelogs */
            log_filesize *= (1000 * 1000);
        }
    }

    if (!get_config_parameter(src, EXTENSION_URI_TAG, extension_uri, sizeof(extension_uri)))
        goto cleanup;
    if (!get_config_parameter(src, JK_WORKER_FILE_TAG, worker_file, sizeof(worker_file)))
        goto cleanup;
    if (is_path_relative(worker_file)) {
        char *fp = path_merge(dll_file_path, worker_file);
        if (fp) {
            StringCbCopy(worker_file, sizeof(worker_file), fp);
            free(fp);
        }
    }
    if (!get_config_parameter(src, JK_MOUNT_FILE_TAG, worker_mount_file, sizeof(worker_mount_file)))
        goto cleanup;
    if (is_path_relative(worker_mount_file)) {
        char *fp = path_merge(dll_file_path, worker_mount_file);
        if (fp) {
            StringCbCopy(worker_mount_file, sizeof(worker_mount_file), fp);
            free(fp);
        }
    }
    if (get_config_parameter(src, URI_REWRITE_TAG, rewrite_rule_file, sizeof(rewrite_rule_file))) {
        if (is_path_relative(rewrite_rule_file)) {
            char *fp = path_merge(dll_file_path, rewrite_rule_file);
            if (fp) {
                StringCbCopy(rewrite_rule_file, sizeof(rewrite_rule_file), fp);
                free(fp);
            }
        }
    }
    if (get_config_parameter(src, URI_SELECT_TAG, tmpbuf, sizeof(tmpbuf))) {
        int opt = parse_uri_select(tmpbuf);
        if (opt >= 0) {
            uri_select_option = opt;
        }
        else {
            jk_log(logger, JK_LOG_ERROR, "Invalid value '%s' for configuration item '"
                   URI_SELECT_TAG "'", tmpbuf);
        }
    }
    if (get_config_parameter(src, COLLAPSE_SLASHES_TAG, tmpbuf, sizeof(tmpbuf))) {
        jk_log(logger, JK_LOG_ERROR, "Configuration item '" COLLAPSE_SLASHES_TAG
                "' is deprecated and will be ignored");
    }
    shm_config_size = get_config_int(src, SHM_SIZE_TAG, -1);
    worker_mount_reload = get_config_int(src, WORKER_MOUNT_RELOAD_TAG, JK_URIMAP_DEF_RELOAD);
    strip_session = get_config_bool(src, STRIP_SESSION_TAG, JK_FALSE);
    use_auth_notification_flags = get_config_int(src, AUTH_COMPLETE_TAG, 1);
    reject_unsafe = get_config_bool(src, REJECT_UNSAFE_TAG, JK_FALSE);
    watchdog_interval = get_config_int(src, WATCHDOG_INTERVAL_TAG, 0);
    if (watchdog_interval < 0)
        watchdog_interval = 0;
    chunked_encoding_enabled = get_config_bool(src, ENABLE_CHUNKED_ENCODING_TAG, JK_FALSE);
    flush_packets = get_config_bool(src, FLUSH_PACKETS_TAG, JK_FALSE);
    if (get_config_parameter(src, ERROR_PAGE_TAG, error_page_buf, sizeof(error_page_buf))) {
        error_page = error_page_buf;
    }
    ok = JK_TRUE;
cleanup:
    if (using_ini_file) {
        jk_map_free(&map);
    }
    else {
        RegCloseKey(hkey);
    }

    return ok;
}

static int get_config_parameter(LPVOID src, const char *tag,
                                char *val, size_t sz)
{
    const char *tmp = NULL;
    if (using_ini_file) {
        tmp = jk_map_get_string((jk_map_t*)src, tag, NULL);
        if (tmp && (strlen(tmp) < sz)) {
            StringCbCopy(val, sz, tmp);
            return JK_TRUE;
        }
        else {
            return JK_FALSE;
        }
    }
    else {
        return get_registry_config_parameter((HKEY)src, tag, val, sz);
    }
}

static int get_config_int(LPVOID src, const char *tag, int def)
{
    if (using_ini_file) {
        return jk_map_get_int((jk_map_t*)src, tag, def);
    }
    else {
        int val;
        if (get_registry_config_number(src, tag, &val) ) {
            return val;
        }
        else {
            return def;
        }
    }
}

static int get_config_bool(LPVOID src, const char *tag, int def)
{
    if (using_ini_file) {
        return jk_map_get_bool((jk_map_t*)src, tag, def);
    }
    else {
        char tmpbuf[128];
        if (get_registry_config_parameter(src, tag,
                                          tmpbuf, sizeof(tmpbuf))) {
            return jk_get_bool_code(tmpbuf, def);
        }
        else {
            return def;
        }
    }
}

static int get_registry_config_parameter(HKEY hkey,
                                         const char *tag, char *b, size_t sz)
{
    DWORD type = 0;
    LONG  lrc;
    DWORD ssz = (DWORD)sz -1;

    b[ssz] = '\0'; /* Null terminate in case RegQueryValueEx doesn't */
    lrc = RegQueryValueEx(hkey, tag, NULL, &type, (LPBYTE)b, &ssz);
    if ((ERROR_SUCCESS != lrc) || (type != REG_SZ)) {
        return JK_FALSE;
    }

    return JK_TRUE;
}

static int get_registry_config_number(HKEY hkey,
                                      const char *tag, int *val)
{
    DWORD type = 0;
    DWORD data = 0;
    DWORD sz   = sizeof(DWORD);
    LONG lrc;

    lrc = RegQueryValueEx(hkey, tag, NULL, &type, (LPBYTE)&data, &sz);
    if ((ERROR_SUCCESS != lrc) || (type != REG_DWORD)) {
        return JK_FALSE;
    }

    *val = (int)data;

    return JK_TRUE;
}

static int init_ws_service(isapi_private_data_t * private_data,
                           jk_ws_service_t *s, char **worker_name)
{
    char *all_headers;
    int worker_index = -1;
    rule_extension_t *e;
    char  temp_buf[64];
    BOOL  unknown_content_length = FALSE;
    unsigned int cnt = 0;
    char *tmp;


    JK_TRACE_ENTER(logger);

    s->start_response = start_response;
    s->read  = iis_read;
    s->write = iis_write;
    s->done  = iis_done;

    GET_SERVER_VARIABLE_VALUE(HTTP_URI_HEADER_NAME, s->req_uri, NULL);

    if (s->req_uri == NULL) {
        if (JK_IS_DEBUG_LEVEL(logger))
            jk_log(logger, JK_LOG_DEBUG, "No URI header value provided. Defaulting to old behaviour" );
        s->query_string = private_data->lpEcb->lpszQueryString;
        *worker_name = DEFAULT_WORKER_NAME;
        GET_SERVER_VARIABLE_VALUE("URL", s->req_uri, "");
        if (unescape_url(s->req_uri) < 0) {
            JK_TRACE_EXIT(logger);
            return JK_FALSE;
        }
        jk_servlet_normalize(s->req_uri, logger);
    } else {
        GET_SERVER_VARIABLE_VALUE(HTTP_QUERY_HEADER_NAME, s->query_string, "");
        GET_SERVER_VARIABLE_VALUE(HTTP_WORKER_HEADER_NAME, (*worker_name), "");
        GET_SERVER_VARIABLE_VALUE_INT(HTTP_WORKER_HEADER_INDEX, worker_index, -1);
    }

    if (JK_IS_DEBUG_LEVEL(logger)) {
        jk_log(logger, JK_LOG_DEBUG, "Reading extension header %s: %s", HTTP_WORKER_HEADER_NAME, (*worker_name));
        jk_log(logger, JK_LOG_DEBUG, "Reading extension header %s: %d", HTTP_WORKER_HEADER_INDEX, worker_index);
        jk_log(logger, JK_LOG_DEBUG, "Reading extension header %s: %s", HTTP_URI_HEADER_NAME, s->req_uri);
        jk_log(logger, JK_LOG_DEBUG, "Reading extension header %s: %s", HTTP_QUERY_HEADER_NAME, s->query_string);
    }

    GET_SERVER_VARIABLE_VALUE("AUTH_TYPE", s->auth_type, NULL);
    GET_SERVER_VARIABLE_VALUE("REMOTE_USER", s->remote_user, NULL);
    GET_SERVER_VARIABLE_VALUE("SERVER_PROTOCOL", s->protocol, "");
    GET_SERVER_VARIABLE_VALUE("REMOTE_HOST", s->remote_host, "");
    GET_SERVER_VARIABLE_VALUE("REMOTE_ADDR", s->remote_addr, "");
    GET_SERVER_VARIABLE_VALUE("REMOTE_PORT", s->remote_port, "");
    GET_SERVER_VARIABLE_VALUE("SERVER_NAME", s->server_name, "");
    GET_SERVER_VARIABLE_VALUE("LOCAL_ADDR", s->local_addr, "");
    GET_SERVER_VARIABLE_VALUE_INT("SERVER_PORT", s->server_port, 80);
    GET_SERVER_VARIABLE_VALUE("SERVER_SOFTWARE", s->server_software, "");
    GET_SERVER_VARIABLE_VALUE_INT("SERVER_PORT_SECURE", s->is_ssl, 0);

    s->method = private_data->lpEcb->lpszMethod;
    /* Check for Transfer Encoding */
    if (get_server_value(private_data->lpEcb,
                         "HTTP_TRANSFER_ENCODING",
                         temp_buf,
                         sizeof(temp_buf))) {
        if (strcasecmp(temp_buf, TRANSFER_ENCODING_CHUNKED_VALUE) == 0) {
            s->is_chunked = JK_TRUE;
            if (JK_IS_DEBUG_LEVEL(logger)) {
                jk_log(logger, JK_LOG_DEBUG, "Request is Transfer-Encoding: chunked");
            }
        }
        else {
            /* XXX: What to do with non chunked T-E ?
             */
            if (JK_IS_DEBUG_LEVEL(logger))
                jk_log(logger, JK_LOG_DEBUG, "Unsupported Transfer-Encoding: %s",
                       temp_buf);
        }
    }
    if (private_data->lpEcb->cbTotalBytes == 0xFFFFFFFF) {
        /* We have size larger then 4Gb or Transfer-Encoding: chunked
         * ReadClient should be called until no more data is returned
         */
        unknown_content_length = TRUE;
    }
    else {
        /* Use the IIS provided content length */
        s->content_length = (jk_uint64_t)private_data->lpEcb->cbTotalBytes;
    }
    e = get_uri_to_worker_ext(uw_map, worker_index);
    if (e) {
        if (JK_IS_DEBUG_LEVEL(logger))
            jk_log(logger, JK_LOG_DEBUG, "Applying service extensions" );
        s->extension.reply_timeout = e->reply_timeout;
        s->extension.sticky_ignore = e->sticky_ignore;
        s->extension.stateless = e->stateless;
        s->extension.use_server_error_pages = e->use_server_error_pages;
        if (e->activation) {
            s->extension.activation = jk_pool_alloc(s->pool, e->activation_size * sizeof(int));
            memcpy(s->extension.activation, e->activation, e->activation_size * sizeof(int));
        }
        if (e->fail_on_status_size > 0) {
            s->extension.fail_on_status_size = e->fail_on_status_size;
            s->extension.fail_on_status = jk_pool_alloc(s->pool, e->fail_on_status_size * sizeof(int));
            memcpy(s->extension.fail_on_status, e->fail_on_status, e->fail_on_status_size * sizeof(int));
        }
        if (e->session_cookie) {
            s->extension.session_cookie = jk_pool_strdup(s->pool, e->session_cookie);
        }
        if (e->session_path) {
            s->extension.session_path = jk_pool_strdup(s->pool, e->session_path);
        }
        if (e->set_session_cookie) {
            s->extension.set_session_cookie = e->set_session_cookie;
        }
        if (e->session_cookie_path) {
            s->extension.session_cookie_path = jk_pool_strdup(s->pool, e->session_cookie_path);
        }

    }

    s->uw_map = uw_map;
    /*
     * Add SSL IIS environment
     */
    if (s->is_ssl) {
        char *ssl_env_names[9] = {
            "CERT_ISSUER",
            "CERT_SUBJECT",
            "CERT_COOKIE",
            "HTTPS_SERVER_SUBJECT",
            "CERT_FLAGS",
            "HTTPS_SECRETKEYSIZE",
            "CERT_SERIALNUMBER",
            "HTTPS_SERVER_ISSUER",
            "HTTPS_KEYSIZE"
        };
        char *ssl_env_values[9] = {
            NULL,
            NULL,
            NULL,
            NULL,
            NULL,
            NULL,
            NULL,
            NULL,
            NULL
        };
        unsigned int i;
        unsigned int num_of_vars = 0;

        for (i = 0; i < 9; i++) {
            GET_SERVER_VARIABLE_VALUE(ssl_env_names[i], ssl_env_values[i], NULL);
            if (ssl_env_values[i]) {
                num_of_vars++;
            }
        }
        /* XXX: To make the isapi plugin more consistent with the other web servers */
        /* we should also set s->ssl_cipher, s->ssl_session, and s->ssl_key_size. */
        if (num_of_vars) {
            unsigned int j = 0;

            s->attributes_names  = jk_pool_alloc(&private_data->p,
                                                 num_of_vars * sizeof(char *));
            s->attributes_values = jk_pool_alloc(&private_data->p,
                                                 num_of_vars * sizeof(char *));
            if (!s->attributes_names || !s->attributes_values) {
                JK_TRACE_EXIT(logger);
                return JK_FALSE;
            }
            for (i = 0; i < 9; i++) {
                if (ssl_env_values[i]) {
                    s->attributes_names[j] = ssl_env_names[i];
                    s->attributes_values[j] = ssl_env_values[i];
                    j++;
                }
            }
            s->num_attributes = num_of_vars;
            if (ssl_env_values[4] && ssl_env_values[4][0] == '1') {
                CERT_CONTEXT_EX cc;
                BYTE *cb = jk_pool_alloc(&private_data->p, AJP13_MAX_PACKET_SIZE);

                if (!cb) {
                    JK_TRACE_EXIT(logger);
                    return JK_FALSE;
                }
                cc.cbAllocated = AJP13_MAX_PACKET_SIZE;
                cc.CertContext.pbCertEncoded = cb;
                cc.CertContext.cbCertEncoded = 0;

                if (private_data->lpEcb->ServerSupportFunction(private_data->lpEcb->ConnID,
                                          HSE_REQ_GET_CERT_INFO_EX,
                                          &cc, NULL, NULL) != FALSE) {
                    jk_log(logger, JK_LOG_DEBUG,
                           "Client Certificate encoding:%d sz:%d flags:%ld",
                           cc.CertContext.
                           dwCertEncodingType & X509_ASN_ENCODING,
                           cc.CertContext.cbCertEncoded,
                           cc.dwCertificateFlags);
                    s->ssl_cert = jk_pool_alloc(&private_data->p,
                                      base64_encode_cert_len(cc.CertContext.cbCertEncoded));
                    s->ssl_cert_len = base64_encode_cert(s->ssl_cert, cb,
                                                         cc.CertContext.cbCertEncoded) - 1;
                }
            }
        }
    }

    GET_SERVER_VARIABLE_VALUE(ALL_HEADERS, all_headers, NULL);
    if (!all_headers) {
        JK_TRACE_EXIT(logger);
        return JK_FALSE;
    }
    for (tmp = all_headers; *tmp; tmp++) {
        if (*tmp == '\n') {
            cnt++;
        }
    }

    if (cnt) {
        char *headers_buf = all_headers;
        unsigned int i;
        BOOL need_content_length_header = FALSE;

        if (s->content_length == 0 && unknown_content_length == FALSE) {
            /* Add content-length=0 only if really zero
             */
            need_content_length_header = TRUE;
        }

        /* allocate an extra header slot in case we need to add a content-length header */
        s->headers_names  = jk_pool_alloc(&private_data->p,
                                         (cnt + 1) * sizeof(char *));
        s->headers_values = jk_pool_alloc(&private_data->p,
                                         (cnt + 1) * sizeof(char *));
        if (!s->headers_names || !s->headers_values) {
            JK_TRACE_EXIT(logger);
            return JK_FALSE;
        }

        for (i = 0, tmp = headers_buf; *tmp && i < cnt;) {
            int real_header = JK_TRUE;

#ifdef USE_CGI_HEADERS
            /* Skip the HTTP_ prefix to the beginning of the header name */
            tmp += HTTP_HEADER_PREFIX_LEN;
#endif

            if (!strnicmp(tmp, URI_HEADER_NAME, strlen(URI_HEADER_NAME)) ||
                !strnicmp(tmp, WORKER_HEADER_NAME, strlen(WORKER_HEADER_NAME)) ||
                !strnicmp(tmp, WORKER_HEADER_INDEX, strlen(WORKER_HEADER_INDEX)) ||
                !strnicmp(tmp, QUERY_HEADER_NAME, strlen(QUERY_HEADER_NAME))) {
                /* Skip redirector headers */
                cnt--;
                real_header = JK_FALSE;
            }
            else if (!strnicmp(tmp, CONTENT_LENGTH,
                               sizeof(CONTENT_LENGTH) - 1)) {
                need_content_length_header = FALSE;

                /* If the content-length is unknown
                 * or larger then 4Gb do not send it.
                 * IIS can also create a synthetic Content-Length header to make
                 * lpcbTotalBytes and the CONTENT_LENGTH server variable agree
                 * on small requests where the entire chunk encoded message is
                 * read into the available buffer.
                 */
                if (unknown_content_length || s->is_chunked) {
                    if (JK_IS_DEBUG_LEVEL(logger)) {
                        jk_log(logger, JK_LOG_DEBUG,
                               "Disregarding Content-Length in request - content is %s",
                               s->is_chunked ? "chunked" : "unknown length");
                    }
                    cnt--;
                    real_header = JK_FALSE;
                }
                else {
                    s->headers_names[i] = tmp;
                }
            }
            else if (!strnicmp(tmp, TOMCAT_TRANSLATE_HEADER_NAME,
                               strlen(TOMCAT_TRANSLATE_HEADER_NAME))) {
                s->headers_names[i] = TRANSLATE_HEADER_NAME_LC;
            }
            else {
                s->headers_names[i] = tmp;
            }

            while (':' != *tmp && *tmp) {
#ifdef USE_CGI_HEADERS
                if (real_header) {
                    if ('_' == *tmp) {
                        *tmp = '-';
                    }
                    else {
                        *tmp = JK_TOLOWER(*tmp);
                    }
                }
#endif
                tmp++;
            }
            *tmp++ = '\0';

            /* Skip all the WS chars after the ':' to the beginning of the header value */
            while (' ' == *tmp || '\t' == *tmp || '\v' == *tmp) {
                tmp++;
            }

            if (real_header) {
                s->headers_values[i] = tmp;
            }

            while (*tmp && *tmp != '\n' && *tmp != '\r') {
                tmp++;
            }
            *tmp++ = '\0';
            /* skip CR LF */
            while (*tmp == '\n' || *tmp == '\r') {
                tmp++;
            }

            if (real_header) {
                if (JK_IS_DEBUG_LEVEL(logger)) {
                    jk_log(logger, JK_LOG_DEBUG, "Forwarding request header %s : %s",
                           s->headers_names[i], s->headers_values[i]);
                }
                i++;
            }
        }
        /* Add a content-length = 0 header if needed.
         * Ajp13 assumes an absent content-length header means an unknown,
         * but non-zero length body.
         */
        if (need_content_length_header) {
            if (JK_IS_DEBUG_LEVEL(logger)) {
                jk_log(logger, JK_LOG_DEBUG, "Incoming request needs explicit Content-Length: 0 in AJP13");
            }
            s->headers_names[cnt] = "Content-Length";
            s->headers_values[cnt] = "0";
            cnt++;
        }
        s->num_headers = cnt;
    }
    else {
        JK_TRACE_EXIT(logger);
        return JK_FALSE;
    }

    /* Dump all connection param so we can trace what's going to
     * the remote tomcat
     */
    if (JK_IS_DEBUG_LEVEL(logger)) {
        jk_log(logger, JK_LOG_DEBUG,
               "Service protocol=%s method=%s host=%s addr=%s name=%s port=%d "
               "auth=%s user=%s uri=%s",
               STRNULL_FOR_NULL(s->protocol),
               STRNULL_FOR_NULL(s->method),
               STRNULL_FOR_NULL(s->remote_host),
               STRNULL_FOR_NULL(s->remote_addr),
               STRNULL_FOR_NULL(s->server_name),
               s->server_port,
               STRNULL_FOR_NULL(s->auth_type),
               STRNULL_FOR_NULL(s->remote_user),
               STRNULL_FOR_NULL(s->req_uri));
        jk_log(logger, JK_LOG_DEBUG,
               "Service request headers=%d attributes=%d "
               "chunked=%s content-length=%" JK_UINT64_T_FMT " available=%u",
               s->num_headers,
               s->num_attributes,
               (s->is_chunked == JK_TRUE) ? "yes" : "no",
               s->content_length,
               private_data->lpEcb->cbTotalBytes);
    }

    JK_TRACE_EXIT(logger);
    return JK_TRUE;
}

static BOOL get_server_value(LPEXTENSION_CONTROL_BLOCK lpEcb,
                            char *name, char *buf, size_t bufsz)
{
    DWORD sz = (DWORD)bufsz;

    buf[0]   = '\0';
    return lpEcb->GetServerVariable(lpEcb->ConnID, name,
                                    buf, &sz);
}

static char *dup_server_value(LPEXTENSION_CONTROL_BLOCK lpEcb,
                              const char *name, jk_pool_t *p,
                              const char *def)
{
    DWORD sz = HDR_BUFFER_SIZE;
    char buf[HDR_BUFFER_SIZE];
    char *dp;

    if (lpEcb->GetServerVariable(lpEcb->ConnID, (LPSTR)name, buf, &sz)) {
        if (sz <= 1) {
            return def == NULL ? NULL : jk_pool_strdup(p, def);
        }
        return jk_pool_strdup(p, buf);
    }
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER)
        return NULL;
    if ((dp = jk_pool_alloc(p, sz))) {
        if (lpEcb->GetServerVariable(lpEcb->ConnID, (LPSTR)name, dp, &sz))
            return dp;
    }
    return NULL;
}

static const char begin_cert[] = "-----BEGIN CERTIFICATE-----\r\n";

static const char end_cert[] = "-----END CERTIFICATE-----\r\n";

static const char basis_64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int base64_encode_cert_len(int len)
{
    int n = ((len + 2) / 3 * 4) + 1;    /* base64 encoded size */
    n += (n + 63 / 64) * 2;             /* add CRLF's */
    n += sizeof(begin_cert) + sizeof(end_cert) - 2;  /* add enclosing strings. */
    return n;
}

static int base64_encode_cert(char *encoded,
                              const char *string, int len)
{
    int i, c;
    char *p;
    const char *t;

    p = encoded;

    t = begin_cert;
    while (*t != '\0')
        *p++ = *t++;

    c = 0;
    for (i = 0; i < len - 2; i += 3) {
        *p++ = basis_64[(string[i] >> 2) & 0x3F];
        *p++ = basis_64[((string[i] & 0x3) << 4) |
                        ((int)(string[i + 1] & 0xF0) >> 4)];
        *p++ = basis_64[((string[i + 1] & 0xF) << 2) |
                        ((int)(string[i + 2] & 0xC0) >> 6)];
        *p++ = basis_64[string[i + 2] & 0x3F];
        c += 4;
        if (c >= 64) {
            *p++ = '\r';
            *p++ = '\n';
            c = 0;
        }
    }
    if (i < len) {
        *p++ = basis_64[(string[i] >> 2) & 0x3F];
        if (i == (len - 1)) {
            *p++ = basis_64[((string[i] & 0x3) << 4)];
            *p++ = '=';
        }
        else {
            *p++ = basis_64[((string[i] & 0x3) << 4) |
                            ((int)(string[i + 1] & 0xF0) >> 4)];
            *p++ = basis_64[((string[i + 1] & 0xF) << 2)];
        }
        *p++ = '=';
        c++;
    }
    if (c != 0) {
        *p++ = '\r';
        *p++ = '\n';
    }

    t = end_cert;
    while (*t != '\0')
        *p++ = *t++;

    *p++ = '\0';
    return (int)(p - encoded);
}

/**
* Determine version info and the primary notification event
*/
static int get_iis_info(iis_info_t* iis_info)
{
    HKEY hkey;
    int rv = JK_FALSE;

    iis_info->major = 0;
    iis_info->minor = 0;

    /* Retrieve the IIS version Major/Minor */
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, W3SVC_REGISTRY_KEY,
                     0, KEY_READ, &hkey) == ERROR_SUCCESS) {
        if (get_registry_config_number(hkey, "MajorVersion", &iis_info->major) == JK_TRUE &&
            get_registry_config_number(hkey, "MinorVersion", &iis_info->minor) == JK_TRUE) {
            rv = JK_TRUE;
        }
        CloseHandle(hkey);
    }
    return rv;
}

/*
 * 1. Remove  //?/ prefix
 * 2. Replace UNC/ with //
 */
static __inline char *NO2UNC(char *fname)
{
    if (fname[0] == '/' && fname[1] == '/' &&
        fname[2] == '?' && fname[3] == '/') {
        fname += 4;
        if (fname[0] == 'U' && fname[1] == 'N' &&
            fname[2] == 'C' && fname[3] == '/') {
            fname += 2;
            *fname = '/';
        }
        else if (fname[0] == 'U' && fname[1] == 'N' &&
            fname[2] == '/' && fname[3] == '/') {
            fname += 2;
            /* Already modified in-place.
             */
            fname += 2;
        }
    }
    return fname;
}

static __inline void FS2BSA(char *str)
{
    for (; *str != 0; str++) {
        if (*str == '/')
            *str = '\\';
    }
}

static __inline void BS2FSA(char *str)
{
    for (; *str != 0; str++) {
        if (*str == '\\')
            *str = '/';
    }
}

#define NON_UNC_PATH_LENGTH     248

#define IS_PATH_SEP(C)   ((C) == '/' || (C) == '\0')
#define IS_DRIVE_CHAR(C) (((C) >= 'A' && (C) <= 'Z') || \
                          ((C) >= 'a' && (C) <= 'z'))
#define _INS_TRAILING_PATH_SEP(s, l)  \
    if (l > 0 && s[l - 1] != '/') s[l++] = '/', s[l] = '\0'
#define _DEL_TRAILING_PATH_SEP(s, l)  \
    do {                                                    \
        if (l > 1 && s[l - 1] == '/') {                     \
            if (s[l - 2] != '/' && s[l - 2] != ':')         \
                s[--l] = '\0';                              \
        }                                                   \
    } while(0)

static char *skip_prefix(char *path, char **sp)
{
    size_t size;
    char *cp;

    /* Convert everything to forward slashes
     */
    BS2FSA(path);
    /* Remove \\?\ and replace \\?\UNC\ with \\
     */
    path = NO2UNC(path);
    size = strlen(path);
    *sp = path;
    if (size < 2) {
        if (path[0] == ' ') {
            /* Single Trailing space is invalid path
             */
            SetLastError(ERROR_BAD_PATHNAME);
            return 0;
        }
        return path;
    }
    else {
        /* Remove any trailing slash unless this is
         * a single slash path.
         */
        _DEL_TRAILING_PATH_SEP(path, size);
        if (path[size - 1] == ' ' || path[size - 1] == '.') {
            /* Trailing space and dot are invalid file or dir names
             */
            SetLastError(ERROR_BAD_PATHNAME);
            return 0;
        }
    }
    if (size > 1 && path[1] == ':' && IS_DRIVE_CHAR(path[0])) {
        /* Never go above C: */
        path += 2;
    }
    else if (path[0] == '/' && path[1] == '/') {
        /* Never go above //share/
         */
        if (path[2] == '.' && path[3] == '/') {
            /* This is probably //./pipe/ */
            return path;
        }
        cp = strchr(path + 2, '/');
        if (cp != 0)
            path = cp;
        else {
            /* We only have //share
             */
            return path;
        }
    }
    return path;
}

static char *relative_path(char *path, int* remain)
{
    char *sp;
    char *cp;
    int   ch = '/';

    path = skip_prefix(path, &sp);
    if (!path)
        return 0;
    if (path != sp) {
        /* Unexpected. Expected a relative path, but it starts with C: or //share/ */
        SetLastError(ERROR_BAD_PATHNAME);
        return 0;
    }
    cp = path;
    while (*path) {
        if (IS_PATH_SEP(ch) && *path == '.') {
            /* nd: number of consecutive dot characters */
            int nd = 0;
            while (path[nd] == '.')
                nd++;
            if (IS_PATH_SEP(path[nd])) {
                if (nd > 2) {
                    SetLastError(ERROR_BAD_PATHNAME);
                    return 0;
                }
                path += nd;
                if (*path)
                    path++;
                if (nd > 1) {
                    if (cp > sp + 1) {
                        cp--;
                        while (cp > sp) {
                            if (IS_PATH_SEP(*(cp - 1)))
                                break;
                            cp--;
                        }
                    }
                    else {
                        (*remain)++;
                    }
                }
            }
            else {
                ch = *cp++ = *path++;
            }
        }
        else {
            ch = *cp++ = *path++;
        }
    }
    *cp = '\0';
    return sp;
}

static char *path_merge(const char *root, const char *path)
{
    char merge[8192];
    char dir[MAX_PATH];
    char *rel;
    char *out = 0;
    size_t sz;
    size_t rsz;
    int remain = 0;

    if (root == NULL || path == NULL) {
        SetLastError(ERROR_INVALID_PARAMETER );
        return 0;
    }
    if (FAILED(StringCbCopy(dir, MAX_PATH, root))) {
        SetLastError(ERROR_FILENAME_EXCED_RANGE);
        return 0;
    }
    if (FAILED(StringCbCopy(merge, 8190, path))) {
        /* TODO: Use dynamic buffer with larger size */
        SetLastError(ERROR_FILENAME_EXCED_RANGE);
        return 0;
    }
    sz = strlen(merge);
    rsz = strlen(dir);
    /* Normalize path */
    if ((rel = relative_path(merge, &remain))) {
        size_t bl;
        if (remain > 0) {
            char *skip = dir + rsz - 1;
            char *spr;
            char *start = skip_prefix(dir, &spr);
            if (*skip == '/')
                skip--;
            while (remain > 0 && skip >= start) {
                if (*skip == '/') {
                    remain--;
                }
                skip--;
            }
            if (remain > 0) {
                SetLastError(ERROR_BAD_PATHNAME);
                return 0;
            }
            if (skip < start) {
                skip = start;
            }
            *++skip = '\0';
        }
        /* one additional byte for trailing '\0',
         * one additional byte for eventual path
         * separator between dir and merge */
        bl = strlen(dir) + sz + 2;
        out = malloc(bl);
        if (out == 0)
            return 0;
        /* Prepend dir */
        StringCbCopy(out, bl, dir);
        sz = strlen(out);
        BS2FSA(out);
        _INS_TRAILING_PATH_SEP(out, sz);
        if ((IS_DRIVE_CHAR(rel[0]) && rel[1] == ':'))
            rel += 2;
        if (rel[0] == '/')
            ++rel;
        if (*rel != '\0')
            StringCbCopy(out + sz, bl - sz, rel);
        FS2BSA(out);
    }
    return out;
}

static int is_path_relative(const char *path)
{
    if ((IS_DRIVE_CHAR(path[0]) && path[1] == ':') ||
         path[0] == '/' || path[0] == '\\')
        return 0;
    else
        return 1;
}
