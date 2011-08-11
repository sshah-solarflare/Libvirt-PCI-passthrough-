/*
 * interface.c: interface support functions
 *
 * Copyright (C) 2010 IBM Corp.
 * Copyright (C) 2010 Stefan Berger
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
 * chgIfaceFlags originated from bridge.c
 *
 * Author: Stefan Berger <stefanb@us.ibm.com>
 */

#include <config.h>

#include <sys/socket.h>
#include <sys/ioctl.h>
#include <dirent.h>
#include <fcntl.h>

#ifdef __linux__
# include <linux/if.h>
# include <linux/sockios.h>
# include <linux/if_vlan.h>
#endif

#include "internal.h"

#include "util.h"
#include "interface.h"
#include "virterror_internal.h"
#include "files.h"
#include "memory.h"
#include "logging.h"

/* For virReportOOMError()  and virReportSystemError() */
#define VIR_FROM_THIS VIR_FROM_NET

#define ifaceError(code, ...) \
        virReportErrorHelper(NULL, VIR_FROM_NET, code, __FILE__, \
                             __FUNCTION__, __LINE__, __VA_ARGS__)

#if __linux__
static int
getFlags(int fd, const char *ifname, struct ifreq *ifr) {

    memset(ifr, 0, sizeof(*ifr));

    if (virStrncpy(ifr->ifr_name,
                   ifname, strlen(ifname), sizeof(ifr->ifr_name)) == NULL)
        return ENODEV;

    if (ioctl(fd, SIOCGIFFLAGS, ifr) < 0)
        return errno;

    return 0;
}


/**
 * ifaceGetFlags
 *
 * @ifname : name of the interface
 * @flags : pointer to short holding the flags on success
 *
 * Get the flags of the interface. Returns 0 on success, error code on failure.
 */
int
ifaceGetFlags(const char *ifname, short *flags) {
    struct ifreq ifr;
    int rc;
    int fd = socket(PF_PACKET, SOCK_DGRAM, 0);

    if (fd < 0)
        return errno;

    rc = getFlags(fd, ifname, &ifr);

    *flags = ifr.ifr_flags;

    VIR_FORCE_CLOSE(fd);

    return rc;
}


int
ifaceIsUp(const char *ifname, bool *up) {
    short flags;
    int rc = ifaceGetFlags(ifname, &flags);

    if (rc)
        return rc;

    *up = ((flags & IFF_UP) == IFF_UP);

    return 0;
}
#else

/* Note: Showstopper on cygwin is only missing PF_PACKET */

int
ifaceGetFlags(const char *ifname ATTRIBUTE_UNUSED,
              short *flags ATTRIBUTE_UNUSED) {
    ifaceError(VIR_ERR_INTERNAL_ERROR, "%s",
               _("ifaceGetFlags is not supported on non-linux platforms"));
    return ENOSYS;
}

int
ifaceIsUp(const char *ifname ATTRIBUTE_UNUSED,
          bool *up ATTRIBUTE_UNUSED) {

    ifaceError(VIR_ERR_INTERNAL_ERROR, "%s",
               _("ifaceIsUp is not supported on non-linux platforms"));
    return ENOSYS;
}

#endif /* __linux__ */

/*
 * chgIfaceFlags: Change flags on an interface
 *
 * @ifname : name of the interface
 * @flagclear : the flags to clear
 * @flagset : the flags to set
 *
 * The new flags of the interface will be calculated as
 * flagmask = (~0 ^ flagclear)
 * newflags = (curflags & flagmask) | flagset;
 *
 * Returns 0 on success, errno on failure.
 */
#ifdef __linux__
static int chgIfaceFlags(const char *ifname, short flagclear, short flagset) {
    struct ifreq ifr;
    int rc = 0;
    short flags;
    short flagmask = (~0 ^ flagclear);
    int fd = socket(PF_PACKET, SOCK_DGRAM, 0);

    if (fd < 0)
        return errno;

    rc = getFlags(fd, ifname, &ifr);
    if (rc != 0)
        goto err_exit;

    flags = (ifr.ifr_flags & flagmask) | flagset;

    if (ifr.ifr_flags != flags) {
        ifr.ifr_flags = flags;

        if (ioctl(fd, SIOCSIFFLAGS, &ifr) < 0)
            rc = errno;
    }

err_exit:
    VIR_FORCE_CLOSE(fd);
    return rc;
}


/*
 * ifaceCtrl
 * @name: name of the interface
 * @up: true (1) for up, false (0) for down
 *
 * Function to control if an interface is activated (up, 1) or not (down, 0)
 *
 * Returns 0 in case of success or an errno code in case of failure.
 */
