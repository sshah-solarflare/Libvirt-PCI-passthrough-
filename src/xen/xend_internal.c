/*
 * xend_internal.c: access to Xen though the Xen Daemon interface
 *
 * Copyright (C) 2010-2011 Red Hat, Inc.
 * Copyright (C) 2005 Anthony Liguori <aliguori@us.ibm.com>
 *
 *  This file is subject to the terms and conditions of the GNU Lesser General
 *  Public License. See the file COPYING.LIB in the main directory of this
 *  archive for more details.
 */

#include <config.h>

#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/errno.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <stdarg.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <libxml/uri.h>
#include <errno.h>

#include "virterror_internal.h"
#include "logging.h"
#include "datatypes.h"
#include "xend_internal.h"
#include "driver.h"
#include "util.h"
#include "sexpr.h"
#include "xen_sxpr.h"
#include "buf.h"
#include "uuid.h"
#include "xen_driver.h"
#include "xen_hypervisor.h"
#include "xs_internal.h" /* To extract VNC port & Serial console TTY */
#include "memory.h"
#include "count-one-bits.h"
#include "virfile.h"

/* required for cpumap_t */
#include <xen/dom0_ops.h>

#define VIR_FROM_THIS VIR_FROM_XEND

/*
 * The number of Xen scheduler parameters
 */

#define XEND_RCV_BUF_MAX_LEN 65536

static int
virDomainXMLDevID(virDomainPtr domain,
                  virDomainDeviceDefPtr dev,
                  char *class,
                  char *ref,
                  int ref_len);

#define virXendError(code, ...)                                            \
        virReportErrorHelper(VIR_FROM_XEND, code, __FILE__,                \
                             __FUNCTION__, __LINE__, __VA_ARGS__)

#define virXendErrorInt(code, ival)                                        \
        virXendError(code, "%d", ival)

/**
 * do_connect:
 * @xend: pointer to the Xen Daemon structure
 *
 * Internal routine to (re)connect to the daemon
 *
 * Returns the socket file descriptor or -1 in case of error
 */
static int
do_connect(virConnectPtr xend)
{
    int s;
    int no_slow_start = 1;
    xenUnifiedPrivatePtr priv = (xenUnifiedPrivatePtr) xend->privateData;

    s = socket(priv->addrfamily, SOCK_STREAM, priv->addrprotocol);
    if (s == -1) {
        virXendError(VIR_ERR_INTERNAL_ERROR,
                     "%s", _("failed to create a socket"));
        return -1;
    }

    /*
     * try to desactivate slow-start
     */
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (void *)&no_slow_start,
               sizeof(no_slow_start));


    if (connect(s, (struct sockaddr *)&priv->addr, priv->addrlen) == -1) {
        VIR_FORCE_CLOSE(s); /* preserves errno */

        /*
         * Connecting to XenD when privileged is mandatory, so log this
         * error
         */
        if (xenHavePrivilege()) {
            virXendError(VIR_ERR_INTERNAL_ERROR,
                         "%s", _("failed to connect to xend"));
        }
    }

    return s;
}

/**
 * wr_sync:
 * @xend: the xend connection object
 * @fd:  the file descriptor
 * @buffer: the I/O buffer
 * @size: the size of the I/O
 * @do_read: write operation if 0, read operation otherwise
 *
 * Do a synchronous read or write on the file descriptor
 *
 * Returns the number of bytes exchanged, or -1 in case of error
 */
static size_t
wr_sync(int fd, void *buffer, size_t size, int do_read)
{
    size_t offset = 0;

    while (offset < size) {
        ssize_t len;

        if (do_read) {
            len = read(fd, ((char *) buffer) + offset, size - offset);
        } else {
            len = write(fd, ((char *) buffer) + offset, size - offset);
        }

        /* recoverable error, retry  */
        if ((len == -1) && ((errno == EAGAIN) || (errno == EINTR))) {
            continue;
        }

        /* eof */
        if (len == 0) {
            break;
        }

        /* unrecoverable error */
        if (len == -1) {
            if (do_read)
                virXendError(VIR_ERR_INTERNAL_ERROR,
                             "%s", _("failed to read from Xen Daemon"));
            else
                virXendError(VIR_ERR_INTERNAL_ERROR,
                             "%s", _("failed to write to Xen Daemon"));

            return (-1);
        }

        offset += len;
    }

    return offset;
}

/**
 * sread:
 * @fd:  the file descriptor
 * @buffer: the I/O buffer
 * @size: the size of the I/O
 *
 * Internal routine to do a synchronous read
 *
 * Returns the number of bytes read, or -1 in case of error
 */
static ssize_t
sread(int fd, void *buffer, size_t size)
{
    return wr_sync(fd, buffer, size, 1);
}

/**
 * swrite:
 * @fd:  the file descriptor
 * @buffer: the I/O buffer
 * @size: the size of the I/O
 *
 * Internal routine to do a synchronous write
 *
 * Returns the number of bytes written, or -1 in case of error
 */
static ssize_t
swrite(int fd, const void *buffer, size_t size)
{
    return wr_sync(fd, (void *) buffer, size, 0);
}

/**
 * swrites:
 * @fd:  the file descriptor
 * @string: the string to write
 *
 * Internal routine to do a synchronous write of a string
 *
 * Returns the number of bytes written, or -1 in case of error
 */
static ssize_t
swrites(int fd, const char *string)
{
    return swrite(fd, string, strlen(string));
}

/**
 * sreads:
 * @fd:  the file descriptor
 * @buffer: the I/O buffer
 * @n_buffer: the size of the I/O buffer
 *
 * Internal routine to do a synchronous read of a line
 *
 * Returns the number of bytes read, or -1 in case of error
 */
static ssize_t
sreads(int fd, char *buffer, size_t n_buffer)
{
    size_t offset;

    if (n_buffer < 1)
        return (-1);

    for (offset = 0; offset < (n_buffer - 1); offset++) {
        ssize_t ret;

        ret = sread(fd, buffer + offset, 1);
        if (ret == 0)
            break;
        else if (ret == -1)
            return ret;

        if (buffer[offset] == '\n') {
            offset++;
            break;
        }
    }
    buffer[offset] = 0;

    return offset;
}

static int
istartswith(const char *haystack, const char *needle)
{
    return STRCASEEQLEN(haystack, needle, strlen(needle));
}


/**
 * xend_req:
 * @fd: the file descriptor
 * @content: the buffer to store the content
 *
 * Read the HTTP response from a Xen Daemon request.
 * If the response contains content, memory is allocated to
 * hold the content.
 *
 * Returns the HTTP return code and @content is set to the
 * allocated memory containing HTTP content.
 */
static int ATTRIBUTE_NONNULL (2)
xend_req(int fd, char **content)
{
    char *buffer;
    size_t buffer_size = 4096;
    int content_length = 0;
    int retcode = 0;

    if (VIR_ALLOC_N(buffer, buffer_size) < 0) {
        virReportOOMError();
        return -1;
    }

    while (sreads(fd, buffer, buffer_size) > 0) {
        if (STREQ(buffer, "\r\n"))
            break;

        if (istartswith(buffer, "Content-Length: "))
            content_length = atoi(buffer + 16);
        else if (istartswith(buffer, "HTTP/1.1 "))
            retcode = atoi(buffer + 9);
    }

    VIR_FREE(buffer);

    if (content_length > 0) {
        ssize_t ret;

        if (content_length > XEND_RCV_BUF_MAX_LEN) {
            virXendError(VIR_ERR_INTERNAL_ERROR,
                         _("Xend returned HTTP Content-Length of %d, "
                           "which exceeds maximum of %d"),
                         content_length,
                         XEND_RCV_BUF_MAX_LEN);
            return -1;
        }

        /* Allocate one byte beyond the end of the largest buffer we will read.
           Combined with the fact that VIR_ALLOC_N zeros the returned buffer,
           this guarantees that "content" will always be NUL-terminated. */
        if (VIR_ALLOC_N(*content, content_length + 1) < 0 ) {
            virReportOOMError();
            return -1;
        }

        ret = sread(fd, *content, content_length);
        if (ret < 0)
            return -1;
    }

    return retcode;
}

/**
 * xend_get:
 * @xend: pointer to the Xen Daemon structure
 * @path: the path used for the HTTP request
 * @content: the buffer to store the content
 *
 * Do an HTTP GET RPC with the Xen Daemon
 *
 * Returns the HTTP return code or -1 in case or error.
 */
static int ATTRIBUTE_NONNULL(3)
xend_get(virConnectPtr xend, const char *path,
         char **content)
{
    int ret;
    int s = do_connect(xend);

    if (s == -1)
        return s;

    swrites(s, "GET ");
    swrites(s, path);
    swrites(s, " HTTP/1.1\r\n");

    swrites(s,
            "Host: localhost:8000\r\n"
            "Accept-Encoding: identity\r\n"
            "Content-Type: application/x-www-form-urlencoded\r\n" "\r\n");

    ret = xend_req(s, content);
    VIR_FORCE_CLOSE(s);

    if (((ret < 0) || (ret >= 300)) &&
        ((ret != 404) || (!STRPREFIX(path, "/xend/domain/")))) {
        virXendError(VIR_ERR_GET_FAILED,
                     _("%d status from xen daemon: %s:%s"),
                     ret, path, NULLSTR(*content));
    }

    return ret;
}

/**
 * xend_post:
 * @xend: pointer to the Xen Daemon structure
 * @path: the path used for the HTTP request
 * @ops: the information sent for the POST
 *
 * Do an HTTP POST RPC with the Xen Daemon, this usually makes changes at the
 * Xen level.
 *
 * Returns the HTTP return code or -1 in case or error.
 */
static int
xend_post(virConnectPtr xend, const char *path, const char *ops)
{
    char buffer[100];
    char *err_buf = NULL;
    int ret;
    int s = do_connect(xend);

    if (s == -1)
        return s;

    swrites(s, "POST ");
    swrites(s, path);
    swrites(s, " HTTP/1.1\r\n");

    swrites(s,
            "Host: localhost:8000\r\n"
            "Accept-Encoding: identity\r\n"
            "Content-Type: application/x-www-form-urlencoded\r\n"
            "Content-Length: ");
    snprintf(buffer, sizeof(buffer), "%d", (int) strlen(ops));
    swrites(s, buffer);
    swrites(s, "\r\n\r\n");
    swrites(s, ops);

    ret = xend_req(s, &err_buf);
    VIR_FORCE_CLOSE(s);

    if ((ret < 0) || (ret >= 300)) {
        virXendError(VIR_ERR_POST_FAILED,
                     _("xend_post: error from xen daemon: %s"), err_buf);
    } else if ((ret == 202) && err_buf && (strstr(err_buf, "failed") != NULL)) {
        virXendError(VIR_ERR_POST_FAILED,
                     _("xend_post: error from xen daemon: %s"), err_buf);
        ret = -1;
    } else if (((ret >= 200) && (ret <= 202)) && err_buf &&
               (strstr(err_buf, "xend.err") != NULL)) {
        /* This is to catch case of things like 'virsh dump Domain-0 foo'
         * which returns a success code, but the word 'xend.err'
         * in body to indicate error :-(
         */
        virXendError(VIR_ERR_POST_FAILED,
                     _("xend_post: error from xen daemon: %s"), err_buf);
        ret = -1;
    }

    VIR_FREE(err_buf);
    return ret;
}


/**
 * http2unix:
 * @ret: the http return code
 *
 * Convert the HTTP return code to 0/-1 and set errno if needed
 *
 * Return -1 in case of error code 0 otherwise
 */
static int
http2unix(int ret)
{
    switch (ret) {
        case -1:
            break;
        case 200:
        case 201:
        case 202:
            return 0;
        case 404:
            errno = ESRCH;
            break;
        case 500:
            errno = EIO;
            break;
        default:
            virXendErrorInt(VIR_ERR_HTTP_ERROR, ret);
            errno = EINVAL;
            break;
    }
    return -1;
}

/**
 * xend_op_ext:
 * @xend: pointer to the Xen Daemon structure
 * @path: path for the object
 * @key: the key for the operation
 * @ap: input values to pass to the operation
 *
 * internal routine to run a POST RPC operation to the Xen Daemon
 *
 * Returns 0 in case of success, -1 in case of failure.
 */
static int
xend_op_ext(virConnectPtr xend, const char *path, const char *key, va_list ap)
{
    const char *k = key, *v;
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    int ret;
    char *content;

    while (k) {
        v = va_arg(ap, const char *);

        virBufferAsprintf(&buf, "%s=%s", k, v);
        k = va_arg(ap, const char *);

        if (k)
            virBufferAddChar(&buf, '&');
    }

    if (virBufferError(&buf)) {
        virBufferFreeAndReset(&buf);
        virReportOOMError();
        return -1;
    }

    content = virBufferContentAndReset(&buf);
    VIR_DEBUG("xend op: %s\n", content);
    ret = http2unix(xend_post(xend, path, content));
    VIR_FREE(content);

    return ret;
}


/**
 * xend_op:
 * @xend: pointer to the Xen Daemon structure
 * @name: the domain name target of this operation
 * @key: the key for the operation
 * @ap: input values to pass to the operation
 * @...: input values to pass to the operation
 *
 * internal routine to run a POST RPC operation to the Xen Daemon targetting
 * a given domain.
 *
 * Returns 0 in case of success, -1 in case of failure.
 */
static int ATTRIBUTE_SENTINEL
xend_op(virConnectPtr xend, const char *name, const char *key, ...)
{
    char buffer[1024];
    va_list ap;
    int ret;

    snprintf(buffer, sizeof(buffer), "/xend/domain/%s", name);

    va_start(ap, key);
    ret = xend_op_ext(xend, buffer, key, ap);
    va_end(ap);

    return ret;
}


/**
 * sexpr_get:
 * @xend: pointer to the Xen Daemon structure
 * @fmt: format string for the path of the operation
 * @...: extra data to build the path of the operation
 *
 * Internal routine to run a simple GET RPC operation to the Xen Daemon
 *
 * Returns a parsed S-Expression in case of success, NULL in case of failure
 */
static struct sexpr *sexpr_get(virConnectPtr xend, const char *fmt, ...)
  ATTRIBUTE_FMT_PRINTF(2,3);

static struct sexpr *
sexpr_get(virConnectPtr xend, const char *fmt, ...)
{
    char *buffer = NULL;
    char path[1024];
    va_list ap;
    int ret;
    struct sexpr *res = NULL;

    va_start(ap, fmt);
    vsnprintf(path, sizeof(path), fmt, ap);
    va_end(ap);

    ret = xend_get(xend, path, &buffer);
    ret = http2unix(ret);
    if (ret == -1)
        goto cleanup;

    if (buffer == NULL)
        goto cleanup;

    res = string2sexpr(buffer);

cleanup:
    VIR_FREE(buffer);
    return res;
}

/**
 * sexpr_uuid:
 * @ptr: where to store the UUID, incremented
 * @sexpr: an S-Expression
 * @name: the name for the value
 *
 * convenience function to lookup an UUID value from the S-Expression
 *
 * Returns a -1 on error, 0 on success
 */
static int
sexpr_uuid(unsigned char *ptr, const struct sexpr *node, const char *path)
{
    const char *r = sexpr_node(node, path);
    if (!r)
        return -1;
    return virUUIDParse(r, ptr);
}


/**
 * urlencode:
 * @string: the input URL
 *
 * Encode an URL see RFC 2396 and following
 *
 * Returns the new string or NULL in case of error.
 */
static char *
urlencode(const char *string)
{
    size_t len = strlen(string);
    char *buffer;
    char *ptr;
    size_t i;

    if (VIR_ALLOC_N(buffer, len * 3 + 1) < 0) {
        virReportOOMError();
        return (NULL);
    }
    ptr = buffer;
    for (i = 0; i < len; i++) {
        switch (string[i]) {
            case ' ':
            case '\n':
            case '&':
                snprintf(ptr, 4, "%%%02x", string[i]);
                ptr += 3;
                break;
            default:
                *ptr = string[i];
                ptr++;
        }
    }

    *ptr = 0;

    return buffer;
}

