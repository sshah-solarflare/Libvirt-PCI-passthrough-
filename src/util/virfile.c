/*
 * virfile.c: safer file handling
 *
 * Copyright (C) 2010-2011 Red Hat, Inc.
 * Copyright (C) 2010 IBM Corporation
 * Copyright (C) 2010 Stefan Berger
 * Copyright (C) 2010 Eric Blake
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
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307  USA
 *
 */

#include <config.h>
#include "internal.h"

#include "virfile.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "command.h"
#include "configmake.h"
#include "memory.h"
#include "virterror_internal.h"

#define VIR_FROM_THIS VIR_FROM_NONE
#define virFileError(code, ...)                                   \
    virReportErrorHelper(VIR_FROM_THIS, code, __FILE__,           \
                         __FUNCTION__, __LINE__, __VA_ARGS__)


int virFileClose(int *fdptr, bool preserve_errno)
{
    int saved_errno;
    int rc = 0;

    if (*fdptr >= 0) {
        if (preserve_errno)
            saved_errno = errno;
        rc = close(*fdptr);
        *fdptr = -1;
        if (preserve_errno)
            errno = saved_errno;
    }

    return rc;
}


int virFileFclose(FILE **file, bool preserve_errno)
{
    int saved_errno;
    int rc = 0;

    if (*file) {
        if (preserve_errno)
            saved_errno = errno;
        rc = fclose(*file);
        *file = NULL;
        if (preserve_errno)
            errno = saved_errno;
    }

    return rc;
}


FILE *virFileFdopen(int *fdptr, const char *mode)
{
    FILE *file = NULL;

    if (*fdptr >= 0) {
        file = fdopen(*fdptr, mode);
        if (file)
            *fdptr = -1;
    } else {
        errno = EBADF;
    }

    return file;
}


/* Opaque type for managing a wrapper around an O_DIRECT fd.  For now,
 * read-write is not supported, just a single direction.  */
struct _virFileDirectFd {
    virCommandPtr cmd; /* Child iohelper process to do the I/O.  */
};

/**
 * virFileDirectFdFlag:
 *
 * Returns 0 if the kernel can avoid file system cache pollution
 * without any additional flags, O_DIRECT if the original fd must be
 * opened in direct mode, or -1 if there is no support for bypassing
 * the file system cache.
 */
int
virFileDirectFdFlag(void)
{
    /* XXX For now, Linux posix_fadvise is not powerful enough to
     * avoid O_DIRECT.  */
    return O_DIRECT ? O_DIRECT : -1;
}

/**
 * virFileDirectFdNew:
 * @fd: pointer to fd to wrap
 * @name: name of fd, for diagnostics
 *
 * Update *FD (created with virFileDirectFdFlag() among the flags to
 * open()) to ensure that all I/O to that file will bypass the system
 * cache.  This must be called after open() and optional fchown() or
 * fchmod(), but before any seek or I/O, and only on seekable fd.  The
 * file must be O_RDONLY (to read the entire existing file) or
 * O_WRONLY (to write to an empty file).  In some cases, *FD is
 * changed to a non-seekable pipe; in this case, the caller must not
 * do anything further with the original fd.
 *
 * On success, the new wrapper object is returned, which must be later
 * freed with virFileDirectFdFree().  On failure, *FD is unchanged, an
 * error message is output, and NULL is returned.
 */
virFileDirectFdPtr
virFileDirectFdNew(int *fd, const char *name)
{
    virFileDirectFdPtr ret = NULL;
    bool output = false;
    int pipefd[2] = { -1, -1 };
    int mode = -1;

    /* XXX support posix_fadvise rather than spawning a child, if the
     * kernel support for that is decent enough.  */

    if (!O_DIRECT) {
        virFileError(VIR_ERR_INTERNAL_ERROR, "%s",
                     _("O_DIRECT unsupported on this platform"));
        return NULL;
    }

    if (VIR_ALLOC(ret) < 0) {
        virReportOOMError();
        return NULL;
    }

#ifdef F_GETFL
    /* Mingw lacks F_GETFL, but it also lacks O_DIRECT so didn't get
     * here in the first place.  All other platforms reach this
     * line.  */
    mode = fcntl(*fd, F_GETFL);
#endif

    if (mode < 0) {
        virFileError(VIR_ERR_INTERNAL_ERROR, _("invalid fd %d for %s"),
                     *fd, name);
        goto error;
    } else if ((mode & O_ACCMODE) == O_WRONLY) {
        output = true;
    } else if ((mode & O_ACCMODE) != O_RDONLY) {
        virFileError(VIR_ERR_INTERNAL_ERROR, _("unexpected mode %x for %s"),
                     mode & O_ACCMODE, name);
        goto error;
    }

    if (pipe2(pipefd, O_CLOEXEC) < 0) {
        virFileError(VIR_ERR_INTERNAL_ERROR,
                     _("unable to create pipe for %s"), name);
        goto error;
    }

    ret->cmd = virCommandNewArgList(LIBEXECDIR "/libvirt_iohelper",
                                    name, "0", NULL);
    if (output) {
        virCommandSetInputFD(ret->cmd, pipefd[0]);
        virCommandSetOutputFD(ret->cmd, fd);
        virCommandAddArg(ret->cmd, "1");
    } else {
        virCommandSetInputFD(ret->cmd, *fd);
        virCommandSetOutputFD(ret->cmd, &pipefd[1]);
        virCommandAddArg(ret->cmd, "0");
    }

    if (virCommandRunAsync(ret->cmd, NULL) < 0)
        goto error;

    if (VIR_CLOSE(pipefd[!output]) < 0) {
        virFileError(VIR_ERR_INTERNAL_ERROR, "%s", _("unable to close pipe"));
        goto error;
    }

    VIR_FORCE_CLOSE(*fd);
    *fd = pipefd[output];
    return ret;

error:
    VIR_FORCE_CLOSE(pipefd[0]);
    VIR_FORCE_CLOSE(pipefd[1]);
    virFileDirectFdFree(ret);
    return NULL;
}