int
ifaceCtrl(const char *name, bool up)
{
    return chgIfaceFlags(name,
                         (up) ? 0      : IFF_UP,
                         (up) ? IFF_UP : 0);
}

#else

int
ifaceCtrl(const char *name ATTRIBUTE_UNUSED, bool up ATTRIBUTE_UNUSED)
{
    return ENOSYS;
}

#endif /* __linux__ */

/**
 * ifaceCheck
 *
 * @reportError: whether to report errors or keep silent
 * @ifname: Name of the interface
 * @macaddr: expected MAC address of the interface; not checked if NULL
 * @ifindex: expected index of the interface; not checked if '-1'
 *
 * Determine whether a given interface is still available. If so,
 * it must have the given MAC address and if an interface index is
 * passed, it must also match the interface index.
 *
 * Returns 0 on success, an error code on failure.
 *   ENODEV : if interface with given name does not exist or its interface
 *            index is different than the one passed
 *   EINVAL : if interface name is invalid (too long)
 */
#ifdef __linux__
int
ifaceCheck(bool reportError, const char *ifname,
           const unsigned char *macaddr, int ifindex)
{
    struct ifreq ifr;
    int fd = -1;
    int rc = 0;
    int idx;

    if (macaddr != NULL) {
        fd = socket(PF_PACKET, SOCK_DGRAM, 0);
        if (fd < 0)
            return errno;

        memset(&ifr, 0, sizeof(ifr));

        if (virStrncpy(ifr.ifr_name,
                       ifname, strlen(ifname), sizeof(ifr.ifr_name)) == NULL) {
            if (reportError)
                ifaceError(VIR_ERR_INTERNAL_ERROR,
                           _("invalid interface name %s"),
                           ifname);
            rc = EINVAL;
            goto err_exit;
        }

        if (ioctl(fd, SIOCGIFHWADDR, &ifr) < 0) {
            if (reportError)
                ifaceError(VIR_ERR_INTERNAL_ERROR,
                           _("coud not get MAC address of interface %s"),
                           ifname);
            rc = errno;
            goto err_exit;
        }

        if (memcmp(&ifr.ifr_hwaddr.sa_data, macaddr, VIR_MAC_BUFLEN) != 0) {
            rc = ENODEV;
            goto err_exit;
        }
    }

    if (ifindex != -1) {
        rc = ifaceGetIndex(reportError, ifname, &idx);
        if (rc == 0 && idx != ifindex)
            rc = ENODEV;
    }

 err_exit:
    VIR_FORCE_CLOSE(fd);

    return rc;
}

#else

int
ifaceCheck(bool reportError ATTRIBUTE_UNUSED,
           const char *ifname ATTRIBUTE_UNUSED,
           const unsigned char *macaddr ATTRIBUTE_UNUSED,
           int ifindex ATTRIBUTE_UNUSED)
{
    return ENOSYS;
}

#endif /* __linux__ */


/**
 * ifaceGetIndex
 *
 * @reportError: whether to report errors or keep silent
 * @ifname : Name of the interface whose index is to be found
 * @ifindex: Pointer to int where the index will be written into
 *
 * Get the index of an interface given its name.
 *
 * Returns 0 on success, an error code on failure.
 *   ENODEV : if interface with given name does not exist
 *   EINVAL : if interface name is invalid (too long)
 */
#ifdef __linux__
int
ifaceGetIndex(bool reportError, const char *ifname, int *ifindex)
{
    int rc = 0;
    struct ifreq ifreq;
    int fd = socket(PF_PACKET, SOCK_DGRAM, 0);

    if (fd < 0)
        return errno;

    memset(&ifreq, 0, sizeof(ifreq));

    if (virStrncpy(ifreq.ifr_name, ifname, strlen(ifname),
                   sizeof(ifreq.ifr_name)) == NULL) {
        if (reportError)
            ifaceError(VIR_ERR_INTERNAL_ERROR,
                       _("invalid interface name %s"),
                       ifname);
        rc = EINVAL;
        goto err_exit;
    }

    if (ioctl(fd, SIOCGIFINDEX, &ifreq) >= 0)
        *ifindex = ifreq.ifr_ifindex;
    else {
        if (reportError)
            ifaceError(VIR_ERR_INTERNAL_ERROR,
                       _("interface %s does not exist"),
                       ifname);
        rc = ENODEV;
    }

err_exit:
    VIR_FORCE_CLOSE(fd);

    return rc;
}

#else

int
ifaceGetIndex(bool reportError,
              const char *ifname ATTRIBUTE_UNUSED,
              int *ifindex ATTRIBUTE_UNUSED)
{
    if (reportError) {
        ifaceError(VIR_ERR_INTERNAL_ERROR, "%s",
                   _("ifaceGetIndex is not supported on non-linux platforms"));
    }

    return ENOSYS;
}