/* PUBLIC FUNCTIONS */

/**
 * xenDaemonOpen_unix:
 * @conn: an existing virtual connection block
 * @path: the path for the Xen Daemon socket
 *
 * Creates a localhost Xen Daemon connection
 * Note: this doesn't try to check if the connection actually works
 *
 * Returns 0 in case of success, -1 in case of error.
 */
int
xenDaemonOpen_unix(virConnectPtr conn, const char *path)
{
    struct sockaddr_un *addr;
    xenUnifiedPrivatePtr priv;

    if ((conn == NULL) || (path == NULL))
        return (-1);

    priv = (xenUnifiedPrivatePtr) conn->privateData;
    memset(&priv->addr, 0, sizeof(priv->addr));
    priv->addrfamily = AF_UNIX;
    /*
     * This must be zero on Solaris at least for AF_UNIX (which should
     * really be PF_UNIX, but doesn't matter).
     */
    priv->addrprotocol = 0;
    priv->addrlen = sizeof(struct sockaddr_un);

    addr = (struct sockaddr_un *)&priv->addr;
    addr->sun_family = AF_UNIX;
    memset(addr->sun_path, 0, sizeof(addr->sun_path));
    if (virStrcpyStatic(addr->sun_path, path) == NULL)
        return -1;

    return (0);
}


/**
 * xenDaemonOpen_tcp:
 * @conn: an existing virtual connection block
 * @host: the host name for the Xen Daemon
 * @port: the port
 *
 * Creates a possibly remote Xen Daemon connection
 * Note: this doesn't try to check if the connection actually works
 *
 * Returns 0 in case of success, -1 in case of error.
 */
static int
xenDaemonOpen_tcp(virConnectPtr conn, const char *host, const char *port)
{
    xenUnifiedPrivatePtr priv;
    struct addrinfo *res, *r;
    struct addrinfo hints;
    int saved_errno = EINVAL;
    int ret;

    if ((conn == NULL) || (host == NULL) || (port == NULL))
        return (-1);

    priv = (xenUnifiedPrivatePtr) conn->privateData;

    priv->addrlen = 0;
    memset(&priv->addr, 0, sizeof(priv->addr));

    /* http://people.redhat.com/drepper/userapi-ipv6.html */
    memset (&hints, 0, sizeof hints);
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_ADDRCONFIG;

    ret = getaddrinfo (host, port, &hints, &res);
    if (ret != 0) {
        virXendError(VIR_ERR_UNKNOWN_HOST,
                     _("unable to resolve hostname '%s': %s"),
                     host, gai_strerror (ret));
        return -1;
    }

    /* Try to connect to each returned address in turn. */
    for (r = res; r; r = r->ai_next) {
        int sock;

        sock = socket (r->ai_family, SOCK_STREAM, r->ai_protocol);
        if (sock == -1) {
            saved_errno = errno;
            continue;
        }

        if (connect (sock, r->ai_addr, r->ai_addrlen) == -1) {
            saved_errno = errno;
            VIR_FORCE_CLOSE(sock);
            continue;
        }

        priv->addrlen = r->ai_addrlen;
        priv->addrfamily = r->ai_family;
        priv->addrprotocol = r->ai_protocol;
        memcpy(&priv->addr,
               r->ai_addr,
               r->ai_addrlen);
        VIR_FORCE_CLOSE(sock);
        break;
    }

    freeaddrinfo (res);

    if (!priv->addrlen) {
        /* Don't raise error when unprivileged, since proxy takes over */
        if (xenHavePrivilege())
            virReportSystemError(saved_errno,
                                 _("unable to connect to '%s:%s'"),
                                 host, port);
        return -1;
    }

    return 0;
}


/**
 * xend_wait_for_devices:
 * @xend: pointer to the Xen Daemon block
 * @name: name for the domain
 *
 * Block the domain until all the virtual devices are ready. This operation
 * is needed when creating a domain before resuming it.
 *
 * Returns 0 in case of success, -1 (with errno) in case of error.
 */
int
xend_wait_for_devices(virConnectPtr xend, const char *name)
{
    return xend_op(xend, name, "op", "wait_for_devices", NULL);
}


/**
 * xenDaemonListDomainsOld:
 * @xend: pointer to the Xen Daemon block
 *
 * This method will return an array of names of currently running
 * domains.  The memory should be released will a call to free().
 *
 * Returns a list of names or NULL in case of error.
 */
char **
xenDaemonListDomainsOld(virConnectPtr xend)
{
    size_t extra = 0;
    struct sexpr *root = NULL;
    char **ret = NULL;
    int count = 0;
    int i;
    char *ptr;
    struct sexpr *_for_i, *node;

    root = sexpr_get(xend, "/xend/domain");
    if (root == NULL)
        goto error;

    for (_for_i = root, node = root->u.s.car; _for_i->kind == SEXPR_CONS;
         _for_i = _for_i->u.s.cdr, node = _for_i->u.s.car) {
        if (node->kind != SEXPR_VALUE)
            continue;
        extra += strlen(node->u.value) + 1;
        count++;
    }

    /*
     * We can'tuse the normal allocation routines as we are mixing
     * an array of char * at the beginning followed by an array of char
     * ret points to the NULL terminated array of char *
     * ptr points to the current string after that array but in the same
     * allocated block
     */
    if (virAlloc((void *)&ptr,
                 (count + 1) * sizeof(char *) + extra * sizeof(char)) < 0)
        goto error;

    ret = (char **) ptr;
    ptr += sizeof(char *) * (count + 1);

    i = 0;
    for (_for_i = root, node = root->u.s.car; _for_i->kind == SEXPR_CONS;
         _for_i = _for_i->u.s.cdr, node = _for_i->u.s.car) {
        if (node->kind != SEXPR_VALUE)
            continue;
        ret[i] = ptr;
        strcpy(ptr, node->u.value);
        ptr += strlen(node->u.value) + 1;
        i++;
    }

    ret[i] = NULL;

  error:
    sexpr_free(root);
    return ret;
}


/**
 * xenDaemonDomainCreateXML:
 * @xend: A xend instance
 * @sexpr: An S-Expr description of the domain.
 *
 * This method will create a domain based on the passed in description.  The
 * domain will be paused after creation and must be unpaused with
 * xenDaemonResumeDomain() to begin execution.
 * This method may be deprecated once switching to XML-RPC based communcations
 * with xend.
 *
 * Returns 0 for success, -1 (with errno) on error
 */

int
xenDaemonDomainCreateXML(virConnectPtr xend, const char *sexpr)
{
    int ret, serrno;
    char *ptr;

    ptr = urlencode(sexpr);
    if (ptr == NULL) {
        /* this should be caught at the interface but ... */
        virXendError(VIR_ERR_INTERNAL_ERROR,
                     "%s", _("failed to urlencode the create S-Expr"));
        return (-1);
    }

    ret = xend_op(xend, "", "op", "create", "config", ptr, NULL);

    serrno = errno;
    VIR_FREE(ptr);
    errno = serrno;

    return ret;
}


/**
 * xenDaemonDomainLookupByName_ids:
 * @xend: A xend instance
 * @domname: The name of the domain
 * @uuid: return value for the UUID if not NULL
 *
 * This method looks up the id of a domain
 *
 * Returns the id on success; -1 (with errno) on error
 */
int
xenDaemonDomainLookupByName_ids(virConnectPtr xend, const char *domname,
                                unsigned char *uuid)
{
    struct sexpr *root;
    const char *value;
    int ret = -1;

    if (uuid != NULL)
        memset(uuid, 0, VIR_UUID_BUFLEN);
    root = sexpr_get(xend, "/xend/domain/%s?detail=1", domname);
    if (root == NULL)
        goto error;

    value = sexpr_node(root, "domain/domid");
    if (value == NULL) {
        virXendError(VIR_ERR_INTERNAL_ERROR,
                     "%s", _("domain information incomplete, missing domid"));
        goto error;
    }
    ret = strtol(value, NULL, 0);
    if ((ret == 0) && (value[0] != '0')) {
        virXendError(VIR_ERR_INTERNAL_ERROR,
                     "%s", _("domain information incorrect domid not numeric"));
        ret = -1;
    } else if (uuid != NULL) {
        if (sexpr_uuid(uuid, root, "domain/uuid") < 0) {
            virXendError(VIR_ERR_INTERNAL_ERROR,
                         "%s", _("domain information incomplete, missing uuid"));
        }
    }

  error:
    sexpr_free(root);
    return (ret);
}


/**
 * xenDaemonDomainLookupByID:
 * @xend: A xend instance
 * @id: The id of the domain
 * @name: return value for the name if not NULL
 * @uuid: return value for the UUID if not NULL
 *
 * This method looks up the name of a domain based on its id
 *
 * Returns the 0 on success; -1 (with errno) on error
 */
int
xenDaemonDomainLookupByID(virConnectPtr xend,
                          int id,
                          char **domname,
                          unsigned char *uuid)
{
    const char *name = NULL;
    struct sexpr *root;

    memset(uuid, 0, VIR_UUID_BUFLEN);

    root = sexpr_get(xend, "/xend/domain/%d?detail=1", id);
    if (root == NULL)
      goto error;

    name = sexpr_node(root, "domain/name");
    if (name == NULL) {
      virXendError(VIR_ERR_INTERNAL_ERROR,
                   "%s", _("domain information incomplete, missing name"));
      goto error;
    }
    if (domname) {
      *domname = strdup(name);
      if (*domname == NULL) {
          virReportOOMError();
          goto error;
      }
    }

    if (sexpr_uuid(uuid, root, "domain/uuid") < 0) {
      virXendError(VIR_ERR_INTERNAL_ERROR,
                   "%s", _("domain information incomplete, missing uuid"));
      goto error;
    }

    sexpr_free(root);
    return (0);

error:
    sexpr_free(root);
    if (domname)
        VIR_FREE(*domname);
    return (-1);
}


static int
xend_detect_config_version(virConnectPtr conn) {
    struct sexpr *root;
    const char *value;
    xenUnifiedPrivatePtr priv;

    if (!VIR_IS_CONNECT(conn)) {
        virXendError(VIR_ERR_INVALID_CONN, __FUNCTION__);
        return (-1);
    }

    priv = (xenUnifiedPrivatePtr) conn->privateData;

    root = sexpr_get(conn, "/xend/node/");
    if (root == NULL)
        return (-1);

    value = sexpr_node(root, "node/xend_config_format");

    if (value) {
        priv->xendConfigVersion = strtol(value, NULL, 10);
    }  else {
        /* Xen prior to 3.0.3 did not have the xend_config_format
           field, and is implicitly version 1. */
        priv->xendConfigVersion = 1;
    }
    sexpr_free(root);
    return (0);
}


/**
 * sexpr_to_xend_domain_state:
 * @root: an S-Expression describing a domain
 *
 * Internal routine getting the domain's state from the domain root provided.
 *
 * Returns domain's state.
 */
static int
ATTRIBUTE_NONNULL(1) ATTRIBUTE_NONNULL(2)
sexpr_to_xend_domain_state(virDomainPtr domain, const struct sexpr *root)
{
    const char *flags;
    int state = VIR_DOMAIN_NOSTATE;

    if ((flags = sexpr_node(root, "domain/state"))) {
        if (strchr(flags, 'c'))
            state = VIR_DOMAIN_CRASHED;
        else if (strchr(flags, 's'))
            state = VIR_DOMAIN_SHUTOFF;
        else if (strchr(flags, 'd'))
            state = VIR_DOMAIN_SHUTDOWN;
        else if (strchr(flags, 'p'))
            state = VIR_DOMAIN_PAUSED;
        else if (strchr(flags, 'b'))
            state = VIR_DOMAIN_BLOCKED;
        else if (strchr(flags, 'r'))
            state = VIR_DOMAIN_RUNNING;
    } else if (domain->id < 0) {
        /* Inactive domains don't have a state reported, so
           mark them SHUTOFF, rather than NOSTATE */
        state = VIR_DOMAIN_SHUTOFF;
    }

    return state;
}

/**
 * sexpr_to_xend_domain_info:
 * @root: an S-Expression describing a domain
 * @info: a info data structure to fill=up
 *
 * Internal routine filling up the info structure with the values from
 * the domain root provided.
 *
 * Returns 0 in case of success, -1 in case of error
 */
static int
sexpr_to_xend_domain_info(virDomainPtr domain, const struct sexpr *root,
                          virDomainInfoPtr info)
{
    int vcpus;

    if ((root == NULL) || (info == NULL))
        return (-1);

    info->state = sexpr_to_xend_domain_state(domain, root);
    info->memory = sexpr_u64(root, "domain/memory") << 10;
    info->maxMem = sexpr_u64(root, "domain/maxmem") << 10;
    info->cpuTime = sexpr_float(root, "domain/cpu_time") * 1000000000;

    vcpus = sexpr_int(root, "domain/vcpus");
    info->nrVirtCpu = count_one_bits_l(sexpr_u64(root, "domain/vcpu_avail"));
    if (!info->nrVirtCpu || vcpus < info->nrVirtCpu)
        info->nrVirtCpu = vcpus;

    return (0);
}

/**
 * sexpr_to_xend_node_info:
 * @root: an S-Expression describing a domain
 * @info: a info data structure to fill up
 *
 * Internal routine filling up the info structure with the values from
 * the node root provided.
 *
 * Returns 0 in case of success, -1 in case of error
 */
static int
sexpr_to_xend_node_info(const struct sexpr *root, virNodeInfoPtr info)
{
    const char *machine;


    if ((root == NULL) || (info == NULL))
        return (-1);

    machine = sexpr_node(root, "node/machine");
    if (machine == NULL) {
        info->model[0] = 0;
    } else {
        snprintf(&info->model[0], sizeof(info->model) - 1, "%s", machine);
        info->model[sizeof(info->model) - 1] = 0;
    }
    info->memory = (unsigned long) sexpr_u64(root, "node/total_memory") << 10;

    info->cpus = sexpr_int(root, "node/nr_cpus");
    info->mhz = sexpr_int(root, "node/cpu_mhz");
    info->nodes = sexpr_int(root, "node/nr_nodes");
    info->sockets = sexpr_int(root, "node/sockets_per_node");
    info->cores = sexpr_int(root, "node/cores_per_socket");
    info->threads = sexpr_int(root, "node/threads_per_core");

    /* Xen 3.2.0 replaces sockets_per_node with 'nr_cpus'.
     * Old Xen calculated sockets_per_node using its internal
     * nr_cpus / (nodes*cores*threads), so fake it ourselves
     * in the same way
     */
    if (info->sockets == 0) {
        int nr_cpus = sexpr_int(root, "node/nr_cpus");
        int procs = info->nodes * info->cores * info->threads;
        if (procs == 0) /* Sanity check in case of Xen bugs in futures..*/
            return (-1);
        info->sockets = nr_cpus / procs;
    }

    /* On systems where NUMA nodes are not composed of whole sockets either Xen
     * provided us wrong number of sockets per node or we computed the wrong
     * number in the compatibility code above. In such case, we compute the
     * correct number of sockets on the host, lie about the number of NUMA
     * nodes, and force apps to check capabilities XML for the actual NUMA
     * topology.
     */
    if (info->nodes * info->sockets * info->cores * info->threads
        != info->cpus) {
        info->nodes = 1;
        info->sockets = info->cpus / (info->cores * info->threads);
    }

    return (0);
}


/**
 * sexpr_to_xend_topology
 * @root: an S-Expression describing a node
 * @caps: capability info
 *
 * Internal routine populating capability info with
 * NUMA node mapping details
 *
 * Does nothing when the system doesn't support NUMA (not an error).
 *
 * Returns 0 in case of success, -1 in case of error
 */