/**
 * virFileDirectFdClose:
 * @dfd: direct fd wrapper, or NULL
 *
 * If DFD is valid, then ensure that I/O has completed, which may
 * include reaping a child process.  Return 0 if all data for the
 * wrapped fd is complete, or -1 on failure with an error emitted.
 * This function intentionally returns 0 when DFD is NULL, so that
 * callers can conditionally create a virFileDirectFd wrapper but
 * unconditionally call the cleanup code.  To avoid deadlock, only
 * call this after closing the fd resulting from virFileDirectFdNew().
 */
int
virFileDirectFdClose(virFileDirectFdPtr dfd)
{
    if (!dfd)
        return 0;

    return virCommandWait(dfd->cmd, NULL);
}

/**
 * virFileDirectFdFree:
 * @dfd: direct fd wrapper, or NULL
 *
 * Free all remaining resources associated with DFD.  If
 * virFileDirectFdClose() was not previously called, then this may
 * discard some previous I/O.  To avoid deadlock, only call this after
 * closing the fd resulting from virFileDirectFdNew().
 */
void
virFileDirectFdFree(virFileDirectFdPtr dfd)
{
    if (!dfd)
        return;

    virCommandFree(dfd->cmd);
    VIR_FREE(dfd);
}


#ifndef WIN32
/**
 * virFileLock:
 * @fd: file descriptor to acquire the lock on
 * @shared: type of lock to acquire
 * @start: byte offset to start lock
 * @len: length of lock (0 to acquire entire remaining file from @start)
 *
 * Attempt to acquire a lock on the file @fd. If @shared
 * is true, then a shared lock will be acquired,
 * otherwise an exclusive lock will be acquired. If
 * the lock cannot be acquired, an error will be
 * returned. This will not wait to acquire the lock if
 * another process already holds it.
 *
 * The lock will be released when @fd is closed. The lock
 * will also be released if *any* other open file descriptor
 * pointing to the same underlying file is closed. As such
 * this function should not be relied on in multi-threaded
 * apps where other threads can be opening/closing arbitrary
 * files.
 *
 * Returns 0 on success, or -errno otherwise
 */
int virFileLock(int fd, bool shared, off_t start, off_t len)
{
    struct flock fl = {
        .l_type = shared ? F_RDLCK : F_WRLCK,
        .l_whence = SEEK_SET,
        .l_start = start,
        .l_len = len,
    };

    if (fcntl(fd, F_SETLK, &fl) < 0)
        return -errno;

    return 0;
}


/**
 * virFileUnlock:
 * @fd: file descriptor to release the lock on
 * @start: byte offset to start unlock
 * @len: length of lock (0 to release entire remaining file from @start)
 *
 * Release a lock previously acquired with virFileUnlock().
 * NB the lock will also be released if any open file descriptor
 * pointing to the same file as @fd is closed
 *
 * Returns 0 on succcess, or -errno on error
 */
int virFileUnlock(int fd, off_t start, off_t len)
{
    struct flock fl = {
        .l_type = F_UNLCK,
        .l_whence = SEEK_SET,
        .l_start = start,
        .l_len = len,
    };

    if (fcntl(fd, F_SETLK, &fl) < 0)
        return -errno;

    return 0;
}
#else
int virFileLock(int fd ATTRIBUTE_UNUSED,
                bool shared ATTRIBUTE_UNUSED,
                off_t start ATTRIBUTE_UNUSED,
                off_t len ATTRIBUTE_UNUSED)
{
    return -ENOSYS;
}
int virFileUnlock(int fd ATTRIBUTE_UNUSED,
                  off_t start ATTRIBUTE_UNUSED,
                  off_t len ATTRIBUTE_UNUSED)
{
    return -ENOSYS;
}
#endif
