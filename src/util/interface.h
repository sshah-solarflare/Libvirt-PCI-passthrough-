/*
 * interface.h: interface helper APIs for libvirt
 *
 * Copyright (C) 2011 Red Hat, Inc.
 * Copyright (C) 2010 IBM Corporation, Inc.
 *
 * See COPYING.LIB for the License of this software
 *
 * Stefan Berger <stefanb@us.ibm.com>
 */
#ifndef __VIR_INTERFACE_H__
# define __VIR_INTERFACE_H__

# include <stdint.h>

# if __linux__

#  include <sys/socket.h>
#  include <linux/netlink.h>

# else

struct nlattr;

# endif

# include "datatypes.h"
# include "network.h"

# define NET_SYSFS "/sys/class/net/"

int ifaceGetFlags(const char *name, short *flags);
int ifaceIsUp(const char *name, bool *up);

int ifaceCtrl(const char *name, bool up);

static inline int ifaceUp(const char *name) {
    return ifaceCtrl(name, true);
}

static inline int ifaceDown(const char *name) {
    return ifaceCtrl(name, false);
}

int ifaceCheck(bool reportError, const char *ifname,
               const unsigned char *macaddr, int ifindex);

int ifaceGetIndex(bool reportError, const char *ifname, int *ifindex);

int ifaceGetVlanID(const char *vlanifname, int *vlanid);

int ifaceSetMacAddress(const char *ifname, const unsigned char *macaddr);

int ifaceGetMacAddress(const char *ifname, unsigned char *macaddr);

int ifaceGetIPAddress(const char *ifname, virSocketAddrPtr addr);

int ifaceMacvtapLinkAdd(const char *type,
                        const unsigned char *macaddress, int macaddrsize,
                        const char *ifname,
                        const char *srcdev,
                        uint32_t macvlan_mode,
                        int *retry);

int ifaceLinkDel(const char *ifname);

int ifaceMacvtapLinkDump(bool nltarget_kernel, const char *ifname, int ifindex,
                         struct nlattr **tb, unsigned char **recvbuf,
                         uint32_t (*getPidFunc)(void));

int ifaceGetNthParent(int ifindex, const char *ifname, unsigned int nthParent,
                      int *parent_ifindex, char *parent_ifname,
                      unsigned int *nth)
    ATTRIBUTE_NONNULL(2) ATTRIBUTE_NONNULL(4) ATTRIBUTE_NONNULL(5)
    ATTRIBUTE_NONNULL(6);

int ifaceReplaceMacAddress(const unsigned char *macaddress,
                           const char *linkdev,
                           const char *stateDir);

int ifaceReplaceVfMacAddress(const unsigned char *macaddress,
                             const char *vf_pci_addr);

int ifaceRestoreMacAddress(const char *linkdev,
                           const char *stateDir);

int ifaceIsVirtualFunction(const char *ifname);

int ifaceGetVirtualFunctionIndex(const char *pfname, const char *vfname,
                                 int *vf_index);

int ifaceGetPhysicalFunction(const char *ifname, char **pfname);

int ifaceGetVirtualFunctions(const char *pfname, 
                             char ***vfname,
                             unsigned int *n_vfname);

int ifaceGetVirtualFunctionsPCIAddr(const char *pfname,
                                    char ***vf_pci_addr,
                                    unsigned int *n_vf_pci_addr);
    
int ifaceGetVfPCIAddr(const char *vf_pci_addr,
                      unsigned int *domain,
                      unsigned int *bus,
                      unsigned int *slot,
                      unsigned int *function);

#endif /* __VIR_INTERFACE_H__ */