static int
sexpr_to_xend_topology(const struct sexpr *root,
                       virCapsPtr caps)
{
    const char *nodeToCpu;
    const char *cur;
    char *cpuset = NULL;
    int *cpuNums = NULL;
    int cell, cpu, nb_cpus;
    int n = 0;
    int numCpus;

    nodeToCpu = sexpr_node(root, "node/node_to_cpu");
    if (nodeToCpu == NULL)
        return 0;               /* no NUMA support */

    numCpus = sexpr_int(root, "node/nr_cpus");


    if (VIR_ALLOC_N(cpuset, numCpus) < 0)
        goto memory_error;
    if (VIR_ALLOC_N(cpuNums, numCpus) < 0)
        goto memory_error;

    cur = nodeToCpu;
    while (*cur != 0) {
        /*
         * Find the next NUMA cell described in the xend output
         */
        cur = strstr(cur, "node");
        if (cur == NULL)
            break;
        cur += 4;
        cell = virParseNumber(&cur);
        if (cell < 0)
            goto parse_error;
        virSkipSpacesAndBackslash(&cur);
        if (*cur != ':')
            goto parse_error;
        cur++;
        virSkipSpacesAndBackslash(&cur);
        if (STRPREFIX(cur, "no cpus")) {
            nb_cpus = 0;
            for (cpu = 0; cpu < numCpus; cpu++)
                cpuset[cpu] = 0;
        } else {
            nb_cpus = virDomainCpuSetParse(&cur, 'n', cpuset, numCpus);
            if (nb_cpus < 0)
                goto error;
        }

        for (n = 0, cpu = 0; cpu < numCpus; cpu++)
            if (cpuset[cpu] == 1)
                cpuNums[n++] = cpu;

        if (virCapabilitiesAddHostNUMACell(caps,
                                           cell,
                                           nb_cpus,
                                           cpuNums) < 0)
            goto memory_error;
    }
    VIR_FREE(cpuNums);
    VIR_FREE(cpuset);
    return (0);

  parse_error:
    virXendError(VIR_ERR_XEN_CALL, "%s", _("topology syntax error"));
  error:
    VIR_FREE(cpuNums);
    VIR_FREE(cpuset);

    return (-1);

  memory_error:
    VIR_FREE(cpuNums);
    VIR_FREE(cpuset);
    virReportOOMError();
    return (-1);
}


/**
 * sexpr_to_domain:
 * @conn: an existing virtual connection block
 * @root: an S-Expression describing a domain
 *
 * Internal routine returning the associated virDomainPtr for this domain
 *
 * Returns the domain pointer or NULL in case of error.
 */
static virDomainPtr
sexpr_to_domain(virConnectPtr conn, const struct sexpr *root)
{
    virDomainPtr ret = NULL;
    unsigned char uuid[VIR_UUID_BUFLEN];
    const char *name;
    const char *tmp;
    xenUnifiedPrivatePtr priv;

    if ((conn == NULL) || (root == NULL))
        return(NULL);

    priv = (xenUnifiedPrivatePtr) conn->privateData;

    if (sexpr_uuid(uuid, root, "domain/uuid") < 0)
        goto error;
    name = sexpr_node(root, "domain/name");
    if (name == NULL)
        goto error;

    ret = virGetDomain(conn, name, uuid);
    if (ret == NULL) return NULL;

    tmp = sexpr_node(root, "domain/domid");
    /* New 3.0.4 XenD will not report a domid for inactive domains,
     * so only error out for old XenD
     */
    if (!tmp && priv->xendConfigVersion < 3)
        goto error;

    if (tmp)
        ret->id = sexpr_int(root, "domain/domid");
    else
        ret->id = -1; /* An inactive domain */

    return (ret);

error:
    virXendError(VIR_ERR_INTERNAL_ERROR,
                 "%s", _("failed to parse Xend domain information"));
    if (ret != NULL)
        virUnrefDomain(ret);
    return(NULL);
}


/*****************************************************************
 ******
 ******
 ******
 ******
             Refactored
 ******
 ******
 ******
 ******
 *****************************************************************/
/**
 * xenDaemonOpen:
 * @conn: an existing virtual connection block
 * @name: optional argument to select a connection type
 * @flags: combination of virDrvOpenFlag(s)
 *
 * Creates a localhost Xen Daemon connection
 *
 * Returns 0 in case of success, -1 in case of error.
 */
virDrvOpenStatus
xenDaemonOpen(virConnectPtr conn,
              virConnectAuthPtr auth ATTRIBUTE_UNUSED,
              unsigned int flags)
{
    char *port = NULL;
    int ret = VIR_DRV_OPEN_ERROR;

    virCheckFlags(VIR_CONNECT_RO, VIR_DRV_OPEN_ERROR);

    /* Switch on the scheme, which we expect to be NULL (file),
     * "http" or "xen".
     */
    if (conn->uri->scheme == NULL) {
        /* It should be a file access */
        if (conn->uri->path == NULL) {
            virXendError(VIR_ERR_NO_CONNECT, __FUNCTION__);
            goto failed;
        }
        if (xenDaemonOpen_unix(conn, conn->uri->path) < 0 ||
            xend_detect_config_version(conn) == -1)
            goto failed;
    }
    else if (STRCASEEQ (conn->uri->scheme, "xen")) {
        /*
         * try first to open the unix socket
         */
        if (xenDaemonOpen_unix(conn, "/var/lib/xend/xend-socket") == 0 &&
            xend_detect_config_version(conn) != -1)
            goto done;

        /*
         * try though http on port 8000
         */
        if (xenDaemonOpen_tcp(conn, "localhost", "8000") < 0 ||
            xend_detect_config_version(conn) == -1)
            goto failed;
    } else if (STRCASEEQ (conn->uri->scheme, "http")) {
        if (conn->uri->port &&
            virAsprintf(&port, "%d", conn->uri->port) == -1) {
            virReportOOMError();
            goto failed;
        }

        if (xenDaemonOpen_tcp(conn,
                              conn->uri->server ? conn->uri->server : "localhost",
                              port ? port : "8000") < 0 ||
            xend_detect_config_version(conn) == -1)
            goto failed;
    } else {
        virXendError(VIR_ERR_NO_CONNECT, __FUNCTION__);
        goto failed;
    }

 done:
    ret = VIR_DRV_OPEN_SUCCESS;

failed:
    VIR_FREE(port);
    return ret;
}


/**
 * xenDaemonClose:
 * @conn: an existing virtual connection block
 *
 * This method should be called when a connection to xend instance
 * initialized with xenDaemonOpen is no longer needed
 * to free the associated resources.
 *
 * Returns 0 in case of success, -1 in case of error
 */
int
xenDaemonClose(virConnectPtr conn ATTRIBUTE_UNUSED)
{
    return 0;
}

/**
 * xenDaemonDomainSuspend:
 * @domain: pointer to the Domain block
 *
 * Pause the domain, the domain is not scheduled anymore though its resources
 * are preserved. Use xenDaemonDomainResume() to resume execution.
 *
 * Returns 0 in case of success, -1 (with errno) in case of error.
 */
int
xenDaemonDomainSuspend(virDomainPtr domain)
{
    if ((domain == NULL) || (domain->conn == NULL) || (domain->name == NULL)) {
        virXendError(VIR_ERR_INVALID_ARG, __FUNCTION__);
        return(-1);
    }

    if (domain->id < 0) {
        virXendError(VIR_ERR_OPERATION_INVALID,
                     _("Domain %s isn't running."), domain->name);
        return(-1);
    }

    return xend_op(domain->conn, domain->name, "op", "pause", NULL);
}

/**
 * xenDaemonDomainResume:
 * @xend: pointer to the Xen Daemon block
 * @name: name for the domain
 *
 * Resume the domain after xenDaemonDomainSuspend() has been called
 *
 * Returns 0 in case of success, -1 (with errno) in case of error.
 */
int
xenDaemonDomainResume(virDomainPtr domain)
{
    if ((domain == NULL) || (domain->conn == NULL) || (domain->name == NULL)) {
        virXendError(VIR_ERR_INVALID_ARG, __FUNCTION__);
        return(-1);
    }

    if (domain->id < 0) {
        virXendError(VIR_ERR_OPERATION_INVALID,
                     _("Domain %s isn't running."), domain->name);
        return(-1);
    }

    return xend_op(domain->conn, domain->name, "op", "unpause", NULL);
}

/**
 * xenDaemonDomainShutdown:
 * @domain: pointer to the Domain block
 *
 * Shutdown the domain, the OS is requested to properly shutdown
 * and the domain may ignore it.  It will return immediately
 * after queuing the request.
 *
 * Returns 0 in case of success, -1 (with errno) in case of error.
 */
int
xenDaemonDomainShutdown(virDomainPtr domain)
{
    if ((domain == NULL) || (domain->conn == NULL) || (domain->name == NULL)) {
        virXendError(VIR_ERR_INVALID_ARG, __FUNCTION__);
        return(-1);
    }

    if (domain->id < 0) {
        virXendError(VIR_ERR_OPERATION_INVALID,
                     _("Domain %s isn't running."), domain->name);
        return(-1);
    }

    return xend_op(domain->conn, domain->name, "op", "shutdown", "reason", "poweroff", NULL);
}

/**
 * xenDaemonDomainReboot:
 * @domain: pointer to the Domain block
 * @flags: extra flags for the reboot operation, not used yet
 *
 * Reboot the domain, the OS is requested to properly shutdown
 * and restart but the domain may ignore it.  It will return immediately
 * after queuing the request.
 *
 * Returns 0 in case of success, -1 (with errno) in case of error.
 */
int
xenDaemonDomainReboot(virDomainPtr domain, unsigned int flags)
{
    virCheckFlags(0, -1);

    if ((domain == NULL) || (domain->conn == NULL) || (domain->name == NULL)) {
        virXendError(VIR_ERR_INVALID_ARG, __FUNCTION__);
        return(-1);
    }

    if (domain->id < 0) {
        virXendError(VIR_ERR_OPERATION_INVALID,
                     _("Domain %s isn't running."), domain->name);
        return(-1);
    }

    return xend_op(domain->conn, domain->name, "op", "shutdown", "reason", "reboot", NULL);
}

/**
 * xenDaemonDomainDestroyFlags:
 * @domain: pointer to the Domain block
 * @flags: an OR'ed set of virDomainDestroyFlagsValues
 *
 * Abruptly halt the domain, the OS is not properly shutdown and the
 * resources allocated for the domain are immediately freed, mounted
 * filesystems will be marked as uncleanly shutdown.
 * After calling this function, the domain's status will change to
 * dying and will go away completely once all of the resources have been
 * unmapped (usually from the backend devices).
 *
 * Calling this function with no @flags set (equal to zero)
 * is equivalent to calling xenDaemonDomainDestroy.
 *
 * Returns 0 in case of success, -1 (with errno) in case of error.
 */
int
xenDaemonDomainDestroyFlags(virDomainPtr domain,
                            unsigned int flags)
{
    virCheckFlags(0, -1);

    if ((domain == NULL) || (domain->conn == NULL) || (domain->name == NULL)) {
        virXendError(VIR_ERR_INVALID_ARG, __FUNCTION__);
        return(-1);
    }

    if (domain->id < 0) {
        virXendError(VIR_ERR_OPERATION_INVALID,
                     _("Domain %s isn't running."), domain->name);
        return(-1);
    }

    return xend_op(domain->conn, domain->name, "op", "destroy", NULL);
}

/**
 * xenDaemonDomainGetOSType:
 * @domain: a domain object
 *
 * Get the type of domain operation system.
 *
 * Returns the new string or NULL in case of error, the string must be
 *         freed by the caller.
 */
static char *
xenDaemonDomainGetOSType(virDomainPtr domain)
{
    char *type;
    struct sexpr *root;
    xenUnifiedPrivatePtr priv;

    if ((domain == NULL) || (domain->conn == NULL) || (domain->name == NULL)) {
        virXendError(VIR_ERR_INVALID_ARG, __FUNCTION__);
        return(NULL);
    }

    priv = (xenUnifiedPrivatePtr) domain->conn->privateData;

    if (domain->id < 0 && priv->xendConfigVersion < 3)
        return(NULL);

    /* can we ask for a subset ? worth it ? */
    root = sexpr_get(domain->conn, "/xend/domain/%s?detail=1", domain->name);
    if (root == NULL)
        return(NULL);

    if (sexpr_lookup(root, "domain/image/hvm")) {
        type = strdup("hvm");
    } else {
        type = strdup("linux");
    }

    if (type == NULL)
        virReportOOMError();

    sexpr_free(root);

    return(type);
}

/**
 * xenDaemonDomainSave:
 * @domain: pointer to the Domain block
 * @filename: path for the output file
 *
 * This method will suspend a domain and save its memory contents to
 * a file on disk.  Use xenDaemonDomainRestore() to restore a domain after
 * saving.
 * Note that for remote Xen Daemon the file path will be interpreted in
 * the remote host.
 *
 * Returns 0 in case of success, -1 (with errno) in case of error.
 */
int
xenDaemonDomainSave(virDomainPtr domain, const char *filename)
{
    if ((domain == NULL) || (domain->conn == NULL) || (domain->name == NULL) ||
        (filename == NULL)) {
        virXendError(VIR_ERR_INVALID_ARG, __FUNCTION__);
        return(-1);
    }

    if (domain->id < 0) {
        virXendError(VIR_ERR_OPERATION_INVALID,
                     _("Domain %s isn't running."), domain->name);
        return(-1);
    }

    /* We can't save the state of Domain-0, that would mean stopping it too */
    if (domain->id == 0) {
        return(-1);
    }

    return xend_op(domain->conn, domain->name, "op", "save", "file", filename, NULL);
}

/**
 * xenDaemonDomainCoreDump:
 * @domain: pointer to the Domain block
 * @filename: path for the output file
 * @flags: extra flags, currently unused
 *
 * This method will dump the core of a domain on a given file for analysis.
 * Note that for remote Xen Daemon the file path will be interpreted in
 * the remote host.
 *
 * Returns 0 in case of success, -1 in case of error.
 */
int
xenDaemonDomainCoreDump(virDomainPtr domain, const char *filename,
                        unsigned int flags)
{
    virCheckFlags(VIR_DUMP_LIVE | VIR_DUMP_CRASH, -1);

    if ((domain == NULL) || (domain->conn == NULL) || (domain->name == NULL) ||
        (filename == NULL)) {
        virXendError(VIR_ERR_INVALID_ARG, __FUNCTION__);
        return(-1);
    }

    if (domain->id < 0) {
        virXendError(VIR_ERR_OPERATION_INVALID,
                     _("Domain %s isn't running."), domain->name);
        return(-1);
    }

    return xend_op(domain->conn, domain->name,
                   "op", "dump", "file", filename,
                   "live", (flags & VIR_DUMP_LIVE ? "1" : "0"),
                   "crash", (flags & VIR_DUMP_CRASH ? "1" : "0"),
                   NULL);
}

/**
 * xenDaemonDomainRestore:
 * @conn: pointer to the Xen Daemon block
 * @filename: path for the output file
 *
 * This method will restore a domain saved to disk by xenDaemonDomainSave().
 * Note that for remote Xen Daemon the file path will be interpreted in
 * the remote host.
 *
 * Returns 0 in case of success, -1 (with errno) in case of error.
 */
int
xenDaemonDomainRestore(virConnectPtr conn, const char *filename)
{
    if ((conn == NULL) || (filename == NULL)) {
        /* this should be caught at the interface but ... */
        virXendError(VIR_ERR_INVALID_ARG, __FUNCTION__);
        return (-1);
    }
    return xend_op(conn, "", "op", "restore", "file", filename, NULL);
}


/**
 * xenDaemonDomainGetMaxMemory:
 * @domain: pointer to the domain block
 *
 * Ask the Xen Daemon for the maximum memory allowed for a domain
 *
 * Returns the memory size in kilobytes or 0 in case of error.
 */
