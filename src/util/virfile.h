/*
 * virfile.h: safer file handling
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


#ifndef __VIR_FILES_H_
# define __VIR_FILES_H_

# include <stdio.h>

# include "internal.h"
# include "ignore-value.h"


/* Don't call these directly - use the macros below */
int virFileClose(int *fdptr, bool preserve_errno) ATTRIBUTE_RETURN_CHECK;
int virFileFclose(FILE **file, bool preserve_errno) ATTRIBUTE_RETURN_CHECK;
FILE *virFileFdopen(int *fdptr, const char *mode) ATTRIBUTE_RETURN_CHECK;

/* For use on normal paths; caller must check return value,
   and failure sets errno per close. */
# define VIR_CLOSE(FD) virFileClose(&(FD), false)
# define VIR_FCLOSE(FILE) virFileFclose(&(FILE), false)

/* Wrapper around fdopen that consumes fd on success. */
# define VIR_FDOPEN(FD, MODE) virFileFdopen(&(FD), MODE)

/* For use on cleanup paths; errno is unaffected by close,
   and no return value to worry about. */
# define VIR_FORCE_CLOSE(FD) ignore_value(virFileClose(&(FD), true))
# define VIR_FORCE_FCLOSE(FILE) ignore_value(virFileFclose(&(FILE), true))

/* Opaque type for managing a wrapper around an O_DIRECT fd.  */
struct _virFileDirectFd;

typedef struct _virFileDirectFd virFileDirectFd;
typedef virFileDirectFd *virFileDirectFdPtr;

int virFileDirectFdFlag(void);

virFileDirectFdPtr virFileDirectFdNew(int *fd, const char *name)
    ATTRIBUTE_NONNULL(1) ATTRIBUTE_NONNULL(2) ATTRIBUTE_RETURN_CHECK;

int virFileDirectFdClose(virFileDirectFdPtr dfd);

void virFileDirectFdFree(virFileDirectFdPtr dfd);

int virFileLock(int fd, bool shared, off_t start, off_t len);
int virFileUnlock(int fd, off_t start, off_t len);

#endif /* __VIR_FILES_H */