#endif /* __linux__ */

#ifdef __linux__
int
ifaceGetVlanID(const char *vlanifname, int *vlanid) {
    struct vlan_ioctl_args vlanargs = {
      .cmd = GET_VLAN_VID_CMD,
    };
    int rc = 0;
    int fd = socket(PF_PACKET, SOCK_DGRAM, 0);

    if (fd < 0)
        return errno;

    if (virStrcpyStatic(vlanargs.device1, vlanifname) == NULL) {
        rc = EINVAL;
        goto err_exit;
    }

    if (ioctl(fd, SIOCGIFVLAN, &vlanargs) != 0) {
        rc = errno;
        goto err_exit;
    }

    *vlanid = vlanargs.u.VID;

 err_exit:
    VIR_FORCE_CLOSE(fd);

    return rc;
}

#else

int
ifaceGetVlanID(const char *vlanifname ATTRIBUTE_UNUSED,
               int *vlanid ATTRIBUTE_UNUSED) {

    ifaceError(VIR_ERR_INTERNAL_ERROR, "%s",
               _("ifaceGetVlanID is not supported on non-linux platforms"));

    return ENOSYS;
}
#endif /* __linux__ */

pciDevice *
ifaceGetVf(const char *ifname, unsigned num)
{
    char *node, *device, *base;
    int ret;
    unsigned domain, bus, slot, function;

    if (virAsprintf(&node, "/sys/class/net/%s/device/virtfn%d", ifname, num) < 0) {
        virReportOOMError();
        return NULL;
    }

    if (virFileResolveLink(node, &device) < 0) {
        VIR_FREE(node);
        return NULL;
    }
    VIR_FREE(node);

    base = basename(device);
    ret = sscanf(base, "%04x:%02x:%02x.%01x", &domain, &bus, &slot, &function);
    VIR_FREE(device);

    if (ret != 4) {
        virReportSystemError(errno,
                             _("unable to parse address '%s'"), base);
        return NULL;
    }

    return pciGetDevice(domain, bus, slot, function);
}

pciDevice *
ifaceReserveFreeVf(const char *ifname, const unsigned char *mac)
{
    pciDevice *dev;
    unsigned int i;

    for (i = 0; (dev = ifaceGetVf(ifname, i)) != NULL; i++) {
        if (!pciVfReserve(dev, mac))
            return dev;
        pciFreeDevice(dev);
    }

    return NULL;
}

pciDevice *
ifaceFindReservedVf(const char *ifname, const unsigned char *mac)
{
    pciDevice *dev;
    unsigned int i;

    for(i=0; (dev = ifaceGetVf(ifname, i)) != NULL; i++) {
        unsigned char devMac[VIR_MAC_BUFLEN];

        if (!pciVfGetMacAddr(dev, devMac) &&
            !memcmp(devMac, mac, VIR_MAC_BUFLEN)) {
            return dev;
        }
        pciFreeDevice(dev);
    }

    return NULL;
}

void
ifaceAddRemoveSfcPeerDevice(const char *ifname, const unsigned char *mac, bool add)
{
    char *node, *line;
    int rc;

    if (virAsprintf(&node, "/sys/class/net/%s/device/local_addrs", ifname) < 0) {
        virReportOOMError();
        return;
    }

    if (VIR_ALLOC_N(line, VIR_MAC_STRING_BUFLEN + 1) < 0) {
        VIR_FREE(node);
        virReportOOMError();
        return;
    }

    line[0] = add ? '+' : '-';
    virFormatMacAddr(mac, line + 1);
    rc = virFileWriteStr(node, line, 0);
    if (rc == -1 && errno != ENOENT) {
        VIR_WARN("Failed to write '%s' to '%s': %s",
                 line, node, strerror(errno));
    }

    VIR_FREE(line);
    VIR_FREE(node);
}

void
ifaceAddRemoveSfcPeerBridge(const char *ifname, const unsigned char *mac,
                            bool add)
{
    DIR *dir = NULL;
    char *node;
    int rc;
    struct dirent *ent;

    if (virAsprintf(&node, "/sys/class/net/%s/brif", ifname) < 0) {
        virReportOOMError();
        return;
    }

    dir = opendir(node);
    if (dir == NULL) {
        rc = errno;
        VIR_WARN("Failed to open %s: %s", node, strerror(rc));
        VIR_FREE(node);
        return;
    }
    VIR_FREE(node);

    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, "..") && strcmp(ent->d_name, ".")) {
            ifaceAddRemoveSfcPeerDevice(ent->d_name, mac, add);
        }
    }

    closedir(dir);
}