unsigned long
xenDaemonDomainGetMaxMemory(virDomainPtr domain)
{
    unsigned long ret = 0;
    struct sexpr *root;
    xenUnifiedPrivatePtr priv;

    if ((domain == NULL) || (domain->conn == NULL) || (domain->name == NULL)) {
        virXendError(VIR_ERR_INVALID_ARG, __FUNCTION__);
        return(-1);
    }

    priv = (xenUnifiedPrivatePtr) domain->conn->privateData;

    if (domain->id < 0 && priv->xendConfigVersion < 3)
        return(-1);

    /* can we ask for a subset ? worth it ? */
    root = sexpr_get(domain->conn, "/xend/domain/%s?detail=1", domain->name);
    if (root == NULL)
        return(0);

    ret = (unsigned long) sexpr_u64(root, "domain/memory") << 10;
    sexpr_free(root);

    return(ret);
}


/**
 * xenDaemonDomainSetMaxMemory:
 * @domain: pointer to the Domain block
 * @memory: The maximum memory in kilobytes
 *
 * This method will set the maximum amount of memory that can be allocated to
 * a domain.  Please note that a domain is able to allocate up to this amount
 * on its own.
 *
 * Returns 0 for success; -1 (with errno) on error
 */
int
xenDaemonDomainSetMaxMemory(virDomainPtr domain, unsigned long memory)
{
    char buf[1024];
    xenUnifiedPrivatePtr priv;

    if ((domain == NULL) || (domain->conn == NULL) || (domain->name == NULL)) {
        virXendError(VIR_ERR_INVALID_ARG, __FUNCTION__);
        return(-1);
    }

    priv = (xenUnifiedPrivatePtr) domain->conn->privateData;

    if (domain->id < 0 && priv->xendConfigVersion < 3)
        return(-1);

    snprintf(buf, sizeof(buf), "%lu", VIR_DIV_UP(memory, 1024));
    return xend_op(domain->conn, domain->name, "op", "maxmem_set", "memory",
                   buf, NULL);
}

/**
 * xenDaemonDomainSetMemory:
 * @domain: pointer to the Domain block
 * @memory: The target memory in kilobytes
 *
 * This method will set a target memory allocation for a given domain and
 * request that the guest meet this target.  The guest may or may not actually
 * achieve this target.  When this function returns, it does not signify that
 * the domain has actually reached that target.
 *
 * Memory for a domain can only be allocated up to the maximum memory setting.
 * There is no safe guard for allocations that are too small so be careful
 * when using this function to reduce a domain's memory usage.
 *
 * Returns 0 for success; -1 (with errno) on error
 */
int
xenDaemonDomainSetMemory(virDomainPtr domain, unsigned long memory)
{
    char buf[1024];
    xenUnifiedPrivatePtr priv;

    if ((domain == NULL) || (domain->conn == NULL) || (domain->name == NULL)) {
        virXendError(VIR_ERR_INVALID_ARG, __FUNCTION__);
        return(-1);
    }

    priv = (xenUnifiedPrivatePtr) domain->conn->privateData;

    if (domain->id < 0 && priv->xendConfigVersion < 3)
        return(-1);

    snprintf(buf, sizeof(buf), "%lu", VIR_DIV_UP(memory, 1024));
    return xend_op(domain->conn, domain->name, "op", "mem_target_set",
                   "target", buf, NULL);
}


virDomainDefPtr
xenDaemonDomainFetch(virConnectPtr conn,
                     int domid,
                     const char *name,
                     const char *cpus)
{
    struct sexpr *root;
    xenUnifiedPrivatePtr priv;
    virDomainDefPtr def;
    int id;
    char * tty;
    int vncport;

    if (name)
        root = sexpr_get(conn, "/xend/domain/%s?detail=1", name);
    else
        root = sexpr_get(conn, "/xend/domain/%d?detail=1", domid);
    if (root == NULL) {
        virXendError(VIR_ERR_XEN_CALL,
                      "%s", _("xenDaemonDomainFetch failed to"
                        " find this domain"));
        return (NULL);
    }

    priv = (xenUnifiedPrivatePtr) conn->privateData;

    id = xenGetDomIdFromSxpr(root, priv->xendConfigVersion);
    xenUnifiedLock(priv);
    tty = xenStoreDomainGetConsolePath(conn, id);
    vncport = xenStoreDomainGetVNCPort(conn, id);
    xenUnifiedUnlock(priv);
    if (!(def = xenParseSxpr(root,
                             priv->xendConfigVersion,
                             cpus,
                             tty,
                             vncport)))
        goto cleanup;

cleanup:
    sexpr_free(root);

    return (def);
}


/**
 * xenDaemonDomainGetXMLDesc:
 * @domain: a domain object
 * @flags: potential dump flags
 * @cpus: list of cpu the domain is pinned to.
 *
 * Provide an XML description of the domain.
 *
 * Returns a 0 terminated UTF-8 encoded XML instance, or NULL in case of error.
 *         the caller must free() the returned value.
 */
char *
xenDaemonDomainGetXMLDesc(virDomainPtr domain, unsigned int flags,
                          const char *cpus)
{
    xenUnifiedPrivatePtr priv;
    virDomainDefPtr def;
    char *xml;

    /* Flags checked by virDomainDefFormat */

    if ((domain == NULL) || (domain->conn == NULL) || (domain->name == NULL)) {
        virXendError(VIR_ERR_INVALID_ARG, __FUNCTION__);
        return(NULL);
    }
    priv = (xenUnifiedPrivatePtr) domain->conn->privateData;

    if (domain->id < 0 && priv->xendConfigVersion < 3) {
        /* fall-through to the next driver to handle */
        return(NULL);
    }

    if (!(def = xenDaemonDomainFetch(domain->conn,
                                     domain->id,
                                     domain->name,
                                     cpus)))
        return(NULL);

    xml = virDomainDefFormat(def, flags);

    virDomainDefFree(def);

    return xml;
}


/**
 * xenDaemonDomainGetInfo:
 * @domain: a domain object
 * @info: pointer to a virDomainInfo structure allocated by the user
 *
 * This method looks up information about a domain and update the
 * information block provided.
 *
 * Returns 0 in case of success, -1 in case of error
 */
int
xenDaemonDomainGetInfo(virDomainPtr domain, virDomainInfoPtr info)
{
    struct sexpr *root;
    int ret;
    xenUnifiedPrivatePtr priv;

    if ((domain == NULL) || (domain->conn == NULL) || (domain->name == NULL) ||
        (info == NULL)) {
        virXendError(VIR_ERR_INVALID_ARG, __FUNCTION__);
        return(-1);
    }

    priv = (xenUnifiedPrivatePtr) domain->conn->privateData;

    if (domain->id < 0 && priv->xendConfigVersion < 3)
        return(-1);

    root = sexpr_get(domain->conn, "/xend/domain/%s?detail=1", domain->name);
    if (root == NULL)
        return (-1);

    ret = sexpr_to_xend_domain_info(domain, root, info);
    sexpr_free(root);
    return (ret);
}


/**
 * xenDaemonDomainGetState:
 * @domain: a domain object
 * @state: returned domain's state
 * @reason: returned reason for the state
 * @flags: additional flags, 0 for now
 *
 * This method looks up domain state and reason.
 *
 * Returns 0 in case of success, -1 in case of error
 */
int
xenDaemonDomainGetState(virDomainPtr domain,
                        int *state,
                        int *reason,
                        unsigned int flags)
{
    xenUnifiedPrivatePtr priv = domain->conn->privateData;
    struct sexpr *root;

    virCheckFlags(0, -1);

    if (domain->id < 0 && priv->xendConfigVersion < 3)
        return -1;

    root = sexpr_get(domain->conn, "/xend/domain/%s?detail=1", domain->name);
    if (!root)
        return -1;

    *state = sexpr_to_xend_domain_state(domain, root);
    if (reason)
        *reason = 0;

    sexpr_free(root);
    return 0;
}


/**
 * xenDaemonLookupByName:
 * @conn: A xend instance
 * @name: The name of the domain
 *
 * This method looks up information about a domain and returns
 * it in the form of a struct xend_domain.  This should be
 * free()'d when no longer needed.
 *
 * Returns domain info on success; NULL (with errno) on error
 */
virDomainPtr
xenDaemonLookupByName(virConnectPtr conn, const char *domname)
{
    struct sexpr *root;
    virDomainPtr ret = NULL;

    if ((conn == NULL) || (domname == NULL)) {
        virXendError(VIR_ERR_INVALID_ARG, __FUNCTION__);
        return(NULL);
    }

    root = sexpr_get(conn, "/xend/domain/%s?detail=1", domname);
    if (root == NULL)
        goto error;

    ret = sexpr_to_domain(conn, root);

error:
    sexpr_free(root);
    return(ret);
}


/**
 * xenDaemonNodeGetInfo:
 * @conn: pointer to the Xen Daemon block
 * @info: pointer to a virNodeInfo structure allocated by the user
 *
 * Extract hardware information about the node.
 *
 * Returns 0 in case of success and -1 in case of failure.
 */
int
xenDaemonNodeGetInfo(virConnectPtr conn, virNodeInfoPtr info) {
    int ret = -1;
    struct sexpr *root;

    if (!VIR_IS_CONNECT(conn)) {
        virXendError(VIR_ERR_INVALID_CONN, __FUNCTION__);
        return (-1);
    }
    if (info == NULL) {
        virXendError(VIR_ERR_INVALID_ARG, __FUNCTION__);
        return (-1);
    }

    root = sexpr_get(conn, "/xend/node/");
    if (root == NULL)
        return (-1);

    ret = sexpr_to_xend_node_info(root, info);
    sexpr_free(root);
    return (ret);
}

/**
 * xenDaemonNodeGetTopology:
 * @conn: pointer to the Xen Daemon block
 * @caps: capabilities info
 *
 * This method retrieves a node's topology information.
 *
 * Returns -1 in case of error, 0 otherwise.
 */
int
xenDaemonNodeGetTopology(virConnectPtr conn,
                         virCapsPtr caps) {
    int ret = -1;
    struct sexpr *root;

    if (!VIR_IS_CONNECT(conn)) {
        virXendError(VIR_ERR_INVALID_CONN, __FUNCTION__);
        return (-1);
    }

    if (caps == NULL) {
        virXendError(VIR_ERR_INVALID_ARG, __FUNCTION__);
        return (-1);
    }

    root = sexpr_get(conn, "/xend/node/");
    if (root == NULL) {
        return (-1);
    }

    ret = sexpr_to_xend_topology(root, caps);
    sexpr_free(root);
    return (ret);
}

/**
 * xenDaemonGetVersion:
 * @conn: pointer to the Xen Daemon block
 * @hvVer: return value for the version of the running hypervisor (OUT)
 *
 * Get the version level of the Hypervisor running.
 *
 * Returns -1 in case of error, 0 otherwise. if the version can't be
 *    extracted by lack of capacities returns 0 and @hvVer is 0, otherwise
 *    @hvVer value is major * 1,000,000 + minor * 1,000 + release
 */
int
xenDaemonGetVersion(virConnectPtr conn, unsigned long *hvVer)
{
    struct sexpr *root;
    int major, minor;
    unsigned long version;

    if (!VIR_IS_CONNECT(conn)) {
        virXendError(VIR_ERR_INVALID_CONN, __FUNCTION__);
        return (-1);
    }
    if (hvVer == NULL) {
        virXendError(VIR_ERR_INVALID_ARG, __FUNCTION__);
        return (-1);
    }
    root = sexpr_get(conn, "/xend/node/");
    if (root == NULL)
        return(-1);

    major = sexpr_int(root, "node/xen_major");
    minor = sexpr_int(root, "node/xen_minor");
    sexpr_free(root);
    version = major * 1000000 + minor * 1000;
    *hvVer = version;
    return(0);
}


/**
 * xenDaemonListDomains:
 * @conn: pointer to the hypervisor connection
 * @ids: array to collect the list of IDs of active domains
 * @maxids: size of @ids
 *
 * Collect the list of active domains, and store their ID in @maxids
 * TODO: this is quite expensive at the moment since there isn't one
 *       xend RPC providing both name and id for all domains.
 *
 * Returns the number of domain found or -1 in case of error
 */
int
xenDaemonListDomains(virConnectPtr conn, int *ids, int maxids)
{
    struct sexpr *root = NULL;
    int ret = -1;
    struct sexpr *_for_i, *node;
    long id;

    if (maxids == 0)
        return(0);

    if ((ids == NULL) || (maxids < 0))
        goto error;
    root = sexpr_get(conn, "/xend/domain");
    if (root == NULL)
        goto error;

    ret = 0;

    for (_for_i = root, node = root->u.s.car; _for_i->kind == SEXPR_CONS;
         _for_i = _for_i->u.s.cdr, node = _for_i->u.s.car) {
        if (node->kind != SEXPR_VALUE)
            continue;
        id = xenDaemonDomainLookupByName_ids(conn, node->u.value, NULL);
        if (id >= 0)
            ids[ret++] = (int) id;
        if (ret >= maxids)
            break;
    }

error:
    sexpr_free(root);
    return(ret);
}

/**
 * xenDaemonNumOfDomains:
 * @conn: pointer to the hypervisor connection
 *
 * Provides the number of active domains.
 *
 * Returns the number of domain found or -1 in case of error
 */
int
xenDaemonNumOfDomains(virConnectPtr conn)
{
    struct sexpr *root = NULL;
    int ret = -1;
    struct sexpr *_for_i, *node;

    root = sexpr_get(conn, "/xend/domain");
    if (root == NULL)
        goto error;

    ret = 0;

    for (_for_i = root, node = root->u.s.car; _for_i->kind == SEXPR_CONS;
         _for_i = _for_i->u.s.cdr, node = _for_i->u.s.car) {
        if (node->kind != SEXPR_VALUE)
            continue;
        ret++;
    }

error:
    sexpr_free(root);
    return(ret);
}


/**
 * xenDaemonLookupByID:
 * @conn: pointer to the hypervisor connection
 * @id: the domain ID number
 *
 * Try to find a domain based on the hypervisor ID number
 *
 * Returns a new domain object or NULL in case of failure
 */
virDomainPtr
xenDaemonLookupByID(virConnectPtr conn, int id) {
    char *name = NULL;
    unsigned char uuid[VIR_UUID_BUFLEN];
    virDomainPtr ret;

    if (xenDaemonDomainLookupByID(conn, id, &name, uuid) < 0) {
        goto error;
    }

    ret = virGetDomain(conn, name, uuid);
    if (ret == NULL) goto error;

    ret->id = id;
    VIR_FREE(name);
    return (ret);

 error:
    VIR_FREE(name);
    return (NULL);
}

/**
 * xenDaemonDomainSetVcpusFlags:
 * @domain: pointer to domain object
 * @nvcpus: the new number of virtual CPUs for this domain
 * @flags: bitwise-ORd from virDomainVcpuFlags
 *
 * Change virtual CPUs allocation of domain according to flags.
 *
 * Returns 0 on success, -1 if an error message was issued, and -2 if
 * the unified driver should keep trying.
 */
