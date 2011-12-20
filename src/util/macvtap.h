/*
 * Copyright (C) 2011 Red Hat, Inc.
 * Copyright (C) 2010 IBM Corporation
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
 * Authors:
 *     Stefan Berger <stefanb@us.ibm.com>
 */

#ifndef __UTIL_MACVTAP_H__
# define __UTIL_MACVTAP_H__

# include <config.h>

/* the mode type for macvtap devices */
enum virMacvtapMode {
    VIR_MACVTAP_MODE_VEPA,
    VIR_MACVTAP_MODE_PRIVATE,
    VIR_MACVTAP_MODE_BRIDGE,
    VIR_MACVTAP_MODE_PASSTHRU,
    VIR_MACVTAP_MODE_PCI_PASSTHRU,

    VIR_MACVTAP_MODE_LAST,
};

enum virVMOperationType {
    VIR_VM_OP_CREATE,
    VIR_VM_OP_SAVE,
    VIR_VM_OP_RESTORE,
    VIR_VM_OP_DESTROY,
    VIR_VM_OP_MIGRATE_OUT,
    VIR_VM_OP_MIGRATE_IN_START,
    VIR_VM_OP_MIGRATE_IN_FINISH,
    VIR_VM_OP_NO_OP,

    VIR_VM_OP_LAST
};

# if WITH_MACVTAP

#  include "internal.h"
#  include "network.h"

int openMacvtapTap(const char *ifname,
                   const unsigned char *macaddress,
                   const char *linkdev,
                   enum virMacvtapMode mode,
                   int vnet_hdr,
                   const unsigned char *vmuuid,
                   virVirtualPortProfileParamsPtr virtPortProfile,
                   char **res_ifname,
                   enum virVMOperationType vmop,
                   char *stateDir,
                   virBandwidthPtr bandwidth);

void delMacvtap(const char *ifname,
                const unsigned char *macaddress,
                const char *linkdev,
                int mode,
                virVirtualPortProfileParamsPtr virtPortProfile,
                char *stateDir);

int vpAssociatePortProfileId(const char *macvtap_ifname,
                             const unsigned char *macvtap_macaddr,
                             const char *linkdev,
                             const virVirtualPortProfileParamsPtr virtPort,
                             const unsigned char *vmuuid,
                             enum virVMOperationType vmOp);

int vpDisassociatePortProfileId(const char *macvtap_ifname,
                                const unsigned char *macvtap_macaddr,
                                const char *linkdev,
                                const virVirtualPortProfileParamsPtr virtPort,
                                enum virVMOperationType vmOp);

# endif /* WITH_MACVTAP */

VIR_ENUM_DECL(virVMOperation)
VIR_ENUM_DECL(virMacvtapMode)

#endif /* __UTIL_MACVTAP_H__ */