int
xenDaemonDomainSetVcpusFlags(virDomainPtr domain, unsigned int vcpus,
                             unsigned int flags)
{
    char buf[VIR_UUID_BUFLEN];
    xenUnifiedPrivatePtr priv;
    int max;

    virCheckFlags(VIR_DOMAIN_VCPU_LIVE |
                  VIR_DOMAIN_VCPU_CONFIG |
                  VIR_DOMAIN_VCPU_MAXIMUM, -1);

    if ((domain == NULL) || (domain->conn == NULL) || (domain->name == NULL)
        || (vcpus < 1)) {
        virXendError(VIR_ERR_INVALID_ARG, __FUNCTION__);
        return (-1);
    }

    priv = (xenUnifiedPrivatePtr) domain->conn->privateData;

    if ((domain->id < 0 && priv->xendConfigVersion < 3) ||
        (flags & VIR_DOMAIN_VCPU_MAXIMUM))
        return -2;

    /* With xendConfigVersion 2, only _LIVE is supported.  With
     * xendConfigVersion 3, only _LIVE|_CONFIG is supported for
     * running domains, or _CONFIG for inactive domains.  */
    if (priv->xendConfigVersion < 3) {
        if (flags & VIR_DOMAIN_VCPU_CONFIG) {
            virXendError(VIR_ERR_OPERATION_INVALID, "%s",
                         _("Xend version does not support modifying "
                           "persistent config"));
            return -1;
        }
    } else if (domain->id < 0) {
        if (flags & VIR_DOMAIN_VCPU_LIVE) {
            virXendError(VIR_ERR_OPERATION_INVALID, "%s",
                         _("domain not running"));
            return -1;
        }
    } else {
        if ((flags & (VIR_DOMAIN_VCPU_LIVE | VIR_DOMAIN_VCPU_CONFIG)) !=
            (VIR_DOMAIN_VCPU_LIVE | VIR_DOMAIN_VCPU_CONFIG)) {
            virXendError(VIR_ERR_OPERATION_INVALID, "%s",
                         _("Xend only supports modifying both live and "
                           "persistent config"));
        }
    }

    /* Unfortunately, xend_op does not validate whether this exceeds
     * the maximum.  */
    flags |= VIR_DOMAIN_VCPU_MAXIMUM;
    if ((max = xenDaemonDomainGetVcpusFlags(domain, flags)) < 0) {
        virXendError(VIR_ERR_OPERATION_INVALID, "%s",
                     _("could not determin max vcpus for the domain"));
        return -1;
    }
    if (vcpus > max) {
        virXendError(VIR_ERR_INVALID_ARG,
                     _("requested vcpus is greater than max allowable"
                       " vcpus for the domain: %d > %d"), vcpus, max);
        return -1;
    }

    snprintf(buf, sizeof(buf), "%d", vcpus);
    return xend_op(domain->conn, domain->name, "op", "set_vcpus", "vcpus",
                   buf, NULL);
}

/**
 * xenDaemonDomainPinCpu:
 * @domain: pointer to domain object
 * @vcpu: virtual CPU number
 * @cpumap: pointer to a bit map of real CPUs (in 8-bit bytes)
 * @maplen: length of cpumap in bytes
 *
 * Dynamically change the real CPUs which can be allocated to a virtual CPU.
 * NOTE: The XenD cpu affinity map format changed from "[0,1,2]" to
 *       "0,1,2"
 *       the XenD cpu affinity works only after cset 19579.
 *       there is no fine grained xend version detection possible, so we
 *       use the old format for anything before version 3
 *
 * Returns 0 for success; -1 (with errno) on error
 */
int
xenDaemonDomainPinVcpu(virDomainPtr domain, unsigned int vcpu,
                       unsigned char *cpumap, int maplen)
{
    char buf[VIR_UUID_BUFLEN], mapstr[sizeof(cpumap_t) * 64];
    int i, j, ret;
    xenUnifiedPrivatePtr priv;
    virDomainDefPtr def = NULL;

    if ((domain == NULL) || (domain->conn == NULL) || (domain->name == NULL)
     || (cpumap == NULL) || (maplen < 1) || (maplen > (int)sizeof(cpumap_t))) {
        virXendError(VIR_ERR_INVALID_ARG, __FUNCTION__);
        return (-1);
    }

    priv = (xenUnifiedPrivatePtr) domain->conn->privateData;
    if (priv->xendConfigVersion < 3) {
        mapstr[0] = '[';
        mapstr[1] = 0;
    } else {
        mapstr[0] = 0;
    }

    /* from bit map, build character string of mapped CPU numbers */
    for (i = 0; i < maplen; i++) for (j = 0; j < 8; j++)
     if (cpumap[i] & (1 << j)) {
        snprintf(buf, sizeof(buf), "%d,", (8 * i) + j);
        strcat(mapstr, buf);
    }
    if (priv->xendConfigVersion < 3)
        mapstr[strlen(mapstr) - 1] = ']';
    else
        mapstr[strlen(mapstr) - 1] = 0;

    snprintf(buf, sizeof(buf), "%d", vcpu);

    ret = xend_op(domain->conn, domain->name, "op", "pincpu", "vcpu", buf,
                  "cpumap", mapstr, NULL);

    if (!(def = xenDaemonDomainFetch(domain->conn,
                                     domain->id,
                                     domain->name,
                                     NULL)))
        goto cleanup;

    if (ret == 0) {
        if (virDomainVcpuPinAdd(def, cpumap, maplen, vcpu) < 0) {
            virXendError(VIR_ERR_INTERNAL_ERROR,
                         "%s", _("failed to add vcpupin xml entry"));
            return (-1);
        }
    }

    return ret;

cleanup:
    virDomainDefFree(def);
    return -1;
}

/**
 * xenDaemonDomainGetVcpusFlags:
 * @domain: pointer to domain object
 * @flags: bitwise-ORd from virDomainVcpuFlags
 *
 * Extract information about virtual CPUs of domain according to flags.
 *
 * Returns the number of vcpus on success, -1 if an error message was
 * issued, and -2 if the unified driver should keep trying.

 */
int
xenDaemonDomainGetVcpusFlags(virDomainPtr domain, unsigned int flags)
{
    struct sexpr *root;
    int ret;
    xenUnifiedPrivatePtr priv;

    virCheckFlags(VIR_DOMAIN_VCPU_LIVE |
                  VIR_DOMAIN_VCPU_CONFIG |
                  VIR_DOMAIN_VCPU_MAXIMUM, -1);

    if (domain == NULL || domain->conn == NULL || domain->name == NULL) {
        virXendError(VIR_ERR_INVALID_ARG, __FUNCTION__);
        return -1;
    }

    priv = (xenUnifiedPrivatePtr) domain->conn->privateData;

    /* If xendConfigVersion is 2, then we can only report _LIVE (and
     * xm_internal reports _CONFIG).  If it is 3, then _LIVE and
     * _CONFIG are always in sync for a running system.  */
    if (domain->id < 0 && priv->xendConfigVersion < 3)
        return -2;
    if (domain->id < 0 && (flags & VIR_DOMAIN_VCPU_LIVE)) {
        virXendError(VIR_ERR_OPERATION_INVALID, "%s",
                     _("domain not active"));
        return -1;
    }

    root = sexpr_get(domain->conn, "/xend/domain/%s?detail=1", domain->name);
    if (root == NULL)
        return -1;

    ret = sexpr_int(root, "domain/vcpus");
    if (!(flags & VIR_DOMAIN_VCPU_MAXIMUM)) {
        int vcpus = count_one_bits_l(sexpr_u64(root, "domain/vcpu_avail"));
        if (vcpus)
            ret = MIN(vcpus, ret);
    }
    if (!ret)
        ret = -2;
    sexpr_free(root);
    return ret;
}

/**
 * virDomainGetVcpus:
 * @domain: pointer to domain object, or NULL for Domain0
 * @info: pointer to an array of virVcpuInfo structures (OUT)
 * @maxinfo: number of structures in info array
 * @cpumaps: pointer to an bit map of real CPUs for all vcpus of this domain (in 8-bit bytes) (OUT)
 *	If cpumaps is NULL, then no cpumap information is returned by the API.
 *	It's assumed there is <maxinfo> cpumap in cpumaps array.
 *	The memory allocated to cpumaps must be (maxinfo * maplen) bytes
 *	(ie: calloc(maxinfo, maplen)).
 *	One cpumap inside cpumaps has the format described in virDomainPinVcpu() API.
 * @maplen: number of bytes in one cpumap, from 1 up to size of CPU map in
 *	underlying virtualization system (Xen...).
 *
 * Extract information about virtual CPUs of domain, store it in info array
 * and also in cpumaps if this pointer isn't NULL.
 *
 * Returns the number of info filled in case of success, -1 in case of failure.
 */
int
xenDaemonDomainGetVcpus(virDomainPtr domain, virVcpuInfoPtr info, int maxinfo,
                        unsigned char *cpumaps, int maplen)
{
    struct sexpr *root, *s, *t;
    virVcpuInfoPtr ipt = info;
    int nbinfo = 0, oln;
    unsigned char *cpumap;
    int vcpu, cpu;

    if ((domain == NULL) || (domain->conn == NULL) || (domain->name == NULL)
        || (info == NULL) || (maxinfo < 1)) {
        virXendError(VIR_ERR_INVALID_ARG, __FUNCTION__);
        return (-1);
    }
    if (cpumaps != NULL && maplen < 1) {
        virXendError(VIR_ERR_INVALID_ARG, __FUNCTION__);
        return (-1);
    }

    root = sexpr_get(domain->conn, "/xend/domain/%s?op=vcpuinfo", domain->name);
    if (root == NULL)
        return (-1);

    if (cpumaps != NULL)
        memset(cpumaps, 0, maxinfo * maplen);

    /* scan the sexprs from "(vcpu (number x)...)" and get parameter values */
    for (s = root; s->kind == SEXPR_CONS; s = s->u.s.cdr) {
        if ((s->u.s.car->kind == SEXPR_CONS) &&
            (s->u.s.car->u.s.car->kind == SEXPR_VALUE) &&
            STREQ(s->u.s.car->u.s.car->u.value, "vcpu")) {
            t = s->u.s.car;
            vcpu = ipt->number = sexpr_int(t, "vcpu/number");
            if ((oln = sexpr_int(t, "vcpu/online")) != 0) {
                if (sexpr_int(t, "vcpu/running")) ipt->state = VIR_VCPU_RUNNING;
                if (sexpr_int(t, "vcpu/blocked")) ipt->state = VIR_VCPU_BLOCKED;
            }
            else
                ipt->state = VIR_VCPU_OFFLINE;
            ipt->cpuTime = sexpr_float(t, "vcpu/cpu_time") * 1000000000;
            ipt->cpu = oln ? sexpr_int(t, "vcpu/cpu") : -1;

            if (cpumaps != NULL && vcpu >= 0 && vcpu < maxinfo) {
                cpumap = (unsigned char *) VIR_GET_CPUMAP(cpumaps, maplen, vcpu);
                /*
                 * get sexpr from "(cpumap (x y z...))" and convert values
                 * to bitmap
                 */
                for (t = t->u.s.cdr; t->kind == SEXPR_CONS; t = t->u.s.cdr)
                    if ((t->u.s.car->kind == SEXPR_CONS) &&
                        (t->u.s.car->u.s.car->kind == SEXPR_VALUE) &&
                        STREQ(t->u.s.car->u.s.car->u.value, "cpumap") &&
                        (t->u.s.car->u.s.cdr->kind == SEXPR_CONS)) {
                        for (t = t->u.s.car->u.s.cdr->u.s.car; t->kind == SEXPR_CONS; t = t->u.s.cdr)
                            if (t->u.s.car->kind == SEXPR_VALUE
                                && virStrToLong_i(t->u.s.car->u.value, NULL, 10, &cpu) == 0
                                && cpu >= 0
                                && (VIR_CPU_MAPLEN(cpu+1) <= maplen)) {
                                VIR_USE_CPU(cpumap, cpu);
                            }
                        break;
                    }
            }

            if (++nbinfo == maxinfo) break;
            ipt++;
        }
    }
    sexpr_free(root);
    return(nbinfo);
}

/**
 * xenDaemonLookupByUUID:
 * @conn: pointer to the hypervisor connection
 * @uuid: the raw UUID for the domain
 *
 * Try to lookup a domain on xend based on its UUID.
 *
 * Returns a new domain object or NULL in case of failure
 */
virDomainPtr
xenDaemonLookupByUUID(virConnectPtr conn, const unsigned char *uuid)
{
    virDomainPtr ret;
    char *name = NULL;
    int id = -1;
    xenUnifiedPrivatePtr priv = (xenUnifiedPrivatePtr) conn->privateData;

    /* Old approach for xen <= 3.0.3 */
    if (priv->xendConfigVersion < 3) {
        char **names, **tmp;
        unsigned char ident[VIR_UUID_BUFLEN];
        names = xenDaemonListDomainsOld(conn);
        tmp = names;

        if (names == NULL) {
            return (NULL);
        }
        while (*tmp != NULL) {
            id = xenDaemonDomainLookupByName_ids(conn, *tmp, &ident[0]);
            if (id >= 0) {
                if (!memcmp(uuid, ident, VIR_UUID_BUFLEN)) {
                    name = strdup(*tmp);

                    if (name == NULL)
                        virReportOOMError();

                    break;
                }
            }
            tmp++;
        }
        VIR_FREE(names);
    } else { /* New approach for xen >= 3.0.4 */
        char *domname = NULL;
        char uuidstr[VIR_UUID_STRING_BUFLEN];
        struct sexpr *root = NULL;

        virUUIDFormat(uuid, uuidstr);
        root = sexpr_get(conn, "/xend/domain/%s?detail=1", uuidstr);
        if (root == NULL)
            return (NULL);
        domname = (char*)sexpr_node(root, "domain/name");
        if (sexpr_node(root, "domain/domid")) /* only active domains have domid */
            id = sexpr_int(root, "domain/domid");
        else
            id = -1;

        if (domname) {
            name = strdup(domname);

            if (name == NULL)
                virReportOOMError();
        }

        sexpr_free(root);
    }

    if (name == NULL)
        return (NULL);

    ret = virGetDomain(conn, name, uuid);
    if (ret == NULL) goto cleanup;

    ret->id = id;

  cleanup:
    VIR_FREE(name);
    return (ret);
}

/**
 * xenDaemonCreateXML:
 * @conn: pointer to the hypervisor connection
 * @xmlDesc: an XML description of the domain
 * @flags: an optional set of virDomainFlags
 *
 * Launch a new Linux guest domain, based on an XML description similar
 * to the one returned by virDomainGetXMLDesc()
 * This function may requires privileged access to the hypervisor.
 *
 * Returns a new domain object or NULL in case of failure
 */
virDomainPtr
xenDaemonCreateXML(virConnectPtr conn, const char *xmlDesc,
                     unsigned int flags)
{
    int ret;
    char *sexpr;
    virDomainPtr dom = NULL;
    xenUnifiedPrivatePtr priv;
    virDomainDefPtr def;

    virCheckFlags(0, NULL);

    priv = (xenUnifiedPrivatePtr) conn->privateData;

    if (!(def = virDomainDefParseString(priv->caps, xmlDesc,
                                        1 << VIR_DOMAIN_VIRT_XEN,
                                        VIR_DOMAIN_XML_INACTIVE)))
        return (NULL);

    if (!(sexpr = xenFormatSxpr(conn, def, priv->xendConfigVersion))) {
        virDomainDefFree(def);
        return (NULL);
    }

    ret = xenDaemonDomainCreateXML(conn, sexpr);
    VIR_FREE(sexpr);
    if (ret != 0) {
        goto error;
    }

    /* This comes before wait_for_devices, to ensure that latter
       cleanup will destroy the domain upon failure */
    if (!(dom = virDomainLookupByName(conn, def->name)))
        goto error;

    if (xend_wait_for_devices(conn, def->name) < 0)
        goto error;

    if (xenDaemonDomainResume(dom) < 0)
        goto error;

    virDomainDefFree(def);
    return (dom);

  error:
    /* Make sure we don't leave a still-born domain around */
    if (dom != NULL) {
        xenDaemonDomainDestroyFlags(dom, 0);
        virUnrefDomain(dom);
    }
    virDomainDefFree(def);
    return (NULL);
}

/**
 * xenDaemonAttachDeviceFlags:
 * @domain: pointer to domain object
 * @xml: pointer to XML description of device
 * @flags: an OR'ed set of virDomainDeviceModifyFlags
 *
 * Create a virtual device attachment to backend.
 * XML description is translated into S-expression.
 *
 * Returns 0 in case of success, -1 in case of failure.
 */
static int
xenDaemonAttachDeviceFlags(virDomainPtr domain, const char *xml,
                           unsigned int flags)
{
    xenUnifiedPrivatePtr priv;
    char *sexpr = NULL;
    int ret = -1;
    virDomainDeviceDefPtr dev = NULL;
    virDomainDefPtr def = NULL;
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    char class[8], ref[80];
    char *target = NULL;

    virCheckFlags(VIR_DOMAIN_AFFECT_LIVE | VIR_DOMAIN_AFFECT_CONFIG, -1);

    if ((domain == NULL) || (domain->conn == NULL) || (domain->name == NULL)) {
        virXendError(VIR_ERR_INVALID_ARG, __FUNCTION__);
        return -1;
    }

    priv = (xenUnifiedPrivatePtr) domain->conn->privateData;

    if (domain->id < 0) {
        /* Cannot modify live config if domain is inactive */
        if (flags & VIR_DOMAIN_DEVICE_MODIFY_LIVE) {
            virXendError(VIR_ERR_OPERATION_INVALID, "%s",
                         _("Cannot modify live config if domain is inactive"));
            return -1;
        }
        /* If xendConfigVersion < 3 only live config can be changed */
        if (priv->xendConfigVersion < 3) {
            virXendError(VIR_ERR_OPERATION_INVALID, "%s",
                         _("Xend version does not support modifying "
                           "persistent config"));
            return -1;
        }
    } else {
        /* Only live config can be changed if xendConfigVersion < 3 */
        if (priv->xendConfigVersion < 3 &&
            (flags != VIR_DOMAIN_DEVICE_MODIFY_CURRENT &&
             flags != VIR_DOMAIN_DEVICE_MODIFY_LIVE)) {
            virXendError(VIR_ERR_OPERATION_INVALID, "%s",
                         _("Xend version does not support modifying "
                           "persistent config"));
            return -1;
        }
        /* Xen only supports modifying both live and persistent config if
         * xendConfigVersion >= 3
         */
        if (priv->xendConfigVersion >= 3 &&
            (flags != (VIR_DOMAIN_DEVICE_MODIFY_LIVE |
                       VIR_DOMAIN_DEVICE_MODIFY_CONFIG))) {
            virXendError(VIR_ERR_OPERATION_INVALID, "%s",
                         _("Xend only supports modifying both live and "
                           "persistent config"));
            return -1;
        }
    }

    if (!(def = xenDaemonDomainFetch(domain->conn,
                                     domain->id,
                                     domain->name,
                                     NULL)))
        goto cleanup;

    if (!(dev = virDomainDeviceDefParse(priv->caps,
                                        def, xml, VIR_DOMAIN_XML_INACTIVE)))
        goto cleanup;


    switch (dev->type) {
    case VIR_DOMAIN_DEVICE_DISK:
        if (xenFormatSxprDisk(dev->data.disk,
                              &buf,
                              STREQ(def->os.type, "hvm") ? 1 : 0,
                              priv->xendConfigVersion, 1) < 0)
            goto cleanup;

        if (dev->data.disk->device != VIR_DOMAIN_DISK_DEVICE_CDROM) {
            if (!(target = strdup(dev->data.disk->dst))) {
                virReportOOMError();
                goto cleanup;
            }
        }
        break;

    case VIR_DOMAIN_DEVICE_NET:
        if (xenFormatSxprNet(domain->conn,
                             dev->data.net,
                             &buf,
                             STREQ(def->os.type, "hvm") ? 1 : 0,
                             priv->xendConfigVersion, 1) < 0)
            goto cleanup;

        char macStr[VIR_MAC_STRING_BUFLEN];
        virFormatMacAddr(dev->data.net->mac, macStr);

        if (!(target = strdup(macStr))) {
            virReportOOMError();
            goto cleanup;
        }
        break;

    case VIR_DOMAIN_DEVICE_HOSTDEV:
        if (dev->data.hostdev->mode == VIR_DOMAIN_HOSTDEV_MODE_SUBSYS &&
            dev->data.hostdev->source.subsys.type == VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_PCI) {
            if (xenFormatSxprOnePCI(dev->data.hostdev, &buf, 0) < 0)
                goto cleanup;

            virDomainDevicePCIAddress PCIAddr;

            PCIAddr = dev->data.hostdev->source.subsys.u.pci;
            virAsprintf(&target, "PCI device: %.4x:%.2x:%.2x", PCIAddr.domain,
                                 PCIAddr.bus, PCIAddr.slot);

            if (target == NULL) {
                virReportOOMError();
                goto cleanup;
            }
        } else {
            virXendError(VIR_ERR_NO_SUPPORT, "%s",
                         _("unsupported device type"));
            goto cleanup;
        }
        break;

    default:
        virXendError(VIR_ERR_NO_SUPPORT, "%s",
                     _("unsupported device type"));
        goto cleanup;
    }

    sexpr = virBufferContentAndReset(&buf);

    if (virDomainXMLDevID(domain, dev, class, ref, sizeof(ref))) {
        /* device doesn't exist, define it */
        ret = xend_op(domain->conn, domain->name, "op", "device_create",
                      "config", sexpr, NULL);
    } else {
        if (dev->data.disk->device != VIR_DOMAIN_DISK_DEVICE_CDROM) {
            virXendError(VIR_ERR_OPERATION_INVALID,
                         _("target '%s' already exists"), target);
        } else {
            /* device exists, attempt to modify it */
            ret = xend_op(domain->conn, domain->name, "op", "device_configure",
                          "config", sexpr, "dev", ref, NULL);
        }
    }

cleanup:
    VIR_FREE(sexpr);
    virDomainDefFree(def);
    virDomainDeviceDefFree(dev);
    VIR_FREE(target);
    return ret;
}

/**
 * xenDaemonUpdateDeviceFlags:
 * @domain: pointer to domain object
 * @xml: pointer to XML description of device
 * @flags: an OR'ed set of virDomainDeviceModifyFlags
 *
 * Create a virtual device attachment to backend.
 * XML description is translated into S-expression.
 *
 * Returns 0 in case of success, -1 in case of failure.
 */
int
xenDaemonUpdateDeviceFlags(virDomainPtr domain, const char *xml,
                           unsigned int flags)
{
    xenUnifiedPrivatePtr priv;
    char *sexpr = NULL;
    int ret = -1;
    virDomainDeviceDefPtr dev = NULL;
    virDomainDefPtr def = NULL;
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    char class[8], ref[80];

    virCheckFlags(VIR_DOMAIN_DEVICE_MODIFY_LIVE |
                  VIR_DOMAIN_DEVICE_MODIFY_CONFIG, -1);

    if ((domain == NULL) || (domain->conn == NULL) || (domain->name == NULL)) {
        virXendError(VIR_ERR_INVALID_ARG, __FUNCTION__);
        return -1;
    }

    priv = (xenUnifiedPrivatePtr) domain->conn->privateData;

    if (domain->id < 0) {
        /* Cannot modify live config if domain is inactive */
        if (flags & VIR_DOMAIN_DEVICE_MODIFY_LIVE) {
            virXendError(VIR_ERR_OPERATION_INVALID, "%s",
                         _("Cannot modify live config if domain is inactive"));
            return -1;
        }
        /* If xendConfigVersion < 3 only live config can be changed */
        if (priv->xendConfigVersion < 3) {
            virXendError(VIR_ERR_OPERATION_INVALID, "%s",
                         _("Xend version does not support modifying "
                           "persistent config"));
            return -1;
        }
    } else {
        /* Only live config can be changed if xendConfigVersion < 3 */
        if (priv->xendConfigVersion < 3 &&
            (flags != VIR_DOMAIN_DEVICE_MODIFY_CURRENT &&
             flags != VIR_DOMAIN_DEVICE_MODIFY_LIVE)) {
            virXendError(VIR_ERR_OPERATION_INVALID, "%s",
                         _("Xend version does not support modifying "
                           "persistent config"));
            return -1;
        }
        /* Xen only supports modifying both live and persistent config if
         * xendConfigVersion >= 3
         */
        if (priv->xendConfigVersion >= 3 &&
            (flags != (VIR_DOMAIN_DEVICE_MODIFY_LIVE |
                       VIR_DOMAIN_DEVICE_MODIFY_CONFIG))) {
            virXendError(VIR_ERR_OPERATION_INVALID, "%s",
                         _("Xend only supports modifying both live and "
                           "persistent config"));
            return -1;
        }
    }

    if (!(def = xenDaemonDomainFetch(domain->conn,
                                     domain->id,
                                     domain->name,
                                     NULL)))
        goto cleanup;

    if (!(dev = virDomainDeviceDefParse(priv->caps,
                                        def, xml, VIR_DOMAIN_XML_INACTIVE)))
        goto cleanup;


    switch (dev->type) {
    case VIR_DOMAIN_DEVICE_DISK:
        if (xenFormatSxprDisk(dev->data.disk,
                              &buf,
                              STREQ(def->os.type, "hvm") ? 1 : 0,
                              priv->xendConfigVersion, 1) < 0)
            goto cleanup;
        break;

    default:
        virXendError(VIR_ERR_NO_SUPPORT, "%s",
                     _("unsupported device type"));
        goto cleanup;
    }

    sexpr = virBufferContentAndReset(&buf);

    if (virDomainXMLDevID(domain, dev, class, ref, sizeof(ref))) {
        virXendError(VIR_ERR_OPERATION_INVALID, "%s",
                     _("requested device does not exist"));
        goto cleanup;
    } else {
        /* device exists, attempt to modify it */
        ret = xend_op(domain->conn, domain->name, "op", "device_configure",
                      "config", sexpr, "dev", ref, NULL);
    }

cleanup:
    VIR_FREE(sexpr);
    virDomainDefFree(def);
    virDomainDeviceDefFree(dev);
    return ret;
}

/**
 * xenDaemonDetachDeviceFlags:
 * @domain: pointer to domain object
 * @xml: pointer to XML description of device
 * @flags: an OR'ed set of virDomainDeviceModifyFlags
 *
 * Destroy a virtual device attachment to backend.
 *
 * Returns 0 in case of success, -1 in case of failure.
 */
static int
xenDaemonDetachDeviceFlags(virDomainPtr domain, const char *xml,
                           unsigned int flags)
{
    xenUnifiedPrivatePtr priv;
    char class[8], ref[80];
    virDomainDeviceDefPtr dev = NULL;
    virDomainDefPtr def = NULL;
    int ret = -1;
    char *xendev = NULL;
    virBuffer buf = VIR_BUFFER_INITIALIZER;

    virCheckFlags(VIR_DOMAIN_AFFECT_LIVE | VIR_DOMAIN_AFFECT_CONFIG, -1);

    if ((domain == NULL) || (domain->conn == NULL) || (domain->name == NULL)) {
        virXendError(VIR_ERR_INVALID_ARG, __FUNCTION__);
        return (-1);
    }

    priv = (xenUnifiedPrivatePtr) domain->conn->privateData;

    if (domain->id < 0) {
        /* Cannot modify live config if domain is inactive */
        if (flags & VIR_DOMAIN_DEVICE_MODIFY_LIVE) {
            virXendError(VIR_ERR_OPERATION_INVALID, "%s",
                         _("Cannot modify live config if domain is inactive"));
            return -1;
        }
        /* If xendConfigVersion < 3 only live config can be changed */
        if (priv->xendConfigVersion < 3) {
            virXendError(VIR_ERR_OPERATION_INVALID, "%s",
                         _("Xend version does not support modifying "
                           "persistent config"));
            return -1;
        }
    } else {
        /* Only live config can be changed if xendConfigVersion < 3 */
        if (priv->xendConfigVersion < 3 &&
            (flags != VIR_DOMAIN_DEVICE_MODIFY_CURRENT &&
             flags != VIR_DOMAIN_DEVICE_MODIFY_LIVE)) {
            virXendError(VIR_ERR_OPERATION_INVALID, "%s",
                         _("Xend version does not support modifying "
                           "persistent config"));
            return -1;
        }
        /* Xen only supports modifying both live and persistent config if
         * xendConfigVersion >= 3
         */
        if (priv->xendConfigVersion >= 3 &&
            (flags != (VIR_DOMAIN_DEVICE_MODIFY_LIVE |
                       VIR_DOMAIN_DEVICE_MODIFY_CONFIG))) {
            virXendError(VIR_ERR_OPERATION_INVALID, "%s",
                         _("Xend only supports modifying both live and "
                           "persistent config"));
            return -1;
        }
    }

    if (!(def = xenDaemonDomainFetch(domain->conn,
                                     domain->id,
                                     domain->name,
                                     NULL)))
        goto cleanup;

    if (!(dev = virDomainDeviceDefParse(priv->caps,
                                        def, xml, VIR_DOMAIN_XML_INACTIVE)))
        goto cleanup;

    if (virDomainXMLDevID(domain, dev, class, ref, sizeof(ref)))
        goto cleanup;

    if (dev->type == VIR_DOMAIN_DEVICE_HOSTDEV) {
        if (dev->data.hostdev->mode == VIR_DOMAIN_HOSTDEV_MODE_SUBSYS &&
            dev->data.hostdev->source.subsys.type == VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_PCI) {
            if (xenFormatSxprOnePCI(dev->data.hostdev, &buf, 1) < 0)
                goto cleanup;
        } else {
            virXendError(VIR_ERR_NO_SUPPORT, "%s",
                         _("unsupported device type"));
            goto cleanup;
        }
        xendev = virBufferContentAndReset(&buf);
        ret = xend_op(domain->conn, domain->name, "op", "device_configure",
                      "config", xendev, "dev", ref, NULL);
        VIR_FREE(xendev);
    }
    else {
        ret = xend_op(domain->conn, domain->name, "op", "device_destroy",
                      "type", class, "dev", ref, "force", "0", "rm_cfg", "1",
                      NULL);
    }

cleanup:
    virDomainDefFree(def);
    virDomainDeviceDefFree(dev);

    return ret;
}

int
xenDaemonDomainGetAutostart(virDomainPtr domain,
                            int *autostart)
{
    struct sexpr *root;
    const char *tmp;
    xenUnifiedPrivatePtr priv;

    if ((domain == NULL) || (domain->conn == NULL) || (domain->name == NULL)) {
        virXendError(VIR_ERR_INVALID_ARG, __FUNCTION__);
        return (-1);
    }

    /* xm_internal.c (the support for defined domains from /etc/xen
     * config files used by old Xen) will handle this.
     */
    priv = (xenUnifiedPrivatePtr) domain->conn->privateData;
    if (priv->xendConfigVersion < 3)
        return(-1);

    root = sexpr_get(domain->conn, "/xend/domain/%s?detail=1", domain->name);
    if (root == NULL) {
        virXendError(VIR_ERR_XEN_CALL,
                      "%s", _("xenDaemonGetAutostart failed to find this domain"));
        return (-1);
    }

    *autostart = 0;

    tmp = sexpr_node(root, "domain/on_xend_start");
    if (tmp && STREQ(tmp, "start")) {
        *autostart = 1;
    }

    sexpr_free(root);
    return 0;
}

int
xenDaemonDomainSetAutostart(virDomainPtr domain,
                            int autostart)
{
    struct sexpr *root, *autonode;
    virBuffer buffer = VIR_BUFFER_INITIALIZER;
    char *content = NULL;
    int ret = -1;
    xenUnifiedPrivatePtr priv;

    if ((domain == NULL) || (domain->conn == NULL) || (domain->name == NULL)) {
        virXendError(VIR_ERR_INTERNAL_ERROR, __FUNCTION__);
        return (-1);
    }

    /* xm_internal.c (the support for defined domains from /etc/xen
     * config files used by old Xen) will handle this.
     */
    priv = (xenUnifiedPrivatePtr) domain->conn->privateData;
    if (priv->xendConfigVersion < 3)
        return(-1);

    root = sexpr_get(domain->conn, "/xend/domain/%s?detail=1", domain->name);
    if (root == NULL) {
        virXendError(VIR_ERR_XEN_CALL,
                      "%s", _("xenDaemonSetAutostart failed to find this domain"));
        return (-1);
    }

    autonode = sexpr_lookup(root, "domain/on_xend_start");
    if (autonode) {
        const char *val = (autonode->u.s.car->kind == SEXPR_VALUE
                           ? autonode->u.s.car->u.value : NULL);
        if (!val || (!STREQ(val, "ignore") && !STREQ(val, "start"))) {
            virXendError(VIR_ERR_INTERNAL_ERROR,
                         "%s", _("unexpected value from on_xend_start"));
            goto error;
        }

        /* Change the autostart value in place, then define the new sexpr */
        VIR_FREE(autonode->u.s.car->u.value);
        autonode->u.s.car->u.value = (autostart ? strdup("start")
                                                : strdup("ignore"));
        if (!(autonode->u.s.car->u.value)) {
            virReportOOMError();
            goto error;
        }

        if (sexpr2string(root, &buffer) < 0) {
            virXendError(VIR_ERR_INTERNAL_ERROR,
                         "%s", _("sexpr2string failed"));
            goto error;
        }

        if (virBufferError(&buffer)) {
            virReportOOMError();
            goto error;
        }

        content = virBufferContentAndReset(&buffer);

        if (xend_op(domain->conn, "", "op", "new", "config", content, NULL) != 0) {
            virXendError(VIR_ERR_XEN_CALL,
                         "%s", _("Failed to redefine sexpr"));
            goto error;
        }
    } else {
        virXendError(VIR_ERR_INTERNAL_ERROR,
                     "%s", _("on_xend_start not present in sexpr"));
        goto error;
    }

    ret = 0;
  error:
    virBufferFreeAndReset(&buffer);
    VIR_FREE(content);
    sexpr_free(root);
    return ret;
}

int
xenDaemonDomainMigratePrepare (virConnectPtr dconn,
                               char **cookie ATTRIBUTE_UNUSED,
                               int *cookielen ATTRIBUTE_UNUSED,
                               const char *uri_in,
                               char **uri_out,
                               unsigned long flags,
                               const char *dname ATTRIBUTE_UNUSED,
                               unsigned long resource ATTRIBUTE_UNUSED)
{
    virCheckFlags(XEN_MIGRATION_FLAGS, -1);

    /* If uri_in is NULL, get the current hostname as a best guess
     * of how the source host should connect to us.  Note that caller
     * deallocates this string.
     */
    if (uri_in == NULL) {
        *uri_out = virGetHostname(dconn);
        if (*uri_out == NULL)
            return -1;
    }

    return 0;
}

int
xenDaemonDomainMigratePerform (virDomainPtr domain,
                               const char *cookie ATTRIBUTE_UNUSED,
                               int cookielen ATTRIBUTE_UNUSED,
                               const char *uri,
                               unsigned long flags,
                               const char *dname,
                               unsigned long bandwidth)
{
    /* Upper layers have already checked domain. */
    /* NB: Passing port=0 to xend means it ignores
     * the port.  However this is somewhat specific to
     * the internals of the xend Python code. (XXX).
     */
    char port[16] = "0";
    char live[2] = "0";
    int ret;
    char *p, *hostname = NULL;

    int undefined_source = 0;

    virCheckFlags(XEN_MIGRATION_FLAGS, -1);

    /* Xen doesn't support renaming domains during migration. */
    if (dname) {
        virXendError(VIR_ERR_NO_SUPPORT,
                      "%s", _("xenDaemonDomainMigrate: Xen does not support"
                        " renaming domains during migration"));
        return -1;
    }

    /* Xen (at least up to 3.1.0) takes a resource parameter but
     * ignores it.
     */
    if (bandwidth) {
        virXendError(VIR_ERR_NO_SUPPORT,
                      "%s", _("xenDaemonDomainMigrate: Xen does not support"
                        " bandwidth limits during migration"));
        return -1;
    }

    /*
     * Check the flags.
     */
    if ((flags & VIR_MIGRATE_LIVE)) {
        strcpy (live, "1");
        flags &= ~VIR_MIGRATE_LIVE;
    }

    /* Undefine the VM on the source host after migration? */
    if (flags & VIR_MIGRATE_UNDEFINE_SOURCE) {
       undefined_source = 1;
       flags &= ~VIR_MIGRATE_UNDEFINE_SOURCE;
    }

    /* Ignore the persist_dest flag here */
    if (flags & VIR_MIGRATE_PERSIST_DEST)
        flags &= ~VIR_MIGRATE_PERSIST_DEST;

    /* This is buggy in Xend, but could be supported in principle.  Give
     * a nice error message.
     */
    if (flags & VIR_MIGRATE_PAUSED) {
        virXendError(VIR_ERR_NO_SUPPORT,
                      "%s", _("xenDaemonDomainMigrate: xend cannot migrate paused domains"));
        return -1;
    }

    /* XXX we could easily do tunnelled & peer2peer migration too
       if we want to. support these... */
    if (flags != 0) {
        virXendError(VIR_ERR_NO_SUPPORT,
                      "%s", _("xenDaemonDomainMigrate: unsupported flag"));
        return -1;
    }

    /* Set hostname and port.
     *
     * URI is non-NULL (guaranteed by caller).  We expect either
     * "hostname", "hostname:port" or "xenmigr://hostname[:port]/".
     */
    if (strstr (uri, "//")) {   /* Full URI. */
        xmlURIPtr uriptr = xmlParseURI (uri);
        if (!uriptr) {
            virXendError(VIR_ERR_INVALID_ARG,
                          "%s", _("xenDaemonDomainMigrate: invalid URI"));
            return -1;
        }
        if (uriptr->scheme && STRCASENEQ (uriptr->scheme, "xenmigr")) {
            virXendError(VIR_ERR_INVALID_ARG,
                          "%s", _("xenDaemonDomainMigrate: only xenmigr://"
                            " migrations are supported by Xen"));
            xmlFreeURI (uriptr);
            return -1;
        }
        if (!uriptr->server) {
            virXendError(VIR_ERR_INVALID_ARG,
                          "%s", _("xenDaemonDomainMigrate: a hostname must be"
                            " specified in the URI"));
            xmlFreeURI (uriptr);
            return -1;
        }
        hostname = strdup (uriptr->server);
        if (!hostname) {
            virReportOOMError();
            xmlFreeURI (uriptr);
            return -1;
        }
        if (uriptr->port)
            snprintf (port, sizeof port, "%d", uriptr->port);
        xmlFreeURI (uriptr);
    }
    else if ((p = strrchr (uri, ':')) != NULL) { /* "hostname:port" */
        int port_nr, n;

        if (virStrToLong_i(p+1, NULL, 10, &port_nr) < 0) {
            virXendError(VIR_ERR_INVALID_ARG,
                          "%s", _("xenDaemonDomainMigrate: invalid port number"));
            return -1;
        }
        snprintf (port, sizeof port, "%d", port_nr);

        /* Get the hostname. */
        n = p - uri; /* n = Length of hostname in bytes. */
        hostname = strdup (uri);
        if (!hostname) {
            virReportOOMError();
            return -1;
        }
        hostname[n] = '\0';
    }
    else {                      /* "hostname" (or IP address) */
        hostname = strdup (uri);
        if (!hostname) {
            virReportOOMError();
            return -1;
        }
    }

    VIR_DEBUG("hostname = %s, port = %s", hostname, port);

    /* Make the call.
     * NB:  xend will fail the operation if any parameters are
     * missing but happily accept unknown parameters.  This works
     * to our advantage since all parameters supported and required
     * by current xend can be included without breaking older xend.
     */
    ret = xend_op (domain->conn, domain->name,
                   "op", "migrate",
                   "destination", hostname,
                   "live", live,
                   "port", port,
                   "node", "-1", /* xen-unstable c/s 17753 */
                   "ssl", "0", /* xen-unstable c/s 17709 */
                   "change_home_server", "0", /* xen-unstable c/s 20326 */
                   "resource", "0", /* removed by xen-unstable c/s 17553 */
                   NULL);
    VIR_FREE (hostname);

    if (ret == 0 && undefined_source)
        xenDaemonDomainUndefine (domain);

    VIR_DEBUG("migration done");

    return ret;
}

virDomainPtr xenDaemonDomainDefineXML(virConnectPtr conn, const char *xmlDesc) {
    int ret;
    char *sexpr;
    virDomainPtr dom;
    xenUnifiedPrivatePtr priv;
    virDomainDefPtr def;

    priv = (xenUnifiedPrivatePtr) conn->privateData;

    if (priv->xendConfigVersion < 3)
        return(NULL);

    if (!(def = virDomainDefParseString(priv->caps, xmlDesc,
                                        1 << VIR_DOMAIN_VIRT_XEN,
                                        VIR_DOMAIN_XML_INACTIVE))) {
        virXendError(VIR_ERR_XML_ERROR,
                     "%s", _("failed to parse domain description"));
        return (NULL);
    }

    if (!(sexpr = xenFormatSxpr(conn, def, priv->xendConfigVersion))) {
        virXendError(VIR_ERR_XML_ERROR,
                     "%s", _("failed to build sexpr"));
        goto error;
    }

    ret = xend_op(conn, "", "op", "new", "config", sexpr, NULL);
    VIR_FREE(sexpr);
    if (ret != 0) {
        virXendError(VIR_ERR_XEN_CALL,
                     _("Failed to create inactive domain %s"), def->name);
        goto error;
    }

    dom = virDomainLookupByName(conn, def->name);
    if (dom == NULL) {
        goto error;
    }
    virDomainDefFree(def);
    return (dom);

  error:
    virDomainDefFree(def);
    return (NULL);
}
int xenDaemonDomainCreate(virDomainPtr domain)
{
    xenUnifiedPrivatePtr priv;
    int ret;
    virDomainPtr tmp;

    if ((domain == NULL) || (domain->conn == NULL) || (domain->name == NULL)) {
        virXendError(VIR_ERR_INVALID_ARG, __FUNCTION__);
        return(-1);
    }

    priv = (xenUnifiedPrivatePtr) domain->conn->privateData;

    if (priv->xendConfigVersion < 3)
        return(-1);

    ret = xend_op(domain->conn, domain->name, "op", "start", NULL);

    if (ret != -1) {
        /* Need to force a refresh of this object's ID */
        tmp = virDomainLookupByName(domain->conn, domain->name);
        if (tmp) {
            domain->id = tmp->id;
            virDomainFree(tmp);
        }
    }
    return ret;
}

int xenDaemonDomainUndefine(virDomainPtr domain)
{
    xenUnifiedPrivatePtr priv;

    if ((domain == NULL) || (domain->conn == NULL) || (domain->name == NULL)) {
        virXendError(VIR_ERR_INVALID_ARG, __FUNCTION__);
        return(-1);
    }

    priv = (xenUnifiedPrivatePtr) domain->conn->privateData;

    if (priv->xendConfigVersion < 3)
        return(-1);

    return xend_op(domain->conn, domain->name, "op", "delete", NULL);
}

/**
 * xenDaemonNumOfDomains:
 * @conn: pointer to the hypervisor connection
 *
 * Provides the number of active domains.
 *
 * Returns the number of domain found or -1 in case of error
 */
static int
xenDaemonNumOfDefinedDomains(virConnectPtr conn)
{
    struct sexpr *root = NULL;
    int ret = -1;
    struct sexpr *_for_i, *node;
    xenUnifiedPrivatePtr priv = (xenUnifiedPrivatePtr) conn->privateData;

    /* xm_internal.c (the support for defined domains from /etc/xen
     * config files used by old Xen) will handle this.
     */
    if (priv->xendConfigVersion < 3)
        return(-1);

    root = sexpr_get(conn, "/xend/domain?state=halted");
    if (root == NULL)
        goto error;

    ret = 0;

    for (_for_i = root, node = root->u.s.car; _for_i->kind == SEXPR_CONS;
         _for_i = _for_i->u.s.cdr, node = _for_i->u.s.car) {
        if (node->kind != SEXPR_VALUE)
            continue;
        ret++;
    }

error:
    sexpr_free(root);
    return(ret);
}

static int
xenDaemonListDefinedDomains(virConnectPtr conn, char **const names, int maxnames) {
    struct sexpr *root = NULL;
    int i, ret = -1;
    struct sexpr *_for_i, *node;
    xenUnifiedPrivatePtr priv = (xenUnifiedPrivatePtr) conn->privateData;

    if (priv->xendConfigVersion < 3)
        return(-1);

    if ((names == NULL) || (maxnames < 0))
        goto error;
    if (maxnames == 0)
        return(0);

    root = sexpr_get(conn, "/xend/domain?state=halted");
    if (root == NULL)
        goto error;

    ret = 0;

    for (_for_i = root, node = root->u.s.car; _for_i->kind == SEXPR_CONS;
         _for_i = _for_i->u.s.cdr, node = _for_i->u.s.car) {
        if (node->kind != SEXPR_VALUE)
            continue;

        if ((names[ret++] = strdup(node->u.value)) == NULL) {
            virReportOOMError();
            goto error;
        }

        if (ret >= maxnames)
            break;
    }

cleanup:
    sexpr_free(root);
    return(ret);

error:
    for (i = 0; i < ret; ++i)
        VIR_FREE(names[i]);

    ret = -1;

    goto cleanup;
}

/**
 * xenDaemonGetSchedulerType:
 * @domain: pointer to the Domain block
 * @nparams: give a number of scheduler parameters
 *
 * Get the scheduler type of Xen
 *
 * Returns a scheduler name (credit or sedf) which must be freed by the
 * caller or NULL in case of failure
 */
static char *
xenDaemonGetSchedulerType(virDomainPtr domain, int *nparams)
{
    xenUnifiedPrivatePtr priv;
    struct sexpr *root;
    const char *ret = NULL;
    char *schedulertype = NULL;

    if (domain->conn == NULL || domain->name == NULL) {
        virXendError(VIR_ERR_INVALID_ARG, __FUNCTION__);
        return NULL;
    }

    /* Support only xendConfigVersion >=4 */
    priv = (xenUnifiedPrivatePtr) domain->conn->privateData;
    if (priv->xendConfigVersion < 4) {
        virXendError(VIR_ERR_NO_SUPPORT,
                      "%s", _("unsupported in xendConfigVersion < 4"));
        return NULL;
    }

    root = sexpr_get(domain->conn, "/xend/node/");
    if (root == NULL)
        return NULL;

    /* get xen_scheduler from xend/node */
    ret = sexpr_node(root, "node/xen_scheduler");
    if (ret == NULL){
        virXendError(VIR_ERR_INTERNAL_ERROR,
                     "%s", _("node information incomplete, missing scheduler name"));
        goto error;
    }
    if (STREQ (ret, "credit")) {
        schedulertype = strdup("credit");
        if (schedulertype == NULL){
            virReportOOMError();
            goto error;
        }
        if (nparams)
            *nparams = XEN_SCHED_CRED_NPARAM;
    } else if (STREQ (ret, "sedf")) {
        schedulertype = strdup("sedf");
        if (schedulertype == NULL){
            virReportOOMError();
            goto error;
        }
        if (nparams)
            *nparams = XEN_SCHED_SEDF_NPARAM;
    } else {
        virXendError(VIR_ERR_INTERNAL_ERROR, "%s", _("Unknown scheduler"));
        goto error;
    }

error:
    sexpr_free(root);
    return schedulertype;

}

static const char *str_weight = "weight";
static const char *str_cap = "cap";

/**
 * xenDaemonGetSchedulerParameters:
 * @domain: pointer to the Domain block
 * @params: pointer to scheduler parameters
 *          This memory area must be allocated by the caller
 * @nparams: a number of scheduler parameters which should be same as a
 *           given number from xenDaemonGetSchedulerType()
 *
 * Get the scheduler parameters
 *
 * Returns 0 or -1 in case of failure
 */
static int
xenDaemonGetSchedulerParameters(virDomainPtr domain,
                                virTypedParameterPtr params, int *nparams)
{
    xenUnifiedPrivatePtr priv;
    struct sexpr *root;
    char *sched_type = NULL;
    int sched_nparam = 0;
    int ret = -1;

    if ((domain == NULL) || (domain->conn == NULL) || (domain->name == NULL)) {
        virXendError(VIR_ERR_INVALID_ARG, __FUNCTION__);
        return (-1);
    }

    /* Support only xendConfigVersion >=4 */
    priv = (xenUnifiedPrivatePtr) domain->conn->privateData;
    if (priv->xendConfigVersion < 4) {
        virXendError(VIR_ERR_NO_SUPPORT,
                      "%s", _("unsupported in xendConfigVersion < 4"));
        return (-1);
    }

    /* look up the information by domain name */
    root = sexpr_get(domain->conn, "/xend/domain/%s?detail=1", domain->name);
    if (root == NULL)
        return (-1);

    /* get the scheduler type */
    sched_type = xenDaemonGetSchedulerType(domain, &sched_nparam);
    if (sched_type == NULL) {
        virXendError(VIR_ERR_INTERNAL_ERROR,
                     "%s", _("Failed to get a scheduler name"));
        goto error;
    }

    switch (sched_nparam){
        case XEN_SCHED_SEDF_NPARAM:
            if (*nparams < XEN_SCHED_SEDF_NPARAM) {
                virXendError(VIR_ERR_INVALID_ARG,
                             "%s", _("Invalid parameter count"));
                goto error;
            }

            /* TODO: Implement for Xen/SEDF */
            TODO
            goto error;
        case XEN_SCHED_CRED_NPARAM:
            if (*nparams < XEN_SCHED_CRED_NPARAM) {
                virXendError(VIR_ERR_INVALID_ARG,
                             "%s", _("Invalid parameter count"));
                goto error;
            }

            /* get cpu_weight/cpu_cap from xend/domain */
            if (sexpr_node(root, "domain/cpu_weight") == NULL) {
                virXendError(VIR_ERR_INTERNAL_ERROR,
                        "%s", _("domain information incomplete, missing cpu_weight"));
                goto error;
            }
            if (sexpr_node(root, "domain/cpu_cap") == NULL) {
                virXendError(VIR_ERR_INTERNAL_ERROR,
                        "%s", _("domain information incomplete, missing cpu_cap"));
                goto error;
            }

            if (virStrcpyStatic(params[0].field, str_weight) == NULL) {
                virXendError(VIR_ERR_INTERNAL_ERROR,
                             _("Weight %s too big for destination"),
                             str_weight);
                goto error;
            }
            params[0].type = VIR_TYPED_PARAM_UINT;
            params[0].value.ui = sexpr_int(root, "domain/cpu_weight");

            if (virStrcpyStatic(params[1].field, str_cap) == NULL) {
                virXendError(VIR_ERR_INTERNAL_ERROR,
                             _("Cap %s too big for destination"), str_cap);
                goto error;
            }
            params[1].type = VIR_TYPED_PARAM_UINT;
            params[1].value.ui = sexpr_int(root, "domain/cpu_cap");
            *nparams = XEN_SCHED_CRED_NPARAM;
            ret = 0;
            break;
        default:
            virXendError(VIR_ERR_INTERNAL_ERROR, "%s", _("Unknown scheduler"));
            goto error;
    }

error:
    sexpr_free(root);
    VIR_FREE(sched_type);
    return (ret);
}

/**
 * xenDaemonSetSchedulerParameters:
 * @domain: pointer to the Domain block
 * @params: pointer to scheduler parameters
 * @nparams: a number of scheduler setting parameters
 *
 * Set the scheduler parameters
 *
 * Returns 0 or -1 in case of failure
 */
static int
xenDaemonSetSchedulerParameters(virDomainPtr domain,
                                virTypedParameterPtr params, int nparams)
{
    xenUnifiedPrivatePtr priv;
    struct sexpr *root;
    char *sched_type = NULL;
    int i;
    int sched_nparam = 0;
    int ret = -1;

    if ((domain == NULL) || (domain->conn == NULL) || (domain->name == NULL)) {
        virXendError(VIR_ERR_INVALID_ARG, __FUNCTION__);
        return (-1);
    }

    /* Support only xendConfigVersion >=4 and active domains */
    priv = (xenUnifiedPrivatePtr) domain->conn->privateData;
    if (priv->xendConfigVersion < 4) {
        virXendError(VIR_ERR_NO_SUPPORT,
                      "%s", _("unsupported in xendConfigVersion < 4"));
        return (-1);
    }

    /* look up the information by domain name */
    root = sexpr_get(domain->conn, "/xend/domain/%s?detail=1", domain->name);
    if (root == NULL)
        return (-1);

    /* get the scheduler type */
    sched_type = xenDaemonGetSchedulerType(domain, &sched_nparam);
    if (sched_type == NULL) {
        virXendError(VIR_ERR_INTERNAL_ERROR,
                     "%s", _("Failed to get a scheduler name"));
        goto error;
    }

    switch (sched_nparam){
        case XEN_SCHED_SEDF_NPARAM:
            /* TODO: Implement for Xen/SEDF */
            TODO
            goto error;
        case XEN_SCHED_CRED_NPARAM: {
            char buf_weight[VIR_UUID_BUFLEN];
            char buf_cap[VIR_UUID_BUFLEN];
            const char *weight = NULL;
            const char *cap = NULL;

            /* get the scheduler parameters */
            memset(&buf_weight, 0, VIR_UUID_BUFLEN);
            memset(&buf_cap, 0, VIR_UUID_BUFLEN);
            for (i = 0; i < nparams; i++) {
                if (STREQ (params[i].field, str_weight) &&
                    params[i].type == VIR_TYPED_PARAM_UINT) {
                    snprintf(buf_weight, sizeof(buf_weight), "%u", params[i].value.ui);
                } else if (STREQ (params[i].field, str_cap) &&
                    params[i].type == VIR_TYPED_PARAM_UINT) {
                    snprintf(buf_cap, sizeof(buf_cap), "%u", params[i].value.ui);
                } else {
                    virXendError(VIR_ERR_INVALID_ARG, __FUNCTION__);
                    goto error;
                }
            }

            /* if not get the scheduler parameter, set the current setting */
            if (strlen(buf_weight) == 0) {
                weight = sexpr_node(root, "domain/cpu_weight");
                if (weight == NULL) {
                    virXendError(VIR_ERR_INTERNAL_ERROR,
                                "%s", _("domain information incomplete, missing cpu_weight"));
                    goto error;
                }
                snprintf(buf_weight, sizeof(buf_weight), "%s", weight);
            }
            if (strlen(buf_cap) == 0) {
                cap = sexpr_node(root, "domain/cpu_cap");
                if (cap == NULL) {
                    virXendError(VIR_ERR_INTERNAL_ERROR,
                                "%s", _("domain information incomplete, missing cpu_cap"));
                    goto error;
                }
                snprintf(buf_cap, sizeof(buf_cap), "%s", cap);
            }

            ret = xend_op(domain->conn, domain->name, "op",
                          "domain_sched_credit_set", "weight", buf_weight,
                          "cap", buf_cap, NULL);
            break;
        }
        default:
            virXendError(VIR_ERR_INTERNAL_ERROR, "%s", _("Unknown scheduler"));
            goto error;
    }

error:
    sexpr_free(root);
    VIR_FREE(sched_type);
    return (ret);
}

/**
 * xenDaemonDomainBlockPeek:
 * @domain: domain object
 * @path: path to the file or device
 * @offset: offset
 * @size: size
 * @buffer: return buffer
 *
 * Returns 0 if successful, -1 if error, -2 if declined.
 */
int
xenDaemonDomainBlockPeek (virDomainPtr domain, const char *path,
                          unsigned long long offset, size_t size,
                          void *buffer)
{
    xenUnifiedPrivatePtr priv;
    struct sexpr *root = NULL;
    int fd = -1, ret = -1;
    virDomainDefPtr def;
    int id;
    char * tty;
    int vncport;
    const char *actual;

    priv = (xenUnifiedPrivatePtr) domain->conn->privateData;

    if (domain->id < 0 && priv->xendConfigVersion < 3)
        return -2;              /* Decline, allow XM to handle it. */

    /* Security check: The path must correspond to a block device. */
    if (domain->id > 0)
        root = sexpr_get (domain->conn, "/xend/domain/%d?detail=1",
                          domain->id);
    else if (domain->id < 0)
        root = sexpr_get (domain->conn, "/xend/domain/%s?detail=1",
                          domain->name);
    else {
        /* This call always fails for dom0. */
        virXendError(VIR_ERR_NO_SUPPORT,
                      "%s", _("domainBlockPeek is not supported for dom0"));
        return -1;
    }

    if (!root) {
        virXendError(VIR_ERR_XEN_CALL, __FUNCTION__);
        return -1;
    }

    id = xenGetDomIdFromSxpr(root, priv->xendConfigVersion);
    xenUnifiedLock(priv);
    tty = xenStoreDomainGetConsolePath(domain->conn, id);
    vncport = xenStoreDomainGetVNCPort(domain->conn, id);
    xenUnifiedUnlock(priv);

    if (!(def = xenParseSxpr(root, priv->xendConfigVersion, NULL, tty,
                             vncport)))
        goto cleanup;

    if (!(actual = virDomainDiskPathByName(def, path))) {
        virXendError(VIR_ERR_INVALID_ARG,
                      _("%s: invalid path"), path);
        goto cleanup;
    }
    path = actual;

    /* The path is correct, now try to open it and get its size. */
    fd = open (path, O_RDONLY);
    if (fd == -1) {
        virReportSystemError(errno,
                             _("failed to open for reading: %s"),
                             path);
        goto cleanup;
    }

    /* Seek and read. */
    /* NB. Because we configure with AC_SYS_LARGEFILE, off_t should
     * be 64 bits on all platforms.
     */
    if (lseek (fd, offset, SEEK_SET) == (off_t) -1 ||
        saferead (fd, buffer, size) == (ssize_t) -1) {
        virReportSystemError(errno,
                             _("failed to lseek or read from file: %s"),
                             path);
        goto cleanup;
    }

    ret = 0;
 cleanup:
    VIR_FORCE_CLOSE(fd);
    sexpr_free(root);
    virDomainDefFree(def);
    return ret;
}

struct xenUnifiedDriver xenDaemonDriver = {
    .xenClose = xenDaemonClose,
    .xenVersion = xenDaemonGetVersion,
    .xenDomainSuspend = xenDaemonDomainSuspend,
    .xenDomainResume = xenDaemonDomainResume,
    .xenDomainShutdown = xenDaemonDomainShutdown,
    .xenDomainReboot = xenDaemonDomainReboot,
    .xenDomainDestroyFlags = xenDaemonDomainDestroyFlags,
    .xenDomainGetOSType = xenDaemonDomainGetOSType,
    .xenDomainGetMaxMemory = xenDaemonDomainGetMaxMemory,
    .xenDomainSetMaxMemory = xenDaemonDomainSetMaxMemory,
    .xenDomainSetMemory = xenDaemonDomainSetMemory,
    .xenDomainGetInfo = xenDaemonDomainGetInfo,
    .xenDomainPinVcpu = xenDaemonDomainPinVcpu,
    .xenDomainGetVcpus = xenDaemonDomainGetVcpus,
    .xenListDefinedDomains = xenDaemonListDefinedDomains,
    .xenNumOfDefinedDomains = xenDaemonNumOfDefinedDomains,
    .xenDomainCreate = xenDaemonDomainCreate,
    .xenDomainDefineXML = xenDaemonDomainDefineXML,
    .xenDomainUndefine = xenDaemonDomainUndefine,
    .xenDomainAttachDeviceFlags = xenDaemonAttachDeviceFlags,
    .xenDomainDetachDeviceFlags = xenDaemonDetachDeviceFlags,
    .xenDomainGetSchedulerType = xenDaemonGetSchedulerType,
    .xenDomainGetSchedulerParameters = xenDaemonGetSchedulerParameters,
    .xenDomainSetSchedulerParameters = xenDaemonSetSchedulerParameters,
};


/**
 * virDomainXMLDevID:
 * @domain: pointer to domain object
 * @dev: pointer to device config object
 * @class: Xen device class "vbd" or "vif" (OUT)
 * @ref: Xen device reference (OUT)
 *
 * Set class according to XML root, and:
 *  - if disk, copy in ref the target name from description
 *  - if network, get MAC address from description, scan XenStore and
 *    copy in ref the corresponding vif number.
 *  - if pci, get BDF from description, scan XenStore and
 *    copy in ref the corresponding dev number.
 *
 * Returns 0 in case of success, -1 in case of failure.
 */
static int
virDomainXMLDevID(virDomainPtr domain,
                  virDomainDeviceDefPtr dev,
                  char *class,
                  char *ref,
                  int ref_len)
{
    xenUnifiedPrivatePtr priv = domain->conn->privateData;
    char *xref;
    char *tmp;

    if (dev->type == VIR_DOMAIN_DEVICE_DISK) {
        if (dev->data.disk->driverName &&
            STREQ(dev->data.disk->driverName, "tap"))
            strcpy(class, "tap");
        else if (dev->data.disk->driverName &&
            STREQ(dev->data.disk->driverName, "tap2"))
            strcpy(class, "tap2");
        else
            strcpy(class, "vbd");

        if (dev->data.disk->dst == NULL)
            return -1;
        xenUnifiedLock(priv);
        xref = xenStoreDomainGetDiskID(domain->conn, domain->id,
                                       dev->data.disk->dst);
        xenUnifiedUnlock(priv);
        if (xref == NULL)
            return -1;

        tmp = virStrcpy(ref, xref, ref_len);
        VIR_FREE(xref);
        if (tmp == NULL)
            return -1;
    } else if (dev->type == VIR_DOMAIN_DEVICE_NET) {
        char mac[30];
        virDomainNetDefPtr def = dev->data.net;
        snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
                 def->mac[0], def->mac[1], def->mac[2],
                 def->mac[3], def->mac[4], def->mac[5]);

        strcpy(class, "vif");

        xenUnifiedLock(priv);
        xref = xenStoreDomainGetNetworkID(domain->conn, domain->id,
                                          mac);
        xenUnifiedUnlock(priv);
        if (xref == NULL)
            return -1;

        tmp = virStrcpy(ref, xref, ref_len);
        VIR_FREE(xref);
        if (tmp == NULL)
            return -1;
    } else if (dev->type == VIR_DOMAIN_DEVICE_HOSTDEV &&
               dev->data.hostdev->mode == VIR_DOMAIN_HOSTDEV_MODE_SUBSYS &&
               dev->data.hostdev->source.subsys.type == VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_PCI) {
        char *bdf;
        virDomainHostdevDefPtr def = dev->data.hostdev;

        if (virAsprintf(&bdf, "%04x:%02x:%02x.%0x",
                        def->source.subsys.u.pci.domain,
                        def->source.subsys.u.pci.bus,
                        def->source.subsys.u.pci.slot,
                        def->source.subsys.u.pci.function) < 0) {
            virReportOOMError();
            return -1;
        }

        strcpy(class, "pci");

        xenUnifiedLock(priv);
        xref = xenStoreDomainGetPCIID(domain->conn, domain->id, bdf);
        xenUnifiedUnlock(priv);
        VIR_FREE(bdf);
        if (xref == NULL)
            return -1;

        tmp = virStrcpy(ref, xref, ref_len);
        VIR_FREE(xref);
        if (tmp == NULL)
            return -1;
    } else {
        virXendError(VIR_ERR_NO_SUPPORT,
                     "%s", _("hotplug of device type not supported"));
        return -1;
    }

    return 0;
}
