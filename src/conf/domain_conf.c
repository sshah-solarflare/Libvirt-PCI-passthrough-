/*
 * domain_conf.c: domain XML processing
 *
 * Copyright (C) 2006-2011 Red Hat, Inc.
 * Copyright (C) 2006-2008 Daniel P. Berrange
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
 * Author: Daniel P. Berrange <berrange@redhat.com>
 */

#include <config.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/time.h>
#include <strings.h>

#include "virterror_internal.h"
#include "datatypes.h"
#include "domain_conf.h"
#include "memory.h"
#include "verify.h"
#include "xml.h"
#include "uuid.h"
#include "util.h"
#include "buf.h"
#include "c-ctype.h"
#include "logging.h"
#include "network.h"
#include "nwfilter_conf.h"
#include "ignore-value.h"
#include "storage_file.h"
#include "virfile.h"
#include "bitmap.h"
#include "count-one-bits.h"

#define VIR_FROM_THIS VIR_FROM_DOMAIN

/* virDomainVirtType is used to set bits in the expectedVirtTypes bitmask,
 * verify that it doesn't overflow an unsigned int when shifting */
verify(VIR_DOMAIN_VIRT_LAST <= 32);

VIR_ENUM_IMPL(virDomainTaint, VIR_DOMAIN_TAINT_LAST,
              "custom-argv",
              "custom-monitor",
              "high-privileges",
              "shell-scripts",
              "disk-probing",
              "external-launch");

VIR_ENUM_IMPL(virDomainVirt, VIR_DOMAIN_VIRT_LAST,
              "qemu",
              "kqemu",
              "kvm",
              "xen",
              "lxc",
              "uml",
              "openvz",
              "vserver",
              "ldom",
              "test",
              "vmware",
              "hyperv",
              "vbox",
              "one",
              "phyp")

VIR_ENUM_IMPL(virDomainBoot, VIR_DOMAIN_BOOT_LAST,
              "fd",
              "cdrom",
              "hd",
              "network")

VIR_ENUM_IMPL(virDomainFeature, VIR_DOMAIN_FEATURE_LAST,
              "acpi",
              "apic",
              "pae",
              "hap",
              "viridian")

VIR_ENUM_IMPL(virDomainLifecycle, VIR_DOMAIN_LIFECYCLE_LAST,
              "destroy",
              "restart",
              "rename-restart",
              "preserve")

VIR_ENUM_IMPL(virDomainLifecycleCrash, VIR_DOMAIN_LIFECYCLE_CRASH_LAST,
              "destroy",
              "restart",
              "rename-restart",
              "preserve",
              "coredump-destroy",
              "coredump-restart")

VIR_ENUM_IMPL(virDomainDevice, VIR_DOMAIN_DEVICE_LAST,
              "disk",
              "lease",
              "filesystem",
              "interface",
              "input",
              "sound",
              "video",
              "hostdev",
              "watchdog",
              "controller",
              "graphics")

VIR_ENUM_IMPL(virDomainDeviceAddress, VIR_DOMAIN_DEVICE_ADDRESS_TYPE_LAST,
              "none",
              "pci",
              "drive",
              "virtio-serial",
              "ccid")

VIR_ENUM_IMPL(virDomainDeviceAddressPciMulti,
              VIR_DOMAIN_DEVICE_ADDRESS_PCI_MULTI_LAST,
              "default",
              "on",
              "off")

VIR_ENUM_IMPL(virDomainDisk, VIR_DOMAIN_DISK_TYPE_LAST,
              "block",
              "file",
              "dir",
              "network")

VIR_ENUM_IMPL(virDomainDiskDevice, VIR_DOMAIN_DISK_DEVICE_LAST,
              "disk",
              "cdrom",
              "floppy")

VIR_ENUM_IMPL(virDomainDiskBus, VIR_DOMAIN_DISK_BUS_LAST,
              "ide",
              "fdc",
              "scsi",
              "virtio",
              "xen",
              "usb",
              "uml",
              "sata")

VIR_ENUM_IMPL(virDomainDiskCache, VIR_DOMAIN_DISK_CACHE_LAST,
              "default",
              "none",
              "writethrough",
              "writeback")

VIR_ENUM_IMPL(virDomainDiskErrorPolicy, VIR_DOMAIN_DISK_ERROR_POLICY_LAST,
              "default",
              "stop",
              "ignore",
              "enospace")

VIR_ENUM_IMPL(virDomainDiskProtocol, VIR_DOMAIN_DISK_PROTOCOL_LAST,
              "nbd",
              "rbd",
              "sheepdog")

VIR_ENUM_IMPL(virDomainDiskIo, VIR_DOMAIN_DISK_IO_LAST,
              "default",
              "native",
              "threads")
VIR_ENUM_IMPL(virDomainIoEventFd, VIR_DOMAIN_IO_EVENT_FD_LAST,
              "default",
              "on",
              "off")

VIR_ENUM_IMPL(virDomainVirtioEventIdx, VIR_DOMAIN_VIRTIO_EVENT_IDX_LAST,
              "default",
              "on",
              "off")

VIR_ENUM_IMPL(virDomainDiskSnapshot, VIR_DOMAIN_DISK_SNAPSHOT_LAST,
              "default",
              "no",
              "internal",
              "external")

VIR_ENUM_IMPL(virDomainController, VIR_DOMAIN_CONTROLLER_TYPE_LAST,
              "ide",
              "fdc",
              "scsi",
              "sata",
              "virtio-serial",
              "ccid")

VIR_ENUM_IMPL(virDomainControllerModel, VIR_DOMAIN_CONTROLLER_MODEL_LAST,
              "auto",
              "buslogic",
              "lsilogic",
              "lsisas1068",
              "vmpvscsi")

VIR_ENUM_IMPL(virDomainFS, VIR_DOMAIN_FS_TYPE_LAST,
              "mount",
              "block",
              "file",
              "template")

VIR_ENUM_IMPL(virDomainFSAccessMode, VIR_DOMAIN_FS_ACCESSMODE_LAST,
              "passthrough",
              "mapped",
              "squash")


VIR_ENUM_IMPL(virDomainNet, VIR_DOMAIN_NET_TYPE_LAST,
              "user",
              "ethernet",
              "server",
              "client",
              "mcast",
              "network",
              "bridge",
              "internal",
              "direct")

VIR_ENUM_IMPL(virDomainNetBackend, VIR_DOMAIN_NET_BACKEND_TYPE_LAST,
              "default",
              "qemu",
              "vhost")

VIR_ENUM_IMPL(virDomainNetVirtioTxMode, VIR_DOMAIN_NET_VIRTIO_TX_MODE_LAST,
              "default",
              "iothread",
              "timer")

VIR_ENUM_IMPL(virDomainChrChannelTarget,
              VIR_DOMAIN_CHR_CHANNEL_TARGET_TYPE_LAST,
              "guestfwd",
              "virtio")

VIR_ENUM_IMPL(virDomainChrConsoleTarget,
              VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_LAST,
              "serial",
              "xen",
              "uml",
              "virtio")

VIR_ENUM_IMPL(virDomainChrDevice, VIR_DOMAIN_CHR_DEVICE_TYPE_LAST,
              "parallel",
              "serial",
              "console",
              "channel")

VIR_ENUM_IMPL(virDomainChr, VIR_DOMAIN_CHR_TYPE_LAST,
              "null",
              "vc",
              "pty",
              "dev",
              "file",
              "pipe",
              "stdio",
              "udp",
              "tcp",
              "unix",
              "spicevmc")

VIR_ENUM_IMPL(virDomainChrTcpProtocol, VIR_DOMAIN_CHR_TCP_PROTOCOL_LAST,
              "raw",
              "telnet",
              "telnets",
              "tls")

VIR_ENUM_IMPL(virDomainChrSpicevmc, VIR_DOMAIN_CHR_SPICEVMC_LAST,
              "vdagent",
              "smartcard")

VIR_ENUM_IMPL(virDomainSmartcard, VIR_DOMAIN_SMARTCARD_TYPE_LAST,
              "host",
              "host-certificates",
              "passthrough")

VIR_ENUM_IMPL(virDomainSoundModel, VIR_DOMAIN_SOUND_MODEL_LAST,
              "sb16",
              "es1370",
              "pcspk",
              "ac97",
              "ich6")

VIR_ENUM_IMPL(virDomainMemballoonModel, VIR_DOMAIN_MEMBALLOON_MODEL_LAST,
              "virtio",
              "xen",
              "none")

VIR_ENUM_IMPL(virDomainSmbiosMode, VIR_DOMAIN_SMBIOS_LAST,
              "none",
              "emulate",
              "host",
              "sysinfo")

VIR_ENUM_IMPL(virDomainWatchdogModel, VIR_DOMAIN_WATCHDOG_MODEL_LAST,
              "i6300esb",
              "ib700")

VIR_ENUM_IMPL(virDomainWatchdogAction, VIR_DOMAIN_WATCHDOG_ACTION_LAST,
              "reset",
              "shutdown",
              "poweroff",
              "pause",
              "dump",
              "none")

VIR_ENUM_IMPL(virDomainVideo, VIR_DOMAIN_VIDEO_TYPE_LAST,
              "vga",
              "cirrus",
              "vmvga",
              "xen",
              "vbox",
              "qxl")

VIR_ENUM_IMPL(virDomainInput, VIR_DOMAIN_INPUT_TYPE_LAST,
              "mouse",
              "tablet")

VIR_ENUM_IMPL(virDomainInputBus, VIR_DOMAIN_INPUT_BUS_LAST,
              "ps2",
              "usb",
              "xen")

VIR_ENUM_IMPL(virDomainGraphics, VIR_DOMAIN_GRAPHICS_TYPE_LAST,
              "sdl",
              "vnc",
              "rdp",
              "desktop",
              "spice")

VIR_ENUM_IMPL(virDomainGraphicsListen, VIR_DOMAIN_GRAPHICS_LISTEN_TYPE_LAST,
              "none",
              "address",
              "network")

VIR_ENUM_IMPL(virDomainGraphicsAuthConnected,
              VIR_DOMAIN_GRAPHICS_AUTH_CONNECTED_LAST,
              "default",
              "fail",
              "disconnect",
              "keep")

VIR_ENUM_IMPL(virDomainGraphicsSpiceChannelName,
              VIR_DOMAIN_GRAPHICS_SPICE_CHANNEL_LAST,
              "main",
              "display",
              "inputs",
              "cursor",
              "playback",
              "record",
              "smartcard");

VIR_ENUM_IMPL(virDomainGraphicsSpiceChannelMode,
              VIR_DOMAIN_GRAPHICS_SPICE_CHANNEL_MODE_LAST,
              "any",
              "secure",
              "insecure");

VIR_ENUM_IMPL(virDomainGraphicsSpiceImageCompression,
              VIR_DOMAIN_GRAPHICS_SPICE_IMAGE_COMPRESSION_LAST,
              "default",
              "auto_glz",
              "auto_lz",
              "quic",
              "glz",
              "lz",
              "off");

VIR_ENUM_IMPL(virDomainGraphicsSpiceJpegCompression,
              VIR_DOMAIN_GRAPHICS_SPICE_JPEG_COMPRESSION_LAST,
              "default",
              "auto",
              "never",
              "always");

VIR_ENUM_IMPL(virDomainGraphicsSpiceZlibCompression,
              VIR_DOMAIN_GRAPHICS_SPICE_ZLIB_COMPRESSION_LAST,
              "default",
              "auto",
              "never",
              "always");

VIR_ENUM_IMPL(virDomainGraphicsSpicePlaybackCompression,
              VIR_DOMAIN_GRAPHICS_SPICE_PLAYBACK_COMPRESSION_LAST,
              "default",
              "on",
              "off");

VIR_ENUM_IMPL(virDomainGraphicsSpiceStreamingMode,
              VIR_DOMAIN_GRAPHICS_SPICE_STREAMING_MODE_LAST,
              "default",
              "filter",
              "all",
              "off");

VIR_ENUM_IMPL(virDomainGraphicsSpiceClipboardCopypaste,
              VIR_DOMAIN_GRAPHICS_SPICE_CLIPBOARD_COPYPASTE_LAST,
              "default",
              "yes",
              "no");

VIR_ENUM_IMPL(virDomainHostdevMode, VIR_DOMAIN_HOSTDEV_MODE_LAST,
              "subsystem",
              "capabilities")

VIR_ENUM_IMPL(virDomainHostdevSubsys, VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_LAST,
              "usb",
              "pci")

VIR_ENUM_IMPL(virDomainPciRombarMode,
              VIR_DOMAIN_PCI_ROMBAR_LAST,
              "default",
              "on",
              "off")

VIR_ENUM_IMPL(virDomainState, VIR_DOMAIN_LAST,
              "nostate",
              "running",
              "blocked",
              "paused",
              "shutdown",
              "shutoff",
              "crashed")

/* virDomainSnapshotState is really virDomainState plus one extra state */
VIR_ENUM_IMPL(virDomainSnapshotState, VIR_DOMAIN_DISK_SNAPSHOT+1,
              "nostate",
              "running",
              "blocked",
              "paused",
              "shutdown",
              "shutoff",
              "crashed",
              "disk-snapshot")

#define VIR_DOMAIN_NOSTATE_LAST (VIR_DOMAIN_NOSTATE_UNKNOWN + 1)
VIR_ENUM_IMPL(virDomainNostateReason, VIR_DOMAIN_NOSTATE_LAST,
              "unknown")

#define VIR_DOMAIN_RUNNING_LAST (VIR_DOMAIN_RUNNING_SAVE_CANCELED + 1)
VIR_ENUM_IMPL(virDomainRunningReason, VIR_DOMAIN_RUNNING_LAST,
              "unknown",
              "booted",
              "migrated",
              "restored",
              "from snapshot",
              "unpaused",
              "migration canceled",
              "save canceled")

#define VIR_DOMAIN_BLOCKED_LAST (VIR_DOMAIN_BLOCKED_UNKNOWN + 1)
VIR_ENUM_IMPL(virDomainBlockedReason, VIR_DOMAIN_BLOCKED_LAST,
              "unknown")

#define VIR_DOMAIN_PAUSED_LAST (VIR_DOMAIN_PAUSED_SHUTTING_DOWN + 1)
VIR_ENUM_IMPL(virDomainPausedReason, VIR_DOMAIN_PAUSED_LAST,
              "unknown",
              "user",
              "migration",
              "save",
              "dump",
              "ioerror",
              "watchdog",
              "from snapshot",
              "shutdown")

#define VIR_DOMAIN_SHUTDOWN_LAST (VIR_DOMAIN_SHUTDOWN_USER + 1)
VIR_ENUM_IMPL(virDomainShutdownReason, VIR_DOMAIN_SHUTDOWN_LAST,
              "unknown",
              "user")

#define VIR_DOMAIN_SHUTOFF_LAST (VIR_DOMAIN_SHUTOFF_FROM_SNAPSHOT + 1)
VIR_ENUM_IMPL(virDomainShutoffReason, VIR_DOMAIN_SHUTOFF_LAST,
              "unknown",
              "shutdown",
              "destroyed",
              "crashed",
              "migrated",
              "saved",
              "failed",
              "from snapshot")

#define VIR_DOMAIN_CRASHED_LAST (VIR_DOMAIN_CRASHED_UNKNOWN + 1)
VIR_ENUM_IMPL(virDomainCrashedReason, VIR_DOMAIN_CRASHED_LAST,
              "unknown")

VIR_ENUM_IMPL(virDomainSeclabel, VIR_DOMAIN_SECLABEL_LAST,
              "dynamic",
              "static")

VIR_ENUM_IMPL(virDomainClockOffset, VIR_DOMAIN_CLOCK_OFFSET_LAST,
              "utc",
              "localtime",
              "variable",
              "timezone");

VIR_ENUM_IMPL(virDomainTimerName, VIR_DOMAIN_TIMER_NAME_LAST,
              "platform",
              "pit",
              "rtc",
              "hpet",
              "tsc");

VIR_ENUM_IMPL(virDomainTimerTrack, VIR_DOMAIN_TIMER_TRACK_LAST,
              "boot",
              "guest",
              "wall");

VIR_ENUM_IMPL(virDomainTimerTickpolicy, VIR_DOMAIN_TIMER_TICKPOLICY_LAST,
              "delay",
              "catchup",
              "merge",
              "discard");

VIR_ENUM_IMPL(virDomainTimerMode, VIR_DOMAIN_TIMER_MODE_LAST,
              "auto",
              "native",
              "emulate",
              "paravirt",
              "smpsafe");

VIR_ENUM_IMPL(virDomainNumatuneMemMode, VIR_DOMAIN_NUMATUNE_MEM_LAST,
              "strict",
              "preferred",
              "interleave");

#define virDomainReportError(code, ...)                              \
    virReportErrorHelper(VIR_FROM_DOMAIN, code, __FILE__,            \
                         __FUNCTION__, __LINE__, __VA_ARGS__)

#define VIR_DOMAIN_XML_WRITE_FLAGS  VIR_DOMAIN_XML_SECURE
#define VIR_DOMAIN_XML_READ_FLAGS   VIR_DOMAIN_XML_INACTIVE

static void
virDomainObjListDataFree(void *payload, const void *name ATTRIBUTE_UNUSED)
{
    virDomainObjPtr obj = payload;
    virDomainObjLock(obj);
    if (virDomainObjUnref(obj) > 0)
        virDomainObjUnlock(obj);
}

int virDomainObjListInit(virDomainObjListPtr doms)
{
    doms->objs = virHashCreate(50, virDomainObjListDataFree);
    if (!doms->objs)
        return -1;
    return 0;
}


void virDomainObjListDeinit(virDomainObjListPtr doms)
{
    virHashFree(doms->objs);
}


static int virDomainObjListSearchID(const void *payload,
                                    const void *name ATTRIBUTE_UNUSED,
                                    const void *data)
{
    virDomainObjPtr obj = (virDomainObjPtr)payload;
    const int *id = data;
    int want = 0;

    virDomainObjLock(obj);
    if (virDomainObjIsActive(obj) &&
        obj->def->id == *id)
        want = 1;
    virDomainObjUnlock(obj);
    return want;
}

virDomainObjPtr virDomainFindByID(const virDomainObjListPtr doms,
                                  int id)
{
    virDomainObjPtr obj;
    obj = virHashSearch(doms->objs, virDomainObjListSearchID, &id);
    if (obj)
        virDomainObjLock(obj);
    return obj;
}


virDomainObjPtr virDomainFindByUUID(const virDomainObjListPtr doms,
                                    const unsigned char *uuid)
{
    char uuidstr[VIR_UUID_STRING_BUFLEN];
    virDomainObjPtr obj;

    virUUIDFormat(uuid, uuidstr);

    obj = virHashLookup(doms->objs, uuidstr);
    if (obj)
        virDomainObjLock(obj);
    return obj;
}

static int virDomainObjListSearchName(const void *payload,
                                      const void *name ATTRIBUTE_UNUSED,
                                      const void *data)
{
    virDomainObjPtr obj = (virDomainObjPtr)payload;
    int want = 0;

    virDomainObjLock(obj);
    if (STREQ(obj->def->name, (const char *)data))
        want = 1;
    virDomainObjUnlock(obj);
    return want;
}

virDomainObjPtr virDomainFindByName(const virDomainObjListPtr doms,
                                    const char *name)
{
    virDomainObjPtr obj;
    obj = virHashSearch(doms->objs, virDomainObjListSearchName, name);
    if (obj)
        virDomainObjLock(obj);
    return obj;
}


bool virDomainObjTaint(virDomainObjPtr obj,
                       enum virDomainTaintFlags taint)
{
    unsigned int flag = (1 << taint);

    if (obj->taint & flag)
        return false;

    obj->taint |= flag;
    return true;
}


static void
virDomainGraphicsAuthDefClear(virDomainGraphicsAuthDefPtr def)
{
    if (!def)
        return;

    VIR_FREE(def->passwd);

    /* Don't free def */
}

static void
virDomainGraphicsListenDefClear(virDomainGraphicsListenDefPtr def)
{
    if (!def)
        return;

    VIR_FREE(def->address);
    VIR_FREE(def->network);
    return;
}

void virDomainGraphicsDefFree(virDomainGraphicsDefPtr def)
{
    int ii;

    if (!def)
        return;

    switch (def->type) {
    case VIR_DOMAIN_GRAPHICS_TYPE_VNC:
        VIR_FREE(def->data.vnc.socket);
        VIR_FREE(def->data.vnc.keymap);
        virDomainGraphicsAuthDefClear(&def->data.vnc.auth);
        break;

    case VIR_DOMAIN_GRAPHICS_TYPE_SDL:
        VIR_FREE(def->data.sdl.display);
        VIR_FREE(def->data.sdl.xauth);
        break;

    case VIR_DOMAIN_GRAPHICS_TYPE_RDP:
        break;

    case VIR_DOMAIN_GRAPHICS_TYPE_DESKTOP:
        VIR_FREE(def->data.desktop.display);
        break;

    case VIR_DOMAIN_GRAPHICS_TYPE_SPICE:
        VIR_FREE(def->data.spice.keymap);
        virDomainGraphicsAuthDefClear(&def->data.spice.auth);
        break;
    }

    for (ii = 0; ii < def->nListens; ii++)
        virDomainGraphicsListenDefClear(&def->listens[ii]);
    VIR_FREE(def->listens);

    VIR_FREE(def);
}

void virDomainInputDefFree(virDomainInputDefPtr def)
{
    if (!def)
        return;

    virDomainDeviceInfoClear(&def->info);
    VIR_FREE(def);
}

static void virDomainLeaseDefFree(virDomainLeaseDefPtr def)
{
    if (!def)
        return;

    VIR_FREE(def->lockspace);
    VIR_FREE(def->key);
    VIR_FREE(def->path);

    VIR_FREE(def);
}

void virDomainDiskDefFree(virDomainDiskDefPtr def)
{
    unsigned int i;

    if (!def)
        return;

    VIR_FREE(def->serial);
    VIR_FREE(def->src);
    VIR_FREE(def->dst);
    VIR_FREE(def->driverName);
    VIR_FREE(def->driverType);
    virStorageEncryptionFree(def->encryption);
    virDomainDeviceInfoClear(&def->info);

    for (i = 0 ; i < def->nhosts ; i++)
        virDomainDiskHostDefFree(&def->hosts[i]);
    VIR_FREE(def->hosts);

    VIR_FREE(def);
}

void virDomainDiskHostDefFree(virDomainDiskHostDefPtr def)
{
    if (!def)
        return;

    VIR_FREE(def->name);
    VIR_FREE(def->port);
}

void virDomainControllerDefFree(virDomainControllerDefPtr def)
{
    if (!def)
        return;

    virDomainDeviceInfoClear(&def->info);

    VIR_FREE(def);
}

void virDomainFSDefFree(virDomainFSDefPtr def)
{
    if (!def)
        return;

    VIR_FREE(def->src);
    VIR_FREE(def->dst);
    virDomainDeviceInfoClear(&def->info);

    VIR_FREE(def);
}

void
virDomainActualNetDefFree(virDomainActualNetDefPtr def)
{
    if (!def)
        return;

    switch (def->type) {
    case VIR_DOMAIN_NET_TYPE_BRIDGE:
        VIR_FREE(def->data.bridge.brname);
        break;
    case VIR_DOMAIN_NET_TYPE_DIRECT:
        VIR_FREE(def->data.direct.linkdev);
        VIR_FREE(def->data.direct.vf_pci_addr);
        VIR_FREE(def->data.direct.virtPortProfile);
        break;
    default:
        break;
    }

    virBandwidthDefFree(def->bandwidth);

    VIR_FREE(def);
}

void virDomainNetDefFree(virDomainNetDefPtr def)
{
    if (!def)
        return;

    VIR_FREE(def->model);

    switch (def->type) {
    case VIR_DOMAIN_NET_TYPE_ETHERNET:
        VIR_FREE(def->data.ethernet.dev);
        VIR_FREE(def->data.ethernet.script);
        VIR_FREE(def->data.ethernet.ipaddr);
        break;

    case VIR_DOMAIN_NET_TYPE_SERVER:
    case VIR_DOMAIN_NET_TYPE_CLIENT:
    case VIR_DOMAIN_NET_TYPE_MCAST:
        VIR_FREE(def->data.socket.address);
        break;

    case VIR_DOMAIN_NET_TYPE_NETWORK:
        VIR_FREE(def->data.network.name);
        VIR_FREE(def->data.network.portgroup);
        VIR_FREE(def->data.network.virtPortProfile);
        virDomainActualNetDefFree(def->data.network.actual);
        break;

    case VIR_DOMAIN_NET_TYPE_BRIDGE:
        VIR_FREE(def->data.bridge.brname);
        VIR_FREE(def->data.bridge.script);
        VIR_FREE(def->data.bridge.ipaddr);
        break;

    case VIR_DOMAIN_NET_TYPE_INTERNAL:
        VIR_FREE(def->data.internal.name);
        break;

    case VIR_DOMAIN_NET_TYPE_DIRECT:
        VIR_FREE(def->data.direct.linkdev);
        VIR_FREE(def->data.direct.vf_pci_addr);
        VIR_FREE(def->data.direct.virtPortProfile);
        break;

    case VIR_DOMAIN_NET_TYPE_USER:
    case VIR_DOMAIN_NET_TYPE_LAST:
        break;
    }

    VIR_FREE(def->ifname);

    virDomainDeviceInfoClear(&def->info);

    VIR_FREE(def->filter);
    virNWFilterHashTableFree(def->filterparams);

    virBandwidthDefFree(def->bandwidth);

    VIR_FREE(def);
}

static void ATTRIBUTE_NONNULL(1)
virDomainChrSourceDefClear(virDomainChrSourceDefPtr def)
{
    switch (def->type) {
    case VIR_DOMAIN_CHR_TYPE_PTY:
    case VIR_DOMAIN_CHR_TYPE_DEV:
    case VIR_DOMAIN_CHR_TYPE_FILE:
    case VIR_DOMAIN_CHR_TYPE_PIPE:
        VIR_FREE(def->data.file.path);
        break;

    case VIR_DOMAIN_CHR_TYPE_UDP:
        VIR_FREE(def->data.udp.bindHost);
        VIR_FREE(def->data.udp.bindService);
        VIR_FREE(def->data.udp.connectHost);
        VIR_FREE(def->data.udp.connectService);
        break;

    case VIR_DOMAIN_CHR_TYPE_TCP:
        VIR_FREE(def->data.tcp.host);
        VIR_FREE(def->data.tcp.service);
        break;

    case VIR_DOMAIN_CHR_TYPE_UNIX:
        VIR_FREE(def->data.nix.path);
        break;
    }
}

void virDomainChrSourceDefFree(virDomainChrSourceDefPtr def)
{
    if (!def)
        return;

    virDomainChrSourceDefClear(def);

    VIR_FREE(def);
}

void virDomainChrDefFree(virDomainChrDefPtr def)
{
    if (!def)
        return;

    switch (def->deviceType) {
    case VIR_DOMAIN_CHR_DEVICE_TYPE_CHANNEL:
        switch (def->targetType) {
        case VIR_DOMAIN_CHR_CHANNEL_TARGET_TYPE_GUESTFWD:
            VIR_FREE(def->target.addr);
            break;

        case VIR_DOMAIN_CHR_CHANNEL_TARGET_TYPE_VIRTIO:
            VIR_FREE(def->target.name);
            break;
        }
        break;

    default:
        break;
    }

    virDomainChrSourceDefClear(&def->source);
    virDomainDeviceInfoClear(&def->info);

    VIR_FREE(def);
}

void virDomainSmartcardDefFree(virDomainSmartcardDefPtr def)
{
    size_t i;
    if (!def)
        return;

    switch (def->type) {
    case VIR_DOMAIN_SMARTCARD_TYPE_HOST:
        break;

    case VIR_DOMAIN_SMARTCARD_TYPE_HOST_CERTIFICATES:
        for (i = 0; i < VIR_DOMAIN_SMARTCARD_NUM_CERTIFICATES; i++)
            VIR_FREE(def->data.cert.file[i]);
        VIR_FREE(def->data.cert.database);
        break;

    case VIR_DOMAIN_SMARTCARD_TYPE_PASSTHROUGH:
        virDomainChrSourceDefClear(&def->data.passthru);
        break;

    default:
        break;
    }

    virDomainDeviceInfoClear(&def->info);

    VIR_FREE(def);
}

void virDomainSoundDefFree(virDomainSoundDefPtr def)
{
    if (!def)
        return;

    virDomainDeviceInfoClear(&def->info);

    VIR_FREE(def);
}

void virDomainMemballoonDefFree(virDomainMemballoonDefPtr def)
{
    if (!def)
        return;

    virDomainDeviceInfoClear(&def->info);

    VIR_FREE(def);
}

void virDomainWatchdogDefFree(virDomainWatchdogDefPtr def)
{
    if (!def)
        return;

    virDomainDeviceInfoClear(&def->info);

    VIR_FREE(def);
}

void virDomainVideoDefFree(virDomainVideoDefPtr def)
{
    if (!def)
        return;

    virDomainDeviceInfoClear(&def->info);

    VIR_FREE(def->accel);
    VIR_FREE(def);
}

void virDomainHostdevDefFree(virDomainHostdevDefPtr def)
{
    if (!def)
        return;

    virDomainDeviceInfoClear(&def->info);
    VIR_FREE(def);
}

void virDomainDeviceDefFree(virDomainDeviceDefPtr def)
{
    if (!def)
        return;

    switch (def->type) {
    case VIR_DOMAIN_DEVICE_DISK:
        virDomainDiskDefFree(def->data.disk);
        break;
    case VIR_DOMAIN_DEVICE_LEASE:
        virDomainLeaseDefFree(def->data.lease);
        break;
    case VIR_DOMAIN_DEVICE_NET:
        virDomainNetDefFree(def->data.net);
        break;
    case VIR_DOMAIN_DEVICE_INPUT:
        virDomainInputDefFree(def->data.input);
        break;
    case VIR_DOMAIN_DEVICE_SOUND:
        virDomainSoundDefFree(def->data.sound);
        break;
    case VIR_DOMAIN_DEVICE_VIDEO:
        virDomainVideoDefFree(def->data.video);
        break;
    case VIR_DOMAIN_DEVICE_HOSTDEV:
        virDomainHostdevDefFree(def->data.hostdev);
        break;
    case VIR_DOMAIN_DEVICE_WATCHDOG:
        virDomainWatchdogDefFree(def->data.watchdog);
        break;
    case VIR_DOMAIN_DEVICE_CONTROLLER:
        virDomainControllerDefFree(def->data.controller);
        break;
    case VIR_DOMAIN_DEVICE_GRAPHICS:
        virDomainGraphicsDefFree(def->data.graphics);
        break;
    }

    VIR_FREE(def);
}

void virSecurityLabelDefFree(virDomainDefPtr def);

void virSecurityLabelDefFree(virDomainDefPtr def)
{
    VIR_FREE(def->seclabel.model);
    VIR_FREE(def->seclabel.label);
    VIR_FREE(def->seclabel.imagelabel);
    VIR_FREE(def->seclabel.baselabel);
}

static void
virDomainClockDefClear(virDomainClockDefPtr def)
{
    if (def->offset == VIR_DOMAIN_CLOCK_OFFSET_TIMEZONE)
        VIR_FREE(def->data.timezone);

    int i;
    for (i = 0; i < def->ntimers; i++)
        VIR_FREE(def->timers[i]);
    VIR_FREE(def->timers);
}

static void
virDomainVcpuPinDefFree(virDomainVcpuPinDefPtr *def,
                        int nvcpupin)
{
    int i;

    if (!def || !nvcpupin)
        return;

    for(i = 0; i < nvcpupin; i++) {
        VIR_FREE(def[i]->cpumask);
        VIR_FREE(def[i]);
    }

    VIR_FREE(def);
}

void virDomainDefFree(virDomainDefPtr def)
{
    unsigned int i;

    if (!def)
        return;

    for (i = 0 ; i < def->nleases ; i++)
        virDomainLeaseDefFree(def->leases[i]);
    VIR_FREE(def->leases);

    for (i = 0 ; i < def->ngraphics ; i++)
        virDomainGraphicsDefFree(def->graphics[i]);
    VIR_FREE(def->graphics);

    for (i = 0 ; i < def->ninputs ; i++)
        virDomainInputDefFree(def->inputs[i]);
    VIR_FREE(def->inputs);

    for (i = 0 ; i < def->ndisks ; i++)
        virDomainDiskDefFree(def->disks[i]);
    VIR_FREE(def->disks);

    for (i = 0 ; i < def->ncontrollers ; i++)
        virDomainControllerDefFree(def->controllers[i]);
    VIR_FREE(def->controllers);

    for (i = 0 ; i < def->nfss ; i++)
        virDomainFSDefFree(def->fss[i]);
    VIR_FREE(def->fss);

    for (i = 0 ; i < def->nnets ; i++)
        virDomainNetDefFree(def->nets[i]);
    VIR_FREE(def->nets);

    for (i = 0 ; i < def->nsmartcards ; i++)
        virDomainSmartcardDefFree(def->smartcards[i]);
    VIR_FREE(def->smartcards);

    for (i = 0 ; i < def->nserials ; i++)
        virDomainChrDefFree(def->serials[i]);
    VIR_FREE(def->serials);

    for (i = 0 ; i < def->nparallels ; i++)
        virDomainChrDefFree(def->parallels[i]);
    VIR_FREE(def->parallels);

    for (i = 0 ; i < def->nchannels ; i++)
        virDomainChrDefFree(def->channels[i]);
    VIR_FREE(def->channels);

    virDomainChrDefFree(def->console);

    for (i = 0 ; i < def->nsounds ; i++)
        virDomainSoundDefFree(def->sounds[i]);
    VIR_FREE(def->sounds);

    for (i = 0 ; i < def->nvideos ; i++)
        virDomainVideoDefFree(def->videos[i]);
    VIR_FREE(def->videos);

    for (i = 0 ; i < def->nhostdevs ; i++)
        virDomainHostdevDefFree(def->hostdevs[i]);
    VIR_FREE(def->hostdevs);

    VIR_FREE(def->os.type);
    VIR_FREE(def->os.arch);
    VIR_FREE(def->os.machine);
    VIR_FREE(def->os.init);
    VIR_FREE(def->os.kernel);
    VIR_FREE(def->os.initrd);
    VIR_FREE(def->os.cmdline);
    VIR_FREE(def->os.root);
    VIR_FREE(def->os.loader);
    VIR_FREE(def->os.bootloader);
    VIR_FREE(def->os.bootloaderArgs);

    virDomainClockDefClear(&def->clock);

    VIR_FREE(def->name);
    VIR_FREE(def->cpumask);
    VIR_FREE(def->emulator);
    VIR_FREE(def->description);

    virDomainWatchdogDefFree(def->watchdog);

    virDomainMemballoonDefFree(def->memballoon);

    virSecurityLabelDefFree(def);

    virCPUDefFree(def->cpu);

    virDomainVcpuPinDefFree(def->cputune.vcpupin, def->cputune.nvcpupin);

    VIR_FREE(def->numatune.memory.nodemask);

    virSysinfoDefFree(def->sysinfo);

    if (def->namespaceData && def->ns.free)
        (def->ns.free)(def->namespaceData);

    VIR_FREE(def);
}

static void virDomainSnapshotObjListDeinit(virDomainSnapshotObjListPtr snapshots);
static void virDomainObjFree(virDomainObjPtr dom)
{
    if (!dom)
        return;

    VIR_DEBUG("obj=%p", dom);
    virDomainDefFree(dom->def);
    virDomainDefFree(dom->newDef);

    if (dom->privateDataFreeFunc)
        (dom->privateDataFreeFunc)(dom->privateData);

    virMutexDestroy(&dom->lock);

    virDomainSnapshotObjListDeinit(&dom->snapshots);

    VIR_FREE(dom);
}

void virDomainObjRef(virDomainObjPtr dom)
{
    dom->refs++;
    VIR_DEBUG("obj=%p refs=%d", dom, dom->refs);
}


int virDomainObjUnref(virDomainObjPtr dom)
{
    dom->refs--;
    VIR_DEBUG("obj=%p refs=%d", dom, dom->refs);
    if (dom->refs == 0) {
        virDomainObjUnlock(dom);
        virDomainObjFree(dom);
        return 0;
    }
    return dom->refs;
}

static virDomainObjPtr virDomainObjNew(virCapsPtr caps)
{
    virDomainObjPtr domain;

    if (VIR_ALLOC(domain) < 0) {
        virReportOOMError();
        return NULL;
    }

    if (caps->privateDataAllocFunc &&
        !(domain->privateData = (caps->privateDataAllocFunc)())) {
        virReportOOMError();
        VIR_FREE(domain);
        return NULL;
    }
    domain->privateDataFreeFunc = caps->privateDataFreeFunc;

    if (virMutexInit(&domain->lock) < 0) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                             "%s", _("cannot initialize mutex"));
        if (domain->privateDataFreeFunc)
            (domain->privateDataFreeFunc)(domain->privateData);
        VIR_FREE(domain);
        return NULL;
    }

    virDomainObjLock(domain);
    virDomainObjSetState(domain, VIR_DOMAIN_SHUTOFF,
                                 VIR_DOMAIN_SHUTOFF_UNKNOWN);
    domain->refs = 1;

    virDomainSnapshotObjListInit(&domain->snapshots);

    VIR_DEBUG("obj=%p", domain);
    return domain;
}

void virDomainObjAssignDef(virDomainObjPtr domain,
                           const virDomainDefPtr def,
                           bool live)
{
    if (!virDomainObjIsActive(domain)) {
        if (live) {
            /* save current configuration to be restored on domain shutdown */
            if (!domain->newDef)
                domain->newDef = domain->def;
            domain->def = def;
        } else {
            virDomainDefFree(domain->def);
            domain->def = def;
        }
    } else {
        virDomainDefFree(domain->newDef);
        domain->newDef = def;
    }
}

virDomainObjPtr virDomainAssignDef(virCapsPtr caps,
                                   virDomainObjListPtr doms,
                                   const virDomainDefPtr def,
                                   bool live)
{
    virDomainObjPtr domain;
    char uuidstr[VIR_UUID_STRING_BUFLEN];

    if ((domain = virDomainFindByUUID(doms, def->uuid))) {
        virDomainObjAssignDef(domain, def, live);
        return domain;
    }

    if (!(domain = virDomainObjNew(caps)))
        return NULL;
    domain->def = def;

    virUUIDFormat(def->uuid, uuidstr);
    if (virHashAddEntry(doms->objs, uuidstr, domain) < 0) {
        VIR_FREE(domain);
        return NULL;
    }

    return domain;
}

/*
 * Mark the running VM config as transient. Ensures transient hotplug
 * operations do not persist past shutdown.
 *
 * @param caps pointer to capabilities info
 * @param domain domain object pointer
 * @param live if true, run this operation even for an inactive domain.
 *   this allows freely updated domain->def with runtime defaults before
 *   starting the VM, which will be discarded on VM shutdown. Any cleanup
 *   paths need to be sure to handle newDef if the domain is never started.
 * @return 0 on success, -1 on failure
 */
int
virDomainObjSetDefTransient(virCapsPtr caps,
                            virDomainObjPtr domain,
                            bool live)
{
    int ret = -1;
    char *xml = NULL;
    virDomainDefPtr newDef = NULL;

    if (!virDomainObjIsActive(domain) && !live)
        return 0;

    if (!domain->persistent)
        return 0;

    if (domain->newDef)
        return 0;

    if (!(xml = virDomainDefFormat(domain->def, VIR_DOMAIN_XML_WRITE_FLAGS)))
        goto out;

    if (!(newDef = virDomainDefParseString(caps, xml, -1,
                                           VIR_DOMAIN_XML_READ_FLAGS)))
        goto out;

    domain->newDef = newDef;
    ret = 0;
out:
    VIR_FREE(xml);
    return ret;
}

/*
 * Return the persistent domain configuration. If domain is transient,
 * return the running config.
 *
 * @param caps pointer to capabilities info
 * @param domain domain object pointer
 * @return NULL on error, virDOmainDefPtr on success
 */
virDomainDefPtr
virDomainObjGetPersistentDef(virCapsPtr caps,
                             virDomainObjPtr domain)
{
    if (virDomainObjSetDefTransient(caps, domain, false) < 0)
        return NULL;

    if (domain->newDef)
        return domain->newDef;
    else
        return domain->def;
}

/*
 * The caller must hold a lock on the driver owning 'doms',
 * and must also have locked 'dom', to ensure no one else
 * is either waiting for 'dom' or still using it
 */
void virDomainRemoveInactive(virDomainObjListPtr doms,
                             virDomainObjPtr dom)
{
    char uuidstr[VIR_UUID_STRING_BUFLEN];
    virUUIDFormat(dom->def->uuid, uuidstr);

    virDomainObjUnlock(dom);

    virHashRemoveEntry(doms->objs, uuidstr);
}


int virDomainDeviceAddressIsValid(virDomainDeviceInfoPtr info,
                                  int type)
{
    if (info->type != type)
        return 0;

    switch (info->type) {
    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI:
        return virDomainDevicePCIAddressIsValid(&info->addr.pci);

    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_DRIVE:
        return virDomainDeviceDriveAddressIsValid(&info->addr.drive);
    }

    return 0;
}


int virDomainDevicePCIAddressIsValid(virDomainDevicePCIAddressPtr addr)
{
    /* PCI bus has 32 slots and 8 functions per slot */
    if (addr->slot >= 32 || addr->function >= 8)
        return 0;
    return addr->domain || addr->bus || addr->slot;
}


int virDomainDeviceDriveAddressIsValid(virDomainDeviceDriveAddressPtr addr ATTRIBUTE_UNUSED)
{
    /*return addr->controller || addr->bus || addr->unit;*/
    return 1; /* 0 is valid for all fields, so any successfully parsed addr is valid */
}


int virDomainDeviceVirtioSerialAddressIsValid(
    virDomainDeviceVirtioSerialAddressPtr addr ATTRIBUTE_UNUSED)
{
    return 1; /* 0 is valid for all fields, so any successfully parsed addr is valid */
}


static int
virDomainDeviceInfoIsSet(virDomainDeviceInfoPtr info, unsigned int flags)
{
    if (info->type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_NONE)
        return 1;
    if (info->alias && !(flags & VIR_DOMAIN_XML_INACTIVE))
        return 1;
    return 0;
}


void virDomainDeviceInfoClear(virDomainDeviceInfoPtr info)
{
    VIR_FREE(info->alias);
    memset(&info->addr, 0, sizeof(info->addr));
    info->type = VIR_DOMAIN_DEVICE_ADDRESS_TYPE_NONE;
}


static int virDomainDeviceInfoClearAlias(virDomainDefPtr def ATTRIBUTE_UNUSED,
                                         virDomainDeviceInfoPtr info,
                                         void *opaque ATTRIBUTE_UNUSED)
{
    VIR_FREE(info->alias);
    return 0;
}

static int virDomainDeviceInfoClearPCIAddress(virDomainDefPtr def ATTRIBUTE_UNUSED,
                                              virDomainDeviceInfoPtr info,
                                              void *opaque ATTRIBUTE_UNUSED)
{
    if (info->type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI) {
        memset(&info->addr, 0, sizeof(info->addr));
        info->type = VIR_DOMAIN_DEVICE_ADDRESS_TYPE_NONE;
    }
    return 0;
}

int virDomainDeviceInfoIterate(virDomainDefPtr def,
                               virDomainDeviceInfoCallback cb,
                               void *opaque)
{
    int i;

    for (i = 0; i < def->ndisks ; i++)
        if (cb(def, &def->disks[i]->info, opaque) < 0)
            return -1;
    for (i = 0; i < def->nnets ; i++)
        if (cb(def, &def->nets[i]->info, opaque) < 0)
            return -1;
    for (i = 0; i < def->nsounds ; i++)
        if (cb(def, &def->sounds[i]->info, opaque) < 0)
            return -1;
    for (i = 0; i < def->nhostdevs ; i++)
        if (cb(def, &def->hostdevs[i]->info, opaque) < 0)
            return -1;
    for (i = 0; i < def->nvideos ; i++)
        if (cb(def, &def->videos[i]->info, opaque) < 0)
            return -1;
    for (i = 0; i < def->ncontrollers ; i++)
        if (cb(def, &def->controllers[i]->info, opaque) < 0)
            return -1;
    for (i = 0; i < def->nsmartcards ; i++)
        if (cb(def, &def->smartcards[i]->info, opaque) < 0)
            return -1;
    for (i = 0; i < def->nserials ; i++)
        if (cb(def, &def->serials[i]->info, opaque) < 0)
            return -1;
    for (i = 0; i < def->nparallels ; i++)
        if (cb(def, &def->parallels[i]->info, opaque) < 0)
            return -1;
    for (i = 0; i < def->nchannels ; i++)
        if (cb(def, &def->channels[i]->info, opaque) < 0)
            return -1;
    for (i = 0; i < def->ninputs ; i++)
        if (cb(def, &def->inputs[i]->info, opaque) < 0)
            return -1;
    for (i = 0; i < def->nfss ; i++)
        if (cb(def, &def->fss[i]->info, opaque) < 0)
            return -1;
    if (def->watchdog)
        if (cb(def, &def->watchdog->info, opaque) < 0)
            return -1;
    if (def->memballoon)
        if (cb(def, &def->memballoon->info, opaque) < 0)
            return -1;
    if (def->console)
        if (cb(def, &def->console->info, opaque) < 0)
            return -1;
    return 0;
}


void virDomainDefClearPCIAddresses(virDomainDefPtr def)
{
    virDomainDeviceInfoIterate(def, virDomainDeviceInfoClearPCIAddress, NULL);
}

void virDomainDefClearDeviceAliases(virDomainDefPtr def)
{
    virDomainDeviceInfoIterate(def, virDomainDeviceInfoClearAlias, NULL);
}


/* Generate a string representation of a device address
 * @param address Device address to stringify
 */
static int ATTRIBUTE_NONNULL(2)
virDomainDeviceInfoFormat(virBufferPtr buf,
                          virDomainDeviceInfoPtr info,
                          unsigned int flags)
{
    if (info->alias &&
        !(flags & VIR_DOMAIN_XML_INACTIVE)) {
        virBufferAsprintf(buf, "      <alias name='%s'/>\n", info->alias);
    }

    if (info->type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_NONE)
        return 0;

    /* We'll be in domain/devices/[device type]/ so 3 level indent */
    virBufferAsprintf(buf, "      <address type='%s'",
                      virDomainDeviceAddressTypeToString(info->type));

    switch (info->type) {
    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI:
        virBufferAsprintf(buf, " domain='0x%.4x' bus='0x%.2x' slot='0x%.2x' function='0x%.1x'",
                          info->addr.pci.domain,
                          info->addr.pci.bus,
                          info->addr.pci.slot,
                          info->addr.pci.function);
        if (info->addr.pci.multi) {
           virBufferAsprintf(buf, " multifunction='%s'",
                             virDomainDeviceAddressPciMultiTypeToString(info->addr.pci.multi));
        }
        break;

    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_DRIVE:
        virBufferAsprintf(buf, " controller='%d' bus='%d' unit='%d'",
                          info->addr.drive.controller,
                          info->addr.drive.bus,
                          info->addr.drive.unit);
        break;

    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_VIRTIO_SERIAL:
        virBufferAsprintf(buf, " controller='%d' bus='%d' port='%d'",
                          info->addr.vioserial.controller,
                          info->addr.vioserial.bus,
                          info->addr.vioserial.port);
        break;

    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_CCID:
        virBufferAsprintf(buf, " controller='%d' slot='%d'",
                          info->addr.ccid.controller,
                          info->addr.ccid.slot);
        break;

    default:
        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                             _("unknown address type '%d'"), info->type);
        return -1;
    }

    virBufferAddLit(buf, "/>\n");

    return 0;
}


static int
virDomainDevicePCIAddressParseXML(xmlNodePtr node,
                                  virDomainDevicePCIAddressPtr addr)
{
    char *domain, *slot, *bus, *function, *multi;
    int ret = -1;

    memset(addr, 0, sizeof(*addr));

    domain   = virXMLPropString(node, "domain");
    bus      = virXMLPropString(node, "bus");
    slot     = virXMLPropString(node, "slot");
    function = virXMLPropString(node, "function");
    multi    = virXMLPropString(node, "multifunction");

    if (domain &&
        virStrToLong_ui(domain, NULL, 0, &addr->domain) < 0) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                             _("Cannot parse <address> 'domain' attribute"));
        goto cleanup;
    }

    if (bus &&
        virStrToLong_ui(bus, NULL, 0, &addr->bus) < 0) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                             _("Cannot parse <address> 'bus' attribute"));
        goto cleanup;
    }

    if (slot &&
        virStrToLong_ui(slot, NULL, 0, &addr->slot) < 0) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                             _("Cannot parse <address> 'slot' attribute"));
        goto cleanup;
    }

    if (function &&
        virStrToLong_ui(function, NULL, 0, &addr->function) < 0) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                             _("Cannot parse <address> 'function' attribute"));
        goto cleanup;
    }

    if (multi &&
        ((addr->multi = virDomainDeviceAddressPciMultiTypeFromString(multi)) <= 0)) {
        virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                             _("Unknown value '%s' for <address> 'multifunction' attribute"),
                             multi);
        goto cleanup;

    }
    if (!virDomainDevicePCIAddressIsValid(addr)) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                             _("Insufficient specification for PCI address"));
        goto cleanup;
    }

    ret = 0;

cleanup:
    VIR_FREE(domain);
    VIR_FREE(bus);
    VIR_FREE(slot);
    VIR_FREE(function);
    VIR_FREE(multi);
    return ret;
}


static int
virDomainDeviceDriveAddressParseXML(xmlNodePtr node,
                                    virDomainDeviceDriveAddressPtr addr)
{
    char *bus, *unit, *controller;
    int ret = -1;

    memset(addr, 0, sizeof(*addr));

    controller = virXMLPropString(node, "controller");
    bus = virXMLPropString(node, "bus");
    unit = virXMLPropString(node, "unit");

    if (controller &&
        virStrToLong_ui(controller, NULL, 10, &addr->controller) < 0) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                             _("Cannot parse <address> 'controller' attribute"));
        goto cleanup;
    }

    if (bus &&
        virStrToLong_ui(bus, NULL, 10, &addr->bus) < 0) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                             _("Cannot parse <address> 'bus' attribute"));
        goto cleanup;
    }

    if (unit &&
        virStrToLong_ui(unit, NULL, 10, &addr->unit) < 0) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                             _("Cannot parse <address> 'unit' attribute"));
        goto cleanup;
    }

    if (!virDomainDeviceDriveAddressIsValid(addr)) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                             _("Insufficient specification for drive address"));
        goto cleanup;
    }

    ret = 0;

cleanup:
    VIR_FREE(controller);
    VIR_FREE(bus);
    VIR_FREE(unit);
    return ret;
}


static int
virDomainDeviceVirtioSerialAddressParseXML(
    xmlNodePtr node,
    virDomainDeviceVirtioSerialAddressPtr addr
)
{
    char *controller, *bus, *port;
    int ret = -1;

    memset(addr, 0, sizeof(*addr));

    controller = virXMLPropString(node, "controller");
    bus = virXMLPropString(node, "bus");
    port = virXMLPropString(node, "port");

    if (controller &&
        virStrToLong_ui(controller, NULL, 10, &addr->controller) < 0) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                             _("Cannot parse <address> 'controller' attribute"));
        goto cleanup;
    }

    if (bus &&
        virStrToLong_ui(bus, NULL, 10, &addr->bus) < 0) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                             _("Cannot parse <address> 'bus' attribute"));
        goto cleanup;
    }

    if (port &&
        virStrToLong_ui(port, NULL, 10, &addr->port) < 0) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                             _("Cannot parse <address> 'port' attribute"));
        goto cleanup;
    }

    if (!virDomainDeviceVirtioSerialAddressIsValid(addr)) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                             _("Insufficient specification for "
                               "virtio serial address"));
        goto cleanup;
    }

    ret = 0;

cleanup:
    VIR_FREE(controller);
    VIR_FREE(bus);
    VIR_FREE(port);
    return ret;
}

static int
virDomainDeviceCcidAddressParseXML(xmlNodePtr node,
                                   virDomainDeviceCcidAddressPtr addr)
{
    char *controller, *slot;
    int ret = -1;

    memset(addr, 0, sizeof(*addr));

    controller = virXMLPropString(node, "controller");
    slot = virXMLPropString(node, "slot");

    if (controller &&
        virStrToLong_ui(controller, NULL, 10, &addr->controller) < 0) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                             _("Cannot parse <address> 'controller' attribute"));
        goto cleanup;
    }

    if (slot &&
        virStrToLong_ui(slot, NULL, 10, &addr->slot) < 0) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                             _("Cannot parse <address> 'slot' attribute"));
        goto cleanup;
    }

    ret = 0;

cleanup:
    VIR_FREE(controller);
    VIR_FREE(slot);
    return ret;
}

/* Parse the XML definition for a device address
 * @param node XML nodeset to parse for device address definition
 */
static int
virDomainDeviceInfoParseXML(xmlNodePtr node,
                            virDomainDeviceInfoPtr info,
                            unsigned int flags)
{
    xmlNodePtr cur;
    xmlNodePtr address = NULL;
    xmlNodePtr alias = NULL;
    char *type = NULL;
    int ret = -1;

    virDomainDeviceInfoClear(info);

    cur = node->children;
    while (cur != NULL) {
        if (cur->type == XML_ELEMENT_NODE) {
            if (alias == NULL &&
                !(flags & VIR_DOMAIN_XML_INACTIVE) &&
                xmlStrEqual(cur->name, BAD_CAST "alias")) {
                alias = cur;
            } else if (address == NULL &&
                       xmlStrEqual(cur->name, BAD_CAST "address")) {
                address = cur;
            }
        }
        cur = cur->next;
    }

    if (alias)
        info->alias = virXMLPropString(alias, "name");

    if (!address)
        return 0;

    type = virXMLPropString(address, "type");

    if (type) {
        if ((info->type = virDomainDeviceAddressTypeFromString(type)) < 0) {
            virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                 _("unknown address type '%s'"), type);
            goto cleanup;
        }
    } else {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                             "%s", _("No type specified for device address"));
        goto cleanup;
    }

    switch (info->type) {
    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI:
        if (virDomainDevicePCIAddressParseXML(address, &info->addr.pci) < 0)
            goto cleanup;
        break;

    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_DRIVE:
        if (virDomainDeviceDriveAddressParseXML(address, &info->addr.drive) < 0)
            goto cleanup;
        break;

    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_VIRTIO_SERIAL:
        if (virDomainDeviceVirtioSerialAddressParseXML
                (address, &info->addr.vioserial) < 0)
            goto cleanup;
        break;

    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_CCID:
        if (virDomainDeviceCcidAddressParseXML(address, &info->addr.ccid) < 0)
            goto cleanup;
        break;

    default:
        /* Should not happen */
        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                             "%s", _("Unknown device address type"));
        goto cleanup;
    }

    ret = 0;

cleanup:
    if (ret == -1)
        VIR_FREE(info->alias);
    VIR_FREE(type);
    return ret;
}

static int
virDomainDeviceBootParseXML(xmlNodePtr node,
                            int *bootIndex,
                            virBitmapPtr bootMap)
{
    char *order;
    int boot;
    int ret = -1;

    order = virXMLPropString(node, "order");
    if (!order) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                            "%s", _("missing boot order attribute"));
        goto cleanup;
    } else if (virStrToLong_i(order, NULL, 10, &boot) < 0 ||
               boot <= 0) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                _("incorrect boot order '%s', expecting positive integer"),
                order);
        goto cleanup;
    }

    if (bootMap) {
        bool set;
        if (virBitmapGetBit(bootMap, boot - 1, &set) < 0) {
            virDomainReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                    _("boot orders have to be contiguous and starting from 1"));
            goto cleanup;
        } else if (set) {
            virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                    _("boot order %d used for more than one device"), boot);
            goto cleanup;
        }
        ignore_value(virBitmapSetBit(bootMap, boot - 1));
    }

    *bootIndex = boot;
    ret = 0;

cleanup:
    VIR_FREE(order);
    return ret;
}

static int
virDomainParseLegacyDeviceAddress(char *devaddr,
                                  virDomainDevicePCIAddressPtr pci)
{
    char *tmp;

    /* expected format: <domain>:<bus>:<slot> */
    if (/* domain */
        virStrToLong_ui(devaddr, &tmp, 16, &pci->domain) < 0 || *tmp != ':' ||
        /* bus */
        virStrToLong_ui(tmp + 1, &tmp, 16, &pci->bus) < 0 || *tmp != ':' ||
        /* slot */
        virStrToLong_ui(tmp + 1, NULL, 16, &pci->slot) < 0)
        return -1;

    return 0;
}

int
virDomainDiskDefAssignAddress(virCapsPtr caps, virDomainDiskDefPtr def)
{
    int idx = virDiskNameToIndex(def->dst);
    if (idx < 0)
        return -1;

    switch (def->bus) {
    case VIR_DOMAIN_DISK_BUS_SCSI:
        def->info.type = VIR_DOMAIN_DEVICE_ADDRESS_TYPE_DRIVE;

        if (caps->hasWideScsiBus) {
            /* For a wide SCSI bus we define the default mapping to be
             * 16 units per bus, 1 bus per controller, many controllers.
             * Unit 7 is the SCSI controller itself. Therefore unit 7
             * cannot be assigned to disks and is skipped.
             */
            def->info.addr.drive.controller = idx / 15;
            def->info.addr.drive.bus = 0;
            def->info.addr.drive.unit = idx % 15;

            /* Skip the SCSI controller at unit 7 */
            if (def->info.addr.drive.unit >= 7) {
                ++def->info.addr.drive.unit;
            }
        } else {
            /* For a narrow SCSI bus we define the default mapping to be
             * 7 units per bus, 1 bus per controller, many controllers */
            def->info.addr.drive.controller = idx / 7;
            def->info.addr.drive.bus = 0;
            def->info.addr.drive.unit = idx % 7;
        }

        break;

    case VIR_DOMAIN_DISK_BUS_IDE:
        /* For IDE we define the default mapping to be 2 units
         * per bus, 2 bus per controller, many controllers */
        def->info.type = VIR_DOMAIN_DEVICE_ADDRESS_TYPE_DRIVE;
        def->info.addr.drive.controller = idx / 4;
        def->info.addr.drive.bus = (idx % 4) / 2;
        def->info.addr.drive.unit = (idx % 2);
        break;

    case VIR_DOMAIN_DISK_BUS_FDC:
        /* For FDC we define the default mapping to be 2 units
         * per bus, 1 bus per controller, many controllers */
        def->info.type = VIR_DOMAIN_DEVICE_ADDRESS_TYPE_DRIVE;
        def->info.addr.drive.controller = idx / 2;
        def->info.addr.drive.bus = 0;
        def->info.addr.drive.unit = idx % 2;
        break;

    default:
        /* Other disk bus's aren't controller based */
        break;
    }

    return 0;
}

/* Parse the XML definition for a lease
 */
static virDomainLeaseDefPtr
virDomainLeaseDefParseXML(xmlNodePtr node)
{
    virDomainLeaseDefPtr def;
    xmlNodePtr cur;
    char *lockspace = NULL;
    char *key = NULL;
    char *path = NULL;
    char *offset = NULL;

    if (VIR_ALLOC(def) < 0) {
        virReportOOMError();
        return NULL;
    }

    cur = node->children;
    while (cur != NULL) {
        if (cur->type == XML_ELEMENT_NODE) {
            if ((key == NULL) &&
                (xmlStrEqual(cur->name, BAD_CAST "key"))) {
                key = (char *)xmlNodeGetContent(cur);
            } else if ((lockspace == NULL) &&
                (xmlStrEqual(cur->name, BAD_CAST "lockspace"))) {
                lockspace = (char *)xmlNodeGetContent(cur);
            } else if ((path == NULL) &&
                       (xmlStrEqual(cur->name, BAD_CAST "target"))) {
                path = virXMLPropString(cur, "path");
                offset = virXMLPropString(cur, "offset");
            }
        }
        cur = cur->next;
    }

    if (!key) {
        virDomainReportError(VIR_ERR_XML_ERROR, "%s",
                             _("Missing 'key' element for lease"));
        goto error;
    }
    if (!path) {
        virDomainReportError(VIR_ERR_XML_ERROR, "%s",
                             _("Missing 'target' element for lease"));
        goto error;
    }

    if (offset &&
        virStrToLong_ull(offset, NULL, 10, &def->offset) < 0) {
        virDomainReportError(VIR_ERR_XML_ERROR,
                             _("Malformed lease target offset %s"), offset);
        goto error;
    }

    def->key = key;
    def->lockspace = lockspace;
    def->path = path;
    path = key = lockspace = NULL;

cleanup:
    VIR_FREE(lockspace);
    VIR_FREE(key);
    VIR_FREE(path);
    VIR_FREE(offset);

    return def;

 error:
    virDomainLeaseDefFree(def);
    def = NULL;
    goto cleanup;
}


/* Parse the XML definition for a disk
 * @param node XML nodeset to parse for disk definition
 */
static virDomainDiskDefPtr
virDomainDiskDefParseXML(virCapsPtr caps,
                         xmlNodePtr node,
                         virBitmapPtr bootMap,
                         unsigned int flags)
{
    virDomainDiskDefPtr def;
    xmlNodePtr cur, host;
    char *type = NULL;
    char *device = NULL;
    char *snapshot = NULL;
    char *driverName = NULL;
    char *driverType = NULL;
    char *source = NULL;
    char *target = NULL;
    char *protocol = NULL;
    virDomainDiskHostDefPtr hosts = NULL;
    int nhosts = 0;
    char *bus = NULL;
    char *cachetag = NULL;
    char *error_policy = NULL;
    char *iotag = NULL;
    char *ioeventfd = NULL;
    char *event_idx = NULL;
    char *devaddr = NULL;
    virStorageEncryptionPtr encryption = NULL;
    char *serial = NULL;

    if (VIR_ALLOC(def) < 0) {
        virReportOOMError();
        return NULL;
    }

    type = virXMLPropString(node, "type");
    if (type) {
        if ((def->type = virDomainDiskTypeFromString(type)) < 0) {
            virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                 _("unknown disk type '%s'"), type);
            goto error;
        }
    } else {
        def->type = VIR_DOMAIN_DISK_TYPE_FILE;
    }

    snapshot = virXMLPropString(node, "snapshot");

    cur = node->children;
    while (cur != NULL) {
        if (cur->type == XML_ELEMENT_NODE) {
            if ((source == NULL && hosts == NULL) &&
                (xmlStrEqual(cur->name, BAD_CAST "source"))) {

                switch (def->type) {
                case VIR_DOMAIN_DISK_TYPE_FILE:
                    source = virXMLPropString(cur, "file");
                    break;
                case VIR_DOMAIN_DISK_TYPE_BLOCK:
                    source = virXMLPropString(cur, "dev");
                    break;
                case VIR_DOMAIN_DISK_TYPE_DIR:
                    source = virXMLPropString(cur, "dir");
                    break;
                case VIR_DOMAIN_DISK_TYPE_NETWORK:
                    protocol = virXMLPropString(cur, "protocol");
                    if (protocol == NULL) {
                        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                             "%s", _("missing protocol type"));
                        goto error;
                    }
                    def->protocol = virDomainDiskProtocolTypeFromString(protocol);
                    if (def->protocol < 0) {
                        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                             _("unknown protocol type '%s'"),
                                             protocol);
                        goto error;
                    }
                    if (!(source = virXMLPropString(cur, "name")) &&
                        def->protocol != VIR_DOMAIN_DISK_PROTOCOL_NBD) {
                        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                             _("missing name for disk source"));
                        goto error;
                    }
                    host = cur->children;
                    while (host != NULL) {
                        if (host->type == XML_ELEMENT_NODE &&
                            xmlStrEqual(host->name, BAD_CAST "host")) {
                            if (VIR_REALLOC_N(hosts, nhosts + 1) < 0) {
                                virReportOOMError();
                                goto error;
                            }
                            hosts[nhosts].name = NULL;
                            hosts[nhosts].port = NULL;
                            nhosts++;

                            hosts[nhosts - 1].name = virXMLPropString(host, "name");
                            if (!hosts[nhosts - 1].name) {
                                virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                                     "%s", _("missing name for host"));
                                goto error;
                            }
                            hosts[nhosts - 1].port = virXMLPropString(host, "port");
                            if (!hosts[nhosts - 1].port) {
                                virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                                     "%s", _("missing port for host"));
                                goto error;
                            }
                        }
                        host = host->next;
                    }
                    break;
                default:
                    virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                         _("unexpected disk type %s"),
                                         virDomainDiskTypeToString(def->type));
                    goto error;
                }

                /* People sometimes pass a bogus '' source path
                   when they mean to omit the source element
                   completely. eg CDROM without media. This is
                   just a little compatability check to help
                   those broken apps */
                if (source && STREQ(source, ""))
                    VIR_FREE(source);
            } else if ((target == NULL) &&
                       (xmlStrEqual(cur->name, BAD_CAST "target"))) {
                target = virXMLPropString(cur, "dev");
                bus = virXMLPropString(cur, "bus");

                /* HACK: Work around for compat with Xen
                 * driver in previous libvirt releases */
                if (target &&
                    STRPREFIX(target, "ioemu:"))
                    memmove(target, target+6, strlen(target)-5);
            } else if ((driverName == NULL) &&
                       (xmlStrEqual(cur->name, BAD_CAST "driver"))) {
                driverName = virXMLPropString(cur, "name");
                driverType = virXMLPropString(cur, "type");
                cachetag = virXMLPropString(cur, "cache");
                error_policy = virXMLPropString(cur, "error_policy");
                iotag = virXMLPropString(cur, "io");
                ioeventfd = virXMLPropString(cur, "ioeventfd");
                event_idx = virXMLPropString(cur, "event_idx");
            } else if (xmlStrEqual(cur->name, BAD_CAST "readonly")) {
                def->readonly = 1;
            } else if (xmlStrEqual(cur->name, BAD_CAST "shareable")) {
                def->shared = 1;
            } else if (xmlStrEqual(cur->name, BAD_CAST "transient")) {
                def->transient = 1;
            } else if ((flags & VIR_DOMAIN_XML_INTERNAL_STATUS) &&
                       xmlStrEqual(cur->name, BAD_CAST "state")) {
                /* Legacy back-compat. Don't add any more attributes here */
                devaddr = virXMLPropString(cur, "devaddr");
            } else if (encryption == NULL &&
                       xmlStrEqual(cur->name, BAD_CAST "encryption")) {
                encryption = virStorageEncryptionParseNode(node->doc,
                                                           cur);
                if (encryption == NULL)
                    goto error;
            } else if ((serial == NULL) &&
                       (xmlStrEqual(cur->name, BAD_CAST "serial"))) {
                serial = (char *)xmlNodeGetContent(cur);
            } else if (xmlStrEqual(cur->name, BAD_CAST "boot")) {
                if (virDomainDeviceBootParseXML(cur, &def->bootIndex,
                                                bootMap))
                    goto error;
            }
        }
        cur = cur->next;
    }

    device = virXMLPropString(node, "device");
    if (device) {
        if ((def->device = virDomainDiskDeviceTypeFromString(device)) < 0) {
            virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                 _("unknown disk device '%s'"), device);
            goto error;
        }
    } else {
        def->device = VIR_DOMAIN_DISK_DEVICE_DISK;
    }

    /* Only CDROM and Floppy devices are allowed missing source path
     * to indicate no media present */
    if (source == NULL && hosts == NULL &&
        def->device != VIR_DOMAIN_DISK_DEVICE_CDROM &&
        def->device != VIR_DOMAIN_DISK_DEVICE_FLOPPY) {
        virDomainReportError(VIR_ERR_NO_SOURCE,
                             target ? "%s" : NULL, target);
        goto error;
    }

    if (target == NULL) {
        virDomainReportError(VIR_ERR_NO_TARGET,
                             source ? "%s" : NULL, source);
        goto error;
    }

    if (def->device == VIR_DOMAIN_DISK_DEVICE_FLOPPY &&
        !STRPREFIX(target, "fd")) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                             _("Invalid floppy device name: %s"), target);
        goto error;
    }

    /* Force CDROM to be listed as read only */
    if (def->device == VIR_DOMAIN_DISK_DEVICE_CDROM)
        def->readonly = 1;

    if (def->device == VIR_DOMAIN_DISK_DEVICE_DISK &&
        !STRPREFIX((const char *)target, "hd") &&
        !STRPREFIX((const char *)target, "sd") &&
        !STRPREFIX((const char *)target, "vd") &&
        !STRPREFIX((const char *)target, "xvd") &&
        !STRPREFIX((const char *)target, "ubd")) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                             _("Invalid harddisk device name: %s"), target);
        goto error;
    }

    if (snapshot) {
        def->snapshot = virDomainDiskSnapshotTypeFromString(snapshot);
        if (def->snapshot <= 0) {
            virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                 _("unknown disk snapshot setting '%s'"),
                                 snapshot);
            goto error;
        }
    } else if (def->readonly) {
        def->snapshot = VIR_DOMAIN_DISK_SNAPSHOT_NO;
    }

    if (bus) {
        if ((def->bus = virDomainDiskBusTypeFromString(bus)) < 0) {
            virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                 _("unknown disk bus type '%s'"), bus);
            goto error;
        }
    } else {
        if (def->device == VIR_DOMAIN_DISK_DEVICE_FLOPPY) {
            def->bus = VIR_DOMAIN_DISK_BUS_FDC;
        } else {
            if (STRPREFIX(target, "hd"))
                def->bus = VIR_DOMAIN_DISK_BUS_IDE;
            else if (STRPREFIX(target, "sd"))
                def->bus = VIR_DOMAIN_DISK_BUS_SCSI;
            else if (STRPREFIX(target, "vd"))
                def->bus = VIR_DOMAIN_DISK_BUS_VIRTIO;
            else if (STRPREFIX(target, "xvd"))
                def->bus = VIR_DOMAIN_DISK_BUS_XEN;
            else if (STRPREFIX(target, "ubd"))
                def->bus = VIR_DOMAIN_DISK_BUS_UML;
            else
                def->bus = VIR_DOMAIN_DISK_BUS_IDE;
        }
    }

    if (def->device == VIR_DOMAIN_DISK_DEVICE_FLOPPY &&
        def->bus != VIR_DOMAIN_DISK_BUS_FDC) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                             _("Invalid bus type '%s' for floppy disk"), bus);
        goto error;
    }
    if (def->device != VIR_DOMAIN_DISK_DEVICE_FLOPPY &&
        def->bus == VIR_DOMAIN_DISK_BUS_FDC) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                             _("Invalid bus type '%s' for disk"), bus);
        goto error;
    }

    if (cachetag &&
        (def->cachemode = virDomainDiskCacheTypeFromString(cachetag)) < 0) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                             _("unknown disk cache mode '%s'"), cachetag);
        goto error;
    }

    if (error_policy &&
        (def->error_policy = virDomainDiskErrorPolicyTypeFromString(error_policy)) <= 0) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                             _("unknown disk error policy '%s'"), error_policy);
        goto error;
    }

    if (iotag) {
        if ((def->iomode = virDomainDiskIoTypeFromString(iotag)) < 0 ||
            def->iomode == VIR_DOMAIN_DISK_IO_DEFAULT) {
            virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                 _("unknown disk io mode '%s'"), iotag);
            goto error;
        }
    }

    if (ioeventfd) {
        if (def->bus != VIR_DOMAIN_DISK_BUS_VIRTIO) {
            virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                                 _("disk ioeventfd mode supported "
                                   "only for virtio bus"));
            goto error;
        }

        int i;
        if ((i = virDomainIoEventFdTypeFromString(ioeventfd)) <= 0) {
            virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                                 _("unknown disk ioeventfd mode '%s'"),
                                 ioeventfd);
            goto error;
        }
        def->ioeventfd=i;
    }

    if (event_idx) {
        if (def->bus != VIR_DOMAIN_DISK_BUS_VIRTIO) {
            virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                                 _("disk event_idx mode supported "
                                   "only for virtio bus"));
            goto error;
        }

        int idx;
        if ((idx = virDomainVirtioEventIdxTypeFromString(event_idx)) <= 0) {
            virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                                 _("unknown disk event_idx mode '%s'"),
                                 event_idx);
            goto error;
        }
        def->event_idx = idx;
    }

    if (devaddr) {
        if (virDomainParseLegacyDeviceAddress(devaddr,
                                              &def->info.addr.pci) < 0) {
            virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                 _("Unable to parse devaddr parameter '%s'"),
                                 devaddr);
            goto error;
        }
        def->info.type = VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI;
    } else {
        if (virDomainDeviceInfoParseXML(node, &def->info, flags) < 0)
            goto error;
    }

    def->src = source;
    source = NULL;
    def->dst = target;
    target = NULL;
    def->hosts = hosts;
    hosts = NULL;
    def->nhosts = nhosts;
    nhosts = 0;
    def->driverName = driverName;
    driverName = NULL;
    def->driverType = driverType;
    driverType = NULL;
    def->encryption = encryption;
    encryption = NULL;
    def->serial = serial;
    serial = NULL;

    if (!def->driverType &&
        caps->defaultDiskDriverType &&
        !(def->driverType = strdup(caps->defaultDiskDriverType)))
        goto no_memory;

    if (!def->driverName &&
        caps->defaultDiskDriverName &&
        !(def->driverName = strdup(caps->defaultDiskDriverName)))
        goto no_memory;

    if (def->info.type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_NONE
        && virDomainDiskDefAssignAddress(caps, def) < 0)
        goto error;

cleanup:
    VIR_FREE(bus);
    VIR_FREE(type);
    VIR_FREE(snapshot);
    VIR_FREE(target);
    VIR_FREE(source);
    while (nhosts > 0) {
        virDomainDiskHostDefFree(&hosts[nhosts - 1]);
        nhosts--;
    }
    VIR_FREE(hosts);
    VIR_FREE(protocol);
    VIR_FREE(device);
    VIR_FREE(driverType);
    VIR_FREE(driverName);
    VIR_FREE(cachetag);
    VIR_FREE(error_policy);
    VIR_FREE(iotag);
    VIR_FREE(ioeventfd);
    VIR_FREE(event_idx);
    VIR_FREE(devaddr);
    VIR_FREE(serial);
    virStorageEncryptionFree(encryption);

    return def;

no_memory:
    virReportOOMError();

error:
    virDomainDiskDefFree(def);
    def = NULL;
    goto cleanup;
}


/* Parse the XML definition for a controller
 * @param node XML nodeset to parse for controller definition
 */
static virDomainControllerDefPtr
virDomainControllerDefParseXML(xmlNodePtr node,
                               unsigned int flags)
{
    virDomainControllerDefPtr def;
    char *type = NULL;
    char *idx = NULL;
    char *model = NULL;

    if (VIR_ALLOC(def) < 0) {
        virReportOOMError();
        return NULL;
    }

    type = virXMLPropString(node, "type");
    if (type) {
        if ((def->type = virDomainControllerTypeFromString(type)) < 0) {
            virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                 _("Unknown controller type '%s'"), type);
            goto error;
        }
    }

    idx = virXMLPropString(node, "index");
    if (idx) {
        if (virStrToLong_i(idx, NULL, 10, &def->idx) < 0) {
            virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                 _("Cannot parse controller index %s"), idx);
            goto error;
        }
    }

    model = virXMLPropString(node, "model");
    if (model) {
        if ((def->model = virDomainControllerModelTypeFromString(model)) < 0) {
            virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                 _("Unknown model type '%s'"), model);
            goto error;
        }
    } else {
        def->model = -1;
    }

    if (virDomainDeviceInfoParseXML(node, &def->info, flags) < 0)
        goto error;

    switch (def->type) {
    case VIR_DOMAIN_CONTROLLER_TYPE_VIRTIO_SERIAL: {
        char *ports = virXMLPropString(node, "ports");
        if (ports) {
            int r = virStrToLong_i(ports, NULL, 10,
                                   &def->opts.vioserial.ports);
            if (r != 0 || def->opts.vioserial.ports < 0) {
                virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                     _("Invalid ports: %s"), ports);
                VIR_FREE(ports);
                goto error;
            }
        } else {
            def->opts.vioserial.ports = -1;
        }
        VIR_FREE(ports);

        char *vectors = virXMLPropString(node, "vectors");
        if (vectors) {
            int r = virStrToLong_i(vectors, NULL, 10,
                                   &def->opts.vioserial.vectors);
            if (r != 0 || def->opts.vioserial.vectors < 0) {
                virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                     _("Invalid vectors: %s"), vectors);
                VIR_FREE(vectors);
                goto error;
            }
        } else {
            def->opts.vioserial.vectors = -1;
        }
        VIR_FREE(vectors);
        break;
    }

    default:
        break;
    }

    if (def->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_NONE &&
        def->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                             _("Controllers must use the 'pci' address type"));
        goto error;
    }

cleanup:
    VIR_FREE(type);
    VIR_FREE(idx);
    VIR_FREE(model);

    return def;

 error:
    virDomainControllerDefFree(def);
    def = NULL;
    goto cleanup;
}

/* Parse the XML definition for a disk
 * @param node XML nodeset to parse for disk definition
 */
static virDomainFSDefPtr
virDomainFSDefParseXML(xmlNodePtr node,
                       unsigned int flags) {
    virDomainFSDefPtr def;
    xmlNodePtr cur;
    char *type = NULL;
    char *source = NULL;
    char *target = NULL;
    char *accessmode = NULL;

    if (VIR_ALLOC(def) < 0) {
        virReportOOMError();
        return NULL;
    }

    type = virXMLPropString(node, "type");
    if (type) {
        if ((def->type = virDomainFSTypeFromString(type)) < 0) {
            virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                 _("unknown filesystem type '%s'"), type);
            goto error;
        }
    } else {
        def->type = VIR_DOMAIN_FS_TYPE_MOUNT;
    }

    accessmode = virXMLPropString(node, "accessmode");
    if (accessmode) {
        if ((def->accessmode = virDomainFSAccessModeTypeFromString(accessmode)) < 0) {
            virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                 _("unknown accessmode '%s'"), accessmode);
            goto error;
        }
    } else {
        def->accessmode = VIR_DOMAIN_FS_ACCESSMODE_PASSTHROUGH;
    }

    cur = node->children;
    while (cur != NULL) {
        if (cur->type == XML_ELEMENT_NODE) {
            if ((source == NULL) &&
                (xmlStrEqual(cur->name, BAD_CAST "source"))) {

                if (def->type == VIR_DOMAIN_FS_TYPE_MOUNT)
                    source = virXMLPropString(cur, "dir");
                else if (def->type == VIR_DOMAIN_FS_TYPE_FILE)
                    source = virXMLPropString(cur, "file");
                else if (def->type == VIR_DOMAIN_FS_TYPE_BLOCK)
                    source = virXMLPropString(cur, "dev");
                else if (def->type == VIR_DOMAIN_FS_TYPE_TEMPLATE)
                    source = virXMLPropString(cur, "name");
            } else if ((target == NULL) &&
                       (xmlStrEqual(cur->name, BAD_CAST "target"))) {
                target = virXMLPropString(cur, "dir");
            } else if (xmlStrEqual(cur->name, BAD_CAST "readonly")) {
                def->readonly = 1;
            }
        }
        cur = cur->next;
    }

    if (source == NULL) {
        virDomainReportError(VIR_ERR_NO_SOURCE,
                             target ? "%s" : NULL, target);
        goto error;
    }

    if (target == NULL) {
        virDomainReportError(VIR_ERR_NO_TARGET,
                             source ? "%s" : NULL, source);
        goto error;
    }

    def->src = source;
    source = NULL;
    def->dst = target;
    target = NULL;

    if (virDomainDeviceInfoParseXML(node, &def->info, flags) < 0)
        goto error;

cleanup:
    VIR_FREE(type);
    VIR_FREE(target);
    VIR_FREE(source);
    VIR_FREE(accessmode);

    return def;

 error:
    virDomainFSDefFree(def);
    def = NULL;
    goto cleanup;
}

static int
virDomainActualNetDefParseXML(xmlNodePtr node,
                              xmlXPathContextPtr ctxt,
                              virDomainActualNetDefPtr *def)
{
    virDomainActualNetDefPtr actual = NULL;
    int ret = -1;
    xmlNodePtr save_ctxt = ctxt->node;
    xmlNodePtr bandwidth_node = NULL;
    char *type = NULL;
    char *mode = NULL;
    char *vlan = NULL;

    if (VIR_ALLOC(actual) < 0) {
        virReportOOMError();
        return -1;
    }

    ctxt->node = node;

    type = virXMLPropString(node, "type");
    if (!type) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                             _("missing type attribute in interface's <actual> element"));
        goto error;
    }
    if ((actual->type = virDomainNetTypeFromString(type)) < 0) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                             _("unknown type '%s' in interface's <actual> element"), type);
        goto error;
    }
    if (actual->type != VIR_DOMAIN_NET_TYPE_BRIDGE &&
        actual->type != VIR_DOMAIN_NET_TYPE_DIRECT &&
        actual->type != VIR_DOMAIN_NET_TYPE_NETWORK) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                             _("unsupported type '%s' in interface's <actual> element"),
                             type);
        goto error;
    }

    if (actual->type == VIR_DOMAIN_NET_TYPE_BRIDGE) {
        actual->data.bridge.brname = virXPathString("string(./source[1]/@bridge)", ctxt);
    } else if (actual->type == VIR_DOMAIN_NET_TYPE_DIRECT) {
        xmlNodePtr virtPortNode;

        actual->data.direct.linkdev = virXPathString("string(./source[1]/@dev)", ctxt);

        mode = virXPathString("string(./source[1]/@mode)", ctxt);
        if (mode) {
            int m;
            if ((m = virMacvtapModeTypeFromString(mode)) < 0) {
                virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                     _("Unkown mode '%s' in interface <actual> element"),
                                     mode);
                goto error;
            }
            actual->data.direct.mode = m;
        }
        
        actual->data.direct.vf_pci_addr = virXPathString("string(./source[1]/@vf_pci_addr)", ctxt);

        vlan = virXPathString("string(./source[1]/@vlan)", ctxt);
        
        if (vlan) {
            if (virStrToLong_i((const char *)vlan, NULL, 10,
                                &actual->data.direct.vlan) < 0)
                goto error;
        }

        virtPortNode = virXPathNode("./virtualport", ctxt);
        if (virtPortNode &&
            virVirtualPortProfileParseXML(virtPortNode,
                                                &actual->data.direct.virtPortProfile) < 0) {
            goto error;
        }
    }

    bandwidth_node = virXPathNode("./bandwidth", ctxt);
    if (bandwidth_node &&
        !(actual->bandwidth = virBandwidthDefParseNode(bandwidth_node)))
        goto error;

    *def = actual;
    actual = NULL;
    ret = 0;
error:
    VIR_FREE(type);
    VIR_FREE(mode);
    virDomainActualNetDefFree(actual);

    ctxt->node = save_ctxt;
    return ret;
}

/* Parse the XML definition for a network interface
 * @param node XML nodeset to parse for net definition
 * @return 0 on success, -1 on failure
 */
static virDomainNetDefPtr
virDomainNetDefParseXML(virCapsPtr caps,
                        xmlNodePtr node,
                        xmlXPathContextPtr ctxt,
                        virBitmapPtr bootMap,
                        unsigned int flags)
{
    virDomainNetDefPtr def;
    xmlNodePtr cur;
    char *macaddr = NULL;
    char *type = NULL;
    char *network = NULL;
    char *portgroup = NULL;
    char *bridge = NULL;
    char *dev = NULL;
    char *ifname = NULL;
    char *script = NULL;
    char *address = NULL;
    char *port = NULL;
    char *model = NULL;
    char *backend = NULL;
    char *txmode = NULL;
    char *ioeventfd = NULL;
    char *event_idx = NULL;
    char *filter = NULL;
    char *internal = NULL;
    char *devaddr = NULL;
    char *mode = NULL;
    char *vf_pci_addr = NULL;
    char *vlan = NULL;
    virNWFilterHashTablePtr filterparams = NULL;
    virVirtualPortProfileParamsPtr virtPort = NULL;
    virDomainActualNetDefPtr actual = NULL;
    xmlNodePtr oldnode = ctxt->node;
    int ret;

    if (VIR_ALLOC(def) < 0) {
        virReportOOMError();
        return NULL;
    }

    ctxt->node = node;

    type = virXMLPropString(node, "type");
    if (type != NULL) {
        if ((int)(def->type = virDomainNetTypeFromString(type)) < 0) {
            virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                 _("unknown interface type '%s'"), type);
            goto error;
        }
    } else {
        def->type = VIR_DOMAIN_NET_TYPE_USER;
    }

    cur = node->children;
    while (cur != NULL) {
        if (cur->type == XML_ELEMENT_NODE) {
            if ((macaddr == NULL) &&
                (xmlStrEqual(cur->name, BAD_CAST "mac"))) {
                macaddr = virXMLPropString(cur, "address");
            } else if ((network == NULL) &&
                       (def->type == VIR_DOMAIN_NET_TYPE_NETWORK) &&
                       (xmlStrEqual(cur->name, BAD_CAST "source"))) {
                network = virXMLPropString(cur, "network");
                portgroup = virXMLPropString(cur, "portgroup");
            } else if ((internal == NULL) &&
                       (def->type == VIR_DOMAIN_NET_TYPE_INTERNAL) &&
                       (xmlStrEqual(cur->name, BAD_CAST "source"))) {
                internal = virXMLPropString(cur, "name");
            } else if ((network == NULL) &&
                       (def->type == VIR_DOMAIN_NET_TYPE_BRIDGE) &&
                       (xmlStrEqual(cur->name, BAD_CAST "source"))) {
                bridge = virXMLPropString(cur, "bridge");
            } else if ((dev == NULL) &&
                       (def->type == VIR_DOMAIN_NET_TYPE_ETHERNET ||
                        def->type == VIR_DOMAIN_NET_TYPE_DIRECT) &&
                       xmlStrEqual(cur->name, BAD_CAST "source")) {
                dev  = virXMLPropString(cur, "dev");
                mode = virXMLPropString(cur, "mode");
                vf_pci_addr = virXMLPropString(cur, "vf_pci_addr");
                vlan = virXMLPropString(cur, "vlan");
            } else if ((virtPort == NULL) &&
                       ((def->type == VIR_DOMAIN_NET_TYPE_DIRECT) ||
                        (def->type == VIR_DOMAIN_NET_TYPE_NETWORK)) &&
                       xmlStrEqual(cur->name, BAD_CAST "virtualport")) {
                if (virVirtualPortProfileParseXML(cur, &virtPort) < 0)
                    goto error;
            } else if ((network == NULL) &&
                       ((def->type == VIR_DOMAIN_NET_TYPE_SERVER) ||
                        (def->type == VIR_DOMAIN_NET_TYPE_CLIENT) ||
                        (def->type == VIR_DOMAIN_NET_TYPE_MCAST)) &&
                       (xmlStrEqual(cur->name, BAD_CAST "source"))) {
                address = virXMLPropString(cur, "address");
                port = virXMLPropString(cur, "port");
            } else if ((address == NULL) &&
                       (def->type == VIR_DOMAIN_NET_TYPE_ETHERNET ||
                        def->type == VIR_DOMAIN_NET_TYPE_BRIDGE) &&
                       (xmlStrEqual(cur->name, BAD_CAST "ip"))) {
                address = virXMLPropString(cur, "address");
            } else if ((ifname == NULL) &&
                       xmlStrEqual(cur->name, BAD_CAST "target")) {
                ifname = virXMLPropString(cur, "dev");
                if ((ifname != NULL) &&
                    ((flags & VIR_DOMAIN_XML_INACTIVE) &&
                      (STRPREFIX(ifname, VIR_NET_GENERATED_PREFIX)))) {
                    /* An auto-generated target name, blank it out */
                    VIR_FREE(ifname);
                }
            } else if ((script == NULL) &&
                       (def->type == VIR_DOMAIN_NET_TYPE_ETHERNET ||
                        def->type == VIR_DOMAIN_NET_TYPE_BRIDGE) &&
                       xmlStrEqual(cur->name, BAD_CAST "script")) {
                script = virXMLPropString(cur, "path");
            } else if (xmlStrEqual (cur->name, BAD_CAST "model")) {
                model = virXMLPropString(cur, "type");
            } else if (xmlStrEqual (cur->name, BAD_CAST "driver")) {
                backend = virXMLPropString(cur, "name");
                txmode = virXMLPropString(cur, "txmode");
                ioeventfd = virXMLPropString(cur, "ioeventfd");
                event_idx = virXMLPropString(cur, "event_idx");
            } else if (xmlStrEqual (cur->name, BAD_CAST "filterref")) {
                filter = virXMLPropString(cur, "filter");
                VIR_FREE(filterparams);
                filterparams = virNWFilterParseParamAttributes(cur);
            } else if ((flags & VIR_DOMAIN_XML_INTERNAL_STATUS) &&
                       xmlStrEqual(cur->name, BAD_CAST "state")) {
                /* Legacy back-compat. Don't add any more attributes here */
                devaddr = virXMLPropString(cur, "devaddr");
            } else if (xmlStrEqual(cur->name, BAD_CAST "boot")) {
                if (virDomainDeviceBootParseXML(cur, &def->bootIndex,
                                                bootMap))
                    goto error;
            } else if ((actual == NULL) &&
                       (flags & VIR_DOMAIN_XML_INTERNAL_ACTUAL_NET) &&
                       (def->type == VIR_DOMAIN_NET_TYPE_NETWORK) &&
                       xmlStrEqual(cur->name, BAD_CAST "actual")) {
                if (virDomainActualNetDefParseXML(cur, ctxt, &actual) < 0)
                    goto error;
            } else if (xmlStrEqual(cur->name, BAD_CAST "bandwidth")) {
                if (!(def->bandwidth = virBandwidthDefParseNode(cur)))
                    goto error;
            }
        }
        cur = cur->next;
    }

    if (macaddr) {
        if (virParseMacAddr((const char *)macaddr, def->mac) < 0) {
            virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                 _("unable to parse mac address '%s'"),
                                 (const char *)macaddr);
            goto error;
        }
    } else {
        virCapabilitiesGenerateMac(caps, def->mac);
    }

    if (devaddr) {
        if (virDomainParseLegacyDeviceAddress(devaddr,
                                              &def->info.addr.pci) < 0) {
            virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                 _("Unable to parse devaddr parameter '%s'"),
                                 devaddr);
            goto error;
        }
        def->info.type = VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI;
    } else {
        if (virDomainDeviceInfoParseXML(node, &def->info, flags) < 0)
            goto error;
    }

    /* XXX what about ISA/USB based NIC models - once we support
     * them we should make sure address type is correct */
    if (def->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_NONE &&
        def->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                             _("Network interfaces must use 'pci' address type"));
        goto error;
    }

    switch (def->type) {
    case VIR_DOMAIN_NET_TYPE_NETWORK:
        if (network == NULL) {
            virDomainReportError(VIR_ERR_INTERNAL_ERROR, "%s",
    _("No <source> 'network' attribute specified with <interface type='network'/>"));
            goto error;
        }
        def->data.network.name = network;
        network = NULL;
        def->data.network.portgroup = portgroup;
        portgroup = NULL;
        def->data.network.virtPortProfile = virtPort;
        virtPort = NULL;
        def->data.network.actual = actual;
        actual = NULL;
        break;

    case VIR_DOMAIN_NET_TYPE_ETHERNET:

        if (script != NULL) {
            def->data.ethernet.script = script;
            script = NULL;
        }
        if (dev != NULL) {
            def->data.ethernet.dev = dev;
            dev = NULL;
        }
        if (address != NULL) {
            def->data.ethernet.ipaddr = address;
            address = NULL;
        }
        break;

    case VIR_DOMAIN_NET_TYPE_BRIDGE:
        if (bridge == NULL) {
            virDomainReportError(VIR_ERR_INTERNAL_ERROR, "%s",
    _("No <source> 'bridge' attribute specified with <interface type='bridge'/>"));
            goto error;
        }
        def->data.bridge.brname = bridge;
        bridge = NULL;
        if (script != NULL) {
            def->data.bridge.script = script;
            script = NULL;
        }
        if (address != NULL) {
            def->data.bridge.ipaddr = address;
            address = NULL;
        }
        break;

    case VIR_DOMAIN_NET_TYPE_CLIENT:
    case VIR_DOMAIN_NET_TYPE_SERVER:
    case VIR_DOMAIN_NET_TYPE_MCAST:
        if (port == NULL) {
            virDomainReportError(VIR_ERR_INTERNAL_ERROR, "%s",
            _("No <source> 'port' attribute specified with socket interface"));
            goto error;
        }
        if (virStrToLong_i(port, NULL, 10, &def->data.socket.port) < 0) {
            virDomainReportError(VIR_ERR_INTERNAL_ERROR, "%s",
            _("Cannot parse <source> 'port' attribute with socket interface"));
            goto error;
        }

        if (address == NULL) {
            if (def->type == VIR_DOMAIN_NET_TYPE_CLIENT ||
                def->type == VIR_DOMAIN_NET_TYPE_MCAST) {
                virDomainReportError(VIR_ERR_INTERNAL_ERROR, "%s",
        _("No <source> 'address' attribute specified with socket interface"));
                goto error;
            }
        } else {
            def->data.socket.address = address;
            address = NULL;
        }
        break;

    case VIR_DOMAIN_NET_TYPE_INTERNAL:
        if (internal == NULL) {
            virDomainReportError(VIR_ERR_INTERNAL_ERROR, "%s",
        _("No <source> 'name' attribute specified with <interface type='internal'/>"));
            goto error;
        }
        def->data.internal.name = internal;
        internal = NULL;
        break;

    case VIR_DOMAIN_NET_TYPE_DIRECT:
        if (dev == NULL) {
            virDomainReportError(VIR_ERR_INTERNAL_ERROR, "%s",
        _("No <source> 'dev' attribute specified with <interface type='direct'/>"));
            goto error;
        }

        if (mode != NULL) {
            int m;
            if ((m = virMacvtapModeTypeFromString(mode)) < 0) {
                virDomainReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                                     _("Unkown mode has been specified"));
                goto error;
            }
            def->data.direct.mode = m;
        } else
            def->data.direct.mode = VIR_MACVTAP_MODE_VEPA;

        def->data.direct.virtPortProfile = virtPort;
        virtPort = NULL;
        def->data.direct.linkdev = dev;
        def->data.direct.vf_pci_addr = NULL;
        dev = NULL;

        if ((flags & VIR_DOMAIN_XML_INACTIVE))
            VIR_FREE(ifname);

        break;

    case VIR_DOMAIN_NET_TYPE_USER:
    case VIR_DOMAIN_NET_TYPE_LAST:
        break;
    }

    if (ifname != NULL) {
        def->ifname = ifname;
        ifname = NULL;
    }

    /* NIC model (see -net nic,model=?).  We only check that it looks
     * reasonable, not that it is a supported NIC type.  FWIW kvm
     * supports these types as of April 2008:
     * i82551 i82557b i82559er ne2k_pci pcnet rtl8139 e1000 virtio
     */
    if (model != NULL) {
        int i;
        for (i = 0 ; i < strlen(model) ; i++) {
            int char_ok = c_isalnum(model[i]) || model[i] == '_';
            if (!char_ok) {
                virDomainReportError(VIR_ERR_INVALID_ARG, "%s",
                                     _("Model name contains invalid characters"));
                goto error;
            }
        }
        def->model = model;
        model = NULL;
    }

    if (def->model && STREQ(def->model, "virtio")) {
        if (backend != NULL) {
            int name;
            if (((name = virDomainNetBackendTypeFromString(backend)) < 0) ||
                (name == VIR_DOMAIN_NET_BACKEND_TYPE_DEFAULT)) {
                virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                     _("Unknown interface <driver name='%s'> "
                                       "has been specified"),
                                     backend);
                goto error;
            }
            def->driver.virtio.name = name;
        }
        if (txmode != NULL) {
            int m;
            if (((m = virDomainNetVirtioTxModeTypeFromString(txmode)) < 0) ||
                (m == VIR_DOMAIN_NET_VIRTIO_TX_MODE_DEFAULT)) {
                virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                     _("Unknown interface <driver txmode='%s'> "
                                       "has been specified"),
                                     txmode);
                goto error;
            }
            def->driver.virtio.txmode = m;
        }
        if (ioeventfd) {
            int i;
            if ((i = virDomainIoEventFdTypeFromString(ioeventfd)) <= 0) {
                virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                                     _("unknown interface ioeventfd mode '%s'"),
                                     ioeventfd);
                goto error;
            }
            def->driver.virtio.ioeventfd = i;
        }
        if (event_idx) {
            int idx;
            if ((idx = virDomainVirtioEventIdxTypeFromString(event_idx)) <= 0) {
                virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                                     _("unknown interface event_idx mode '%s'"),
                                     event_idx);
                goto error;
            }
            def->driver.virtio.event_idx = idx;
        }
    }

    if (filter != NULL) {
        switch (def->type) {
        case VIR_DOMAIN_NET_TYPE_ETHERNET:
        case VIR_DOMAIN_NET_TYPE_NETWORK:
        case VIR_DOMAIN_NET_TYPE_BRIDGE:
            def->filter = filter;
            filter = NULL;
            def->filterparams = filterparams;
            filterparams = NULL;
        break;
        default:
        break;
        }
    }

    ret = virXPathULong("string(./tune/sndbuf)", ctxt, &def->tune.sndbuf);
    if (ret >= 0) {
        def->tune.sndbuf_specified = true;
    } else if (ret == -2) {
        virDomainReportError(VIR_ERR_XML_ERROR, "%s",
                             _("sndbuf must be a positive integer"));
        goto error;
    }

cleanup:
    ctxt->node = oldnode;
    VIR_FREE(macaddr);
    VIR_FREE(network);
    VIR_FREE(portgroup);
    VIR_FREE(address);
    VIR_FREE(port);
    VIR_FREE(ifname);
    VIR_FREE(dev);
    VIR_FREE(virtPort);
    virDomainActualNetDefFree(actual);
    VIR_FREE(script);
    VIR_FREE(bridge);
    VIR_FREE(model);
    VIR_FREE(backend);
    VIR_FREE(txmode);
    VIR_FREE(ioeventfd);
    VIR_FREE(event_idx);
    VIR_FREE(filter);
    VIR_FREE(type);
    VIR_FREE(internal);
    VIR_FREE(devaddr);
    VIR_FREE(mode);
    VIR_FREE(vf_pci_addr);
    VIR_FREE(vlan);
    virNWFilterHashTableFree(filterparams);

    return def;

error:
    virDomainNetDefFree(def);
    def = NULL;
    goto cleanup;
}

static int
virDomainChrDefaultTargetType(virCapsPtr caps, int devtype) {

    int target = -1;

    switch (devtype) {
    case VIR_DOMAIN_CHR_DEVICE_TYPE_CHANNEL:
        virDomainReportError(VIR_ERR_XML_ERROR,
                             _("target type must be specified for %s device"),
                             virDomainChrDeviceTypeToString(devtype));
        break;

    case VIR_DOMAIN_CHR_DEVICE_TYPE_CONSOLE:
        target = caps->defaultConsoleTargetType;
        break;

    case VIR_DOMAIN_CHR_DEVICE_TYPE_SERIAL:
    case VIR_DOMAIN_CHR_DEVICE_TYPE_PARALLEL:
    default:
        /* No target type yet*/
        target = 0;
        break;
    }

    return target;
}

static int
virDomainChrTargetTypeFromString(virCapsPtr caps,
                                 int devtype,
                                 const char *targetType)
{
    int ret = -1;
    int target = 0;

    if (!targetType) {
        target = virDomainChrDefaultTargetType(caps, devtype);
        goto out;
    }

    switch (devtype) {
    case VIR_DOMAIN_CHR_DEVICE_TYPE_CHANNEL:
        target = virDomainChrChannelTargetTypeFromString(targetType);
        break;

    case VIR_DOMAIN_CHR_DEVICE_TYPE_CONSOLE:
        target = virDomainChrConsoleTargetTypeFromString(targetType);
        break;

    case VIR_DOMAIN_CHR_DEVICE_TYPE_SERIAL:
    case VIR_DOMAIN_CHR_DEVICE_TYPE_PARALLEL:
    default:
        /* No target type yet*/
        break;
    }

out:
    ret = target;
    return ret;
}

static int
virDomainChrDefParseTargetXML(virCapsPtr caps,
                              virDomainChrDefPtr def,
                              xmlNodePtr cur)
{
    int ret = -1;
    unsigned int port;
    const char *targetType = virXMLPropString(cur, "type");
    const char *addrStr = NULL;
    const char *portStr = NULL;

    if ((def->targetType =
        virDomainChrTargetTypeFromString(caps,
                                         def->deviceType, targetType)) < 0) {
        goto error;
    }

    switch (def->deviceType) {
    case VIR_DOMAIN_CHR_DEVICE_TYPE_CHANNEL:
        switch (def->targetType) {
        case VIR_DOMAIN_CHR_CHANNEL_TARGET_TYPE_GUESTFWD:
            addrStr = virXMLPropString(cur, "address");
            portStr = virXMLPropString(cur, "port");

            if (addrStr == NULL) {
                virDomainReportError(VIR_ERR_XML_ERROR, "%s",
                                     _("guestfwd channel does not "
                                       "define a target address"));
                goto error;
            }

            if (VIR_ALLOC(def->target.addr) < 0) {
                virReportOOMError();
                goto error;
            }

            if (virSocketParseAddr(addrStr, def->target.addr, AF_UNSPEC) < 0)
                goto error;

            if (def->target.addr->data.stor.ss_family != AF_INET) {
                virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                                     "%s", _("guestfwd channel only supports "
                                             "IPv4 addresses"));
                goto error;
            }

            if (portStr == NULL) {
                virDomainReportError(VIR_ERR_XML_ERROR, "%s",
                                     _("guestfwd channel does "
                                       "not define a target port"));
                goto error;
            }

            if (virStrToLong_ui(portStr, NULL, 10, &port) < 0) {
                virDomainReportError(VIR_ERR_XML_ERROR,
                                     _("Invalid port number: %s"),
                                     portStr);
                goto error;
            }

            virSocketSetPort(def->target.addr, port);
            break;

        case VIR_DOMAIN_CHR_CHANNEL_TARGET_TYPE_VIRTIO:
            def->target.name = virXMLPropString(cur, "name");
            break;
        }
        break;

    default:
        portStr = virXMLPropString(cur, "port");
        if (portStr == NULL) {
            /* Set to negative value to indicate we should set it later */
            def->target.port = -1;
            break;
        }

        if (virStrToLong_ui(portStr, NULL, 10, &port) < 0) {
            virDomainReportError(VIR_ERR_XML_ERROR,
                                 _("Invalid port number: %s"),
                                 portStr);
            goto error;
        }
        def->target.port = port;
        break;
    }


    ret = 0;
error:
    VIR_FREE(targetType);
    VIR_FREE(addrStr);
    VIR_FREE(portStr);

    return ret;
}

/* Parse the source half of the XML definition for a character device,
 * where node is the first element of node->children of the parent
 * element.  def->type must already be valid.  Return -1 on failure,
 * otherwise the number of ignored children (this intentionally skips
 * <target>, which is used by <serial> but not <smartcard>). */
static int
virDomainChrSourceDefParseXML(virDomainChrSourceDefPtr def,
                              xmlNodePtr cur, unsigned int flags)
{
    char *bindHost = NULL;
    char *bindService = NULL;
    char *connectHost = NULL;
    char *connectService = NULL;
    char *path = NULL;
    char *mode = NULL;
    char *protocol = NULL;
    int remaining = 0;

    while (cur != NULL) {
        if (cur->type == XML_ELEMENT_NODE) {
            if (xmlStrEqual(cur->name, BAD_CAST "source")) {
                if (mode == NULL)
                    mode = virXMLPropString(cur, "mode");

                switch (def->type) {
                case VIR_DOMAIN_CHR_TYPE_PTY:
                case VIR_DOMAIN_CHR_TYPE_DEV:
                case VIR_DOMAIN_CHR_TYPE_FILE:
                case VIR_DOMAIN_CHR_TYPE_PIPE:
                case VIR_DOMAIN_CHR_TYPE_UNIX:
                    /* PTY path is only parsed from live xml.  */
                    if (path == NULL &&
                        (def->type != VIR_DOMAIN_CHR_TYPE_PTY ||
                         !(flags & VIR_DOMAIN_XML_INACTIVE)))
                        path = virXMLPropString(cur, "path");

                    break;

                case VIR_DOMAIN_CHR_TYPE_UDP:
                case VIR_DOMAIN_CHR_TYPE_TCP:
                    if (mode == NULL ||
                        STREQ((const char *)mode, "connect")) {

                        if (connectHost == NULL)
                            connectHost = virXMLPropString(cur, "host");
                        if (connectService == NULL)
                            connectService = virXMLPropString(cur, "service");
                    } else if (STREQ((const char *)mode, "bind")) {
                        if (bindHost == NULL)
                            bindHost = virXMLPropString(cur, "host");
                        if (bindService == NULL)
                            bindService = virXMLPropString(cur, "service");
                    } else {
                        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                             _("Unknown source mode '%s'"),
                                             mode);
                        goto error;
                    }

                    if (def->type == VIR_DOMAIN_CHR_TYPE_UDP)
                        VIR_FREE(mode);
                }
            } else if (xmlStrEqual(cur->name, BAD_CAST "protocol")) {
                if (protocol == NULL)
                    protocol = virXMLPropString(cur, "type");
            } else {
                remaining++;
            }
        }
        cur = cur->next;
    }

    switch (def->type) {
    case VIR_DOMAIN_CHR_TYPE_NULL:
        /* Nada */
        break;

    case VIR_DOMAIN_CHR_TYPE_VC:
        break;

    case VIR_DOMAIN_CHR_TYPE_PTY:
    case VIR_DOMAIN_CHR_TYPE_DEV:
    case VIR_DOMAIN_CHR_TYPE_FILE:
    case VIR_DOMAIN_CHR_TYPE_PIPE:
        if (path == NULL &&
            def->type != VIR_DOMAIN_CHR_TYPE_PTY) {
            virDomainReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Missing source path attribute for char device"));
            goto error;
        }

        def->data.file.path = path;
        path = NULL;
        break;

    case VIR_DOMAIN_CHR_TYPE_STDIO:
    case VIR_DOMAIN_CHR_TYPE_SPICEVMC:
        /* Nada */
        break;

    case VIR_DOMAIN_CHR_TYPE_TCP:
        if (mode == NULL ||
            STREQ(mode, "connect")) {
            if (connectHost == NULL) {
                virDomainReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                        _("Missing source host attribute for char device"));
                goto error;
            }
            if (connectService == NULL) {
                virDomainReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                     _("Missing source service attribute for char device"));
                goto error;
            }

            def->data.tcp.host = connectHost;
            connectHost = NULL;
            def->data.tcp.service = connectService;
            connectService = NULL;
            def->data.tcp.listen = false;
        } else {
            if (bindHost == NULL) {
                virDomainReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                        _("Missing source host attribute for char device"));
                goto error;
            }
            if (bindService == NULL) {
                virDomainReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                     _("Missing source service attribute for char device"));
                goto error;
            }

            def->data.tcp.host = bindHost;
            bindHost = NULL;
            def->data.tcp.service = bindService;
            bindService = NULL;
            def->data.tcp.listen = true;
        }

        if (protocol == NULL)
            def->data.tcp.protocol = VIR_DOMAIN_CHR_TCP_PROTOCOL_RAW;
        else if ((def->data.tcp.protocol =
                  virDomainChrTcpProtocolTypeFromString(protocol)) < 0) {
            virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                 _("Unknown protocol '%s'"), protocol);
            goto error;
        }

        break;

    case VIR_DOMAIN_CHR_TYPE_UDP:
        if (connectService == NULL) {
            virDomainReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                   _("Missing source service attribute for char device"));
            goto error;
        }

        def->data.udp.connectHost = connectHost;
        connectHost = NULL;
        def->data.udp.connectService = connectService;
        connectService = NULL;

        def->data.udp.bindHost = bindHost;
        bindHost = NULL;
        def->data.udp.bindService = bindService;
        bindService = NULL;
        break;

    case VIR_DOMAIN_CHR_TYPE_UNIX:
        if (path == NULL) {
            virDomainReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                         _("Missing source path attribute for char device"));
            goto error;
        }

        def->data.nix.listen = mode != NULL && STRNEQ(mode, "connect");

        def->data.nix.path = path;
        path = NULL;
        break;
    }

cleanup:
    VIR_FREE(mode);
    VIR_FREE(protocol);
    VIR_FREE(bindHost);
    VIR_FREE(bindService);
    VIR_FREE(connectHost);
    VIR_FREE(connectService);
    VIR_FREE(path);

    return remaining;

error:
    virDomainChrSourceDefClear(def);
    remaining = -1;
    goto cleanup;
}

/* Create a new character device definition and set
 * default port.
 */
virDomainChrDefPtr
virDomainChrDefNew(void) {
    virDomainChrDefPtr def = NULL;

    if (VIR_ALLOC(def) < 0) {
        virReportOOMError();
        return NULL;
    }

    def->target.port = -1;
    return def;
}

/* Parse the XML definition for a character device
 * @param node XML nodeset to parse for net definition
 *
 * The XML we're dealing with looks like
 *
 * <serial type="pty">
 *   <source path="/dev/pts/3"/>
 *   <target port="1"/>
 * </serial>
 *
 * <serial type="dev">
 *   <source path="/dev/ttyS0"/>
 *   <target port="1"/>
 * </serial>
 *
 * <serial type="tcp">
 *   <source mode="connect" host="0.0.0.0" service="2445"/>
 *   <target port="1"/>
 * </serial>
 *
 * <serial type="tcp">
 *   <source mode="bind" host="0.0.0.0" service="2445"/>
 *   <target port="1"/>
 *   <protocol type='raw'/>
 * </serial>
 *
 * <serial type="udp">
 *   <source mode="bind" host="0.0.0.0" service="2445"/>
 *   <source mode="connect" host="0.0.0.0" service="2445"/>
 *   <target port="1"/>
 * </serial>
 *
 * <serial type="unix">
 *   <source mode="bind" path="/tmp/foo"/>
 *   <target port="1"/>
 * </serial>
 *
 */
static virDomainChrDefPtr
virDomainChrDefParseXML(virCapsPtr caps,
                        xmlNodePtr node,
                        unsigned int flags)
{
    xmlNodePtr cur;
    char *type = NULL;
    const char *nodeName;
    virDomainChrDefPtr def;
    int remaining;

    if (!(def = virDomainChrDefNew()))
        return NULL;

    type = virXMLPropString(node, "type");
    if (type == NULL) {
        def->source.type = VIR_DOMAIN_CHR_TYPE_PTY;
    } else if ((def->source.type = virDomainChrTypeFromString(type)) < 0) {
        virDomainReportError(VIR_ERR_XML_ERROR,
                             _("unknown type presented to host for character device: %s"),
                             type);
        goto error;
    }

    nodeName = (const char *) node->name;
    if ((def->deviceType = virDomainChrDeviceTypeFromString(nodeName)) < 0) {
        virDomainReportError(VIR_ERR_XML_ERROR,
                             _("unknown character device type: %s"),
                             nodeName);
    }

    cur = node->children;
    remaining = virDomainChrSourceDefParseXML(&def->source, cur, flags);
    if (remaining < 0)
        goto error;
    if (remaining) {
        while (cur != NULL) {
            if (cur->type == XML_ELEMENT_NODE) {
                if (xmlStrEqual(cur->name, BAD_CAST "target")) {
                    if (virDomainChrDefParseTargetXML(caps, def, cur) < 0) {
                        goto error;
                    }
                }
            }
            cur = cur->next;
        }
    }

    if (def->source.type == VIR_DOMAIN_CHR_TYPE_SPICEVMC) {
        if (def->targetType != VIR_DOMAIN_CHR_CHANNEL_TARGET_TYPE_VIRTIO) {
            virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                                 _("spicevmc device type only supports "
                                   "virtio"));
            goto error;
        } else {
            def->source.data.spicevmc = VIR_DOMAIN_CHR_SPICEVMC_VDAGENT;
        }
    }

    if (virDomainDeviceInfoParseXML(node, &def->info, flags) < 0)
        goto error;

cleanup:
    VIR_FREE(type);

    return def;

error:
    virDomainChrDefFree(def);
    def = NULL;
    goto cleanup;
}

static virDomainSmartcardDefPtr
virDomainSmartcardDefParseXML(xmlNodePtr node,
                              unsigned int flags)
{
    xmlNodePtr cur;
    char *mode = NULL;
    char *type = NULL;
    virDomainSmartcardDefPtr def;
    int i;

    if (VIR_ALLOC(def) < 0) {
        virReportOOMError();
        return NULL;
    }

    mode = virXMLPropString(node, "mode");
    if (mode == NULL) {
        virDomainReportError(VIR_ERR_XML_ERROR, "%s",
                             _("missing smartcard device mode"));
        goto error;
    }
    if ((def->type = virDomainSmartcardTypeFromString(mode)) < 0) {
        virDomainReportError(VIR_ERR_XML_ERROR,
                             _("unknown smartcard device mode: %s"),
                             mode);
        goto error;
    }

    switch (def->type) {
    case VIR_DOMAIN_SMARTCARD_TYPE_HOST:
        break;

    case VIR_DOMAIN_SMARTCARD_TYPE_HOST_CERTIFICATES:
        i = 0;
        cur = node->children;
        while (cur) {
            if (cur->type == XML_ELEMENT_NODE &&
                xmlStrEqual(cur->name, BAD_CAST "certificate")) {
                if (i == 3) {
                    virDomainReportError(VIR_ERR_XML_ERROR, "%s",
                                         _("host-certificates mode needs "
                                           "exactly three certificates"));
                    goto error;
                }
                def->data.cert.file[i] = (char *)xmlNodeGetContent(cur);
                if (!def->data.cert.file[i]) {
                    virReportOOMError();
                    goto error;
                }
                i++;
            } else if (cur->type == XML_ELEMENT_NODE &&
                       xmlStrEqual(cur->name, BAD_CAST "database") &&
                       !def->data.cert.database) {
                def->data.cert.database = (char *)xmlNodeGetContent(cur);
                if (!def->data.cert.database) {
                    virReportOOMError();
                    goto error;
                }
                if (*def->data.cert.database != '/') {
                    virDomainReportError(VIR_ERR_XML_ERROR,
                                         _("expecting absolute path: %s"),
                                         def->data.cert.database);
                    goto error;
                }
            }
            cur = cur->next;
        }
        if (i < 3) {
            virDomainReportError(VIR_ERR_XML_ERROR, "%s",
                                 _("host-certificates mode needs "
                                   "exactly three certificates"));
            goto error;
        }
        break;

    case VIR_DOMAIN_SMARTCARD_TYPE_PASSTHROUGH:
        type = virXMLPropString(node, "type");
        if (type == NULL) {
            virDomainReportError(VIR_ERR_XML_ERROR, "%s",
                                 _("passthrough mode requires a character "
                                   "device type attribute"));
            goto error;
        }
        if ((def->data.passthru.type = virDomainChrTypeFromString(type)) < 0) {
            virDomainReportError(VIR_ERR_XML_ERROR,
                                 _("unknown type presented to host for "
                                   "character device: %s"), type);
            goto error;
        }

        cur = node->children;
        if (virDomainChrSourceDefParseXML(&def->data.passthru, cur, flags) < 0)
            goto error;

        if (def->data.passthru.type == VIR_DOMAIN_CHR_TYPE_SPICEVMC) {
            def->data.passthru.data.spicevmc
                = VIR_DOMAIN_CHR_SPICEVMC_SMARTCARD;
        }

        break;

    default:
        virDomainReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                             _("unknown smartcard mode"));
        goto error;
    }

    if (virDomainDeviceInfoParseXML(node, &def->info, flags) < 0)
        goto error;
    if (def->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_NONE &&
        def->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_CCID) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                             _("Controllers must use the 'ccid' address type"));
        goto error;
    }

cleanup:
    VIR_FREE(mode);
    VIR_FREE(type);

    return def;

error:
    virDomainSmartcardDefFree(def);
    def = NULL;
    goto cleanup;
}

/* Parse the XML definition for a network interface */
static virDomainInputDefPtr
virDomainInputDefParseXML(const char *ostype,
                          xmlNodePtr node,
                          unsigned int flags)
{
    virDomainInputDefPtr def;
    char *type = NULL;
    char *bus = NULL;

    if (VIR_ALLOC(def) < 0) {
        virReportOOMError();
        return NULL;
    }

    type = virXMLPropString(node, "type");
    bus = virXMLPropString(node, "bus");

    if (!type) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                             "%s", _("missing input device type"));
        goto error;
    }

    if ((def->type = virDomainInputTypeFromString(type)) < 0) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                             _("unknown input device type '%s'"), type);
        goto error;
    }

    if (bus) {
        if ((def->bus = virDomainInputBusTypeFromString(bus)) < 0) {
            virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                 _("unknown input bus type '%s'"), bus);
            goto error;
        }

        if (STREQ(ostype, "hvm")) {
            if (def->bus == VIR_DOMAIN_INPUT_BUS_PS2 && /* Only allow mouse for ps2 */
                def->type != VIR_DOMAIN_INPUT_TYPE_MOUSE) {
                virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                 _("ps2 bus does not support %s input device"),
                                     type);
                goto error;
            }
            if (def->bus == VIR_DOMAIN_INPUT_BUS_XEN) {
                virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                     _("unsupported input bus %s"),
                                     bus);
                goto error;
            }
        } else {
            if (def->bus != VIR_DOMAIN_INPUT_BUS_XEN) {
                virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                     _("unsupported input bus %s"),
                                     bus);
            }
            if (def->type != VIR_DOMAIN_INPUT_TYPE_MOUSE) {
                virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                 _("xen bus does not support %s input device"),
                                     type);
                goto error;
            }
        }
    } else {
        if (STREQ(ostype, "hvm")) {
            if (def->type == VIR_DOMAIN_INPUT_TYPE_MOUSE)
                def->bus = VIR_DOMAIN_INPUT_BUS_PS2;
            else
                def->bus = VIR_DOMAIN_INPUT_BUS_USB;
        } else {
            def->bus = VIR_DOMAIN_INPUT_BUS_XEN;
        }
    }

    if (virDomainDeviceInfoParseXML(node, &def->info, flags) < 0)
        goto error;

cleanup:
    VIR_FREE(type);
    VIR_FREE(bus);

    return def;

error:
    virDomainInputDefFree(def);
    def = NULL;
    goto cleanup;
}


/* Parse the XML definition for a clock timer */
static virDomainTimerDefPtr
virDomainTimerDefParseXML(const xmlNodePtr node,
                          xmlXPathContextPtr ctxt)
{
    char *name = NULL;
    char *present = NULL;
    char *tickpolicy = NULL;
    char *track = NULL;
    char *mode = NULL;

    virDomainTimerDefPtr def;
    xmlNodePtr oldnode = ctxt->node;
    xmlNodePtr catchup;
    int ret;

    if (VIR_ALLOC(def) < 0) {
        virReportOOMError();
        return NULL;
    }

    ctxt->node = node;

    name = virXMLPropString(node, "name");
    if (name == NULL) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                             "%s", _("missing timer name"));
        goto error;
    }
    if ((def->name = virDomainTimerNameTypeFromString(name)) < 0) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                             _("unknown timer name '%s'"), name);
        goto error;
    }

    def->present = -1; /* unspecified */
    if ((present = virXMLPropString(node, "present")) != NULL) {
        if (STREQ(present, "yes")) {
            def->present = 1;
        } else if (STREQ(present, "no")) {
            def->present = 0;
        } else {
            virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                 _("unknown timer present value '%s'"), present);
            goto error;
        }
    }

    def->tickpolicy = -1;
    tickpolicy = virXMLPropString(node, "tickpolicy");
    if (tickpolicy != NULL) {
        if ((def->tickpolicy = virDomainTimerTickpolicyTypeFromString(tickpolicy)) < 0) {
            virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                 _("unknown timer tickpolicy '%s'"), tickpolicy);
            goto error;
        }
    }

    def->track = -1;
    track = virXMLPropString(node, "track");
    if (track != NULL) {
        if ((def->track = virDomainTimerTrackTypeFromString(track)) < 0) {
            virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                 _("unknown timer track '%s'"), track);
            goto error;
        }
    }

    ret = virXPathULong("string(./frequency)", ctxt, &def->frequency);
    if (ret == -1) {
        def->frequency = 0;
    } else if (ret < 0) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                             "%s", _("invalid timer frequency"));
        goto error;
    }

    def->mode = -1;
    mode = virXMLPropString(node, "mode");
    if (mode != NULL) {
        if ((def->mode = virDomainTimerModeTypeFromString(mode)) < 0) {
            virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                 _("unknown timer mode '%s'"), mode);
            goto error;
        }
    }

    catchup = virXPathNode("./catchup", ctxt);
    if (catchup != NULL) {
        ret = virXPathULong("string(./catchup/@threshold)", ctxt,
                            &def->catchup.threshold);
        if (ret == -1) {
            def->catchup.threshold = 0;
        } else if (ret < 0) {
            virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                 "%s", _("invalid catchup threshold"));
            goto error;
        }

        ret = virXPathULong("string(./catchup/@slew)", ctxt, &def->catchup.slew);
        if (ret == -1) {
            def->catchup.slew = 0;
        } else if (ret < 0) {
            virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                 "%s", _("invalid catchup slew"));
            goto error;
        }

        ret = virXPathULong("string(./catchup/@limit)", ctxt, &def->catchup.limit);
        if (ret == -1) {
            def->catchup.limit = 0;
        } else if (ret < 0) {
            virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                 "%s", _("invalid catchup limit"));
            goto error;
        }
    }

cleanup:
    VIR_FREE(name);
    VIR_FREE(present);
    VIR_FREE(tickpolicy);
    VIR_FREE(track);
    VIR_FREE(mode);
    ctxt->node = oldnode;

    return def;

error:
    VIR_FREE(def);
    goto cleanup;
}


static int
virDomainGraphicsAuthDefParseXML(xmlNodePtr node,
                                 virDomainGraphicsAuthDefPtr def,
                                 int type)
{
    char *validTo = NULL;
    char *connected = virXMLPropString(node, "connected");

    def->passwd = virXMLPropString(node, "passwd");

    if (!def->passwd)
        return 0;

    validTo = virXMLPropString(node, "passwdValidTo");
    if (validTo) {
        char *tmp;
        struct tm tm;
        memset(&tm, 0, sizeof(tm));
        /* Expect: YYYY-MM-DDTHH:MM:SS (%d-%d-%dT%d:%d:%d)  eg 2010-11-28T14:29:01 */
        if (/* year */
            virStrToLong_i(validTo, &tmp, 10, &tm.tm_year) < 0 || *tmp != '-' ||
            /* month */
            virStrToLong_i(tmp+1, &tmp, 10, &tm.tm_mon) < 0 || *tmp != '-' ||
            /* day */
            virStrToLong_i(tmp+1, &tmp, 10, &tm.tm_mday) < 0 || *tmp != 'T' ||
            /* hour */
            virStrToLong_i(tmp+1, &tmp, 10, &tm.tm_hour) < 0 || *tmp != ':' ||
            /* minute */
            virStrToLong_i(tmp+1, &tmp, 10, &tm.tm_min) < 0 || *tmp != ':' ||
            /* second */
            virStrToLong_i(tmp+1, &tmp, 10, &tm.tm_sec) < 0 || *tmp != '\0') {
            virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                 _("cannot parse password validity time '%s', expect YYYY-MM-DDTHH:MM:SS"),
                                 validTo);
            VIR_FREE(validTo);
            VIR_FREE(def->passwd);
            return -1;
        }
        VIR_FREE(validTo);

        tm.tm_year -= 1900; /* Human epoch starts at 0 BC, not 1900BC */
        tm.tm_mon--; /* Humans start months at 1, computers at 0 */

        def->validTo = timegm(&tm);
        def->expires = 1;
    }

    if (connected) {
        int action = virDomainGraphicsAuthConnectedTypeFromString(connected);
        if (action <= 0) {
            virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                                 _("unknown connected value %s"),
                                 connected);
            VIR_FREE(connected);
            return -1;
        }
        VIR_FREE(connected);

        /* VNC supports connected='keep' only */
        if (type == VIR_DOMAIN_GRAPHICS_TYPE_VNC &&
            action != VIR_DOMAIN_GRAPHICS_AUTH_CONNECTED_KEEP) {
            virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                                 _("VNC supports connected='keep' only"));
            return -1;
        }

        def->connected = action;
    }

    return 0;
}

static int
virDomainGraphicsListenDefParseXML(virDomainGraphicsListenDefPtr def,
                                   xmlNodePtr node,
                                   unsigned int flags)
{
    int ret = -1;
    char *type     = virXMLPropString(node, "type");
    char *address  = virXMLPropString(node, "address");
    char *network  = virXMLPropString(node, "network");

    if (!type) {
        virDomainReportError(VIR_ERR_XML_ERROR, "%s",
                             _("graphics listen type must be specified"));
        goto error;
    }

    if ((def->type = virDomainGraphicsListenTypeFromString(type)) < 0) {
        virDomainReportError(VIR_ERR_XML_ERROR,
                             _("unknown graphics listen type '%s'"), type);
        goto error;
    }

    /* address is recognized if either type='address', or if
     * type='network' and we're looking at live XML (i.e. *not*
     * inactive). It is otherwise ignored. */
    if (address && address[0] &&
        ((def->type == VIR_DOMAIN_GRAPHICS_LISTEN_TYPE_ADDRESS) ||
         ((def->type == VIR_DOMAIN_GRAPHICS_LISTEN_TYPE_NETWORK) &&
          !(flags & VIR_DOMAIN_XML_INACTIVE)))) {
        def->address = address;
        address = NULL;
    }

    if (network && network[0]) {
        if (def->type != VIR_DOMAIN_GRAPHICS_LISTEN_TYPE_NETWORK) {
            /* network='xxx' never makes sense with anything except
             * type='address' */
            virDomainReportError(VIR_ERR_XML_ERROR, "%s",
                                 _("network attribute not allowed when listen type is not network"));
            goto error;
        }
        def->network = network;
        network = NULL;
    }

    ret = 0;
error:
    if (ret < 0)
        virDomainGraphicsListenDefClear(def);
    VIR_FREE(type);
    VIR_FREE(address);
    VIR_FREE(network);
    return ret;
}


/* Parse the XML definition for a graphics device */
static virDomainGraphicsDefPtr
virDomainGraphicsDefParseXML(xmlNodePtr node,
                             xmlXPathContextPtr ctxt,
                             unsigned int flags)
{
    virDomainGraphicsDefPtr def;
    char *type = NULL;
    int nListens;
    xmlNodePtr *listenNodes = NULL;
    char *listenAddr = NULL;
    xmlNodePtr save = ctxt->node;

    if (VIR_ALLOC(def) < 0) {
        virReportOOMError();
        return NULL;
    }

    ctxt->node = node;

    type = virXMLPropString(node, "type");

    if (!type) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                             "%s", _("missing graphics device type"));
        goto error;
    }

    if ((def->type = virDomainGraphicsTypeFromString(type)) < 0) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                             _("unknown graphics device type '%s'"), type);
        goto error;
    }

    if ((def->type == VIR_DOMAIN_GRAPHICS_TYPE_VNC) ||
        (def->type == VIR_DOMAIN_GRAPHICS_TYPE_RDP) ||
        (def->type == VIR_DOMAIN_GRAPHICS_TYPE_SPICE)) {

        /* parse the <listen> subelements for graphics types that support it */
        nListens = virXPathNodeSet("./listen", ctxt, &listenNodes);
        if (nListens < 0)
            goto error;

        if (nListens > 0) {
            int ii;

            if (VIR_ALLOC_N(def->listens, nListens) < 0) {
                virReportOOMError();
                goto error;
            }

            for (ii = 0; ii < nListens; ii++) {
                int ret = virDomainGraphicsListenDefParseXML(&def->listens[ii],
                                                             listenNodes[ii],
                                                             flags);
                if (ret < 0)
                    goto error;
                def->nListens++;
            }
            VIR_FREE(listenNodes);
        }

        /* listen attribute of <graphics> is also supported by these,
         * but must match the 'address' attribute of the first listen
         * that is type='address' (if present) */
        listenAddr = virXMLPropString(node, "listen");
        if (listenAddr && !listenAddr[0])
            VIR_FREE(listenAddr);

        if (listenAddr) {
            if (def->nListens == 0) {
                /* There were no <listen> elements, so we can just
                 * directly set listenAddr as listens[0]->address */
                if (virDomainGraphicsListenSetAddress(def, 0, listenAddr,
                                                      -1, true) < 0)
                    goto error;
            } else {
                /* There is at least 1 listen element, so we look for
                 * the first listen of type='address', and make sure
                 * its address matches the listen attribute from
                 * graphics. */
                bool matched = false;
                const char *found = NULL;
                int ii;

                for (ii = 0; ii < nListens; ii++) {
                    if (virDomainGraphicsListenGetType(def, ii)
                        == VIR_DOMAIN_GRAPHICS_LISTEN_TYPE_ADDRESS) {
                        found = virDomainGraphicsListenGetAddress(def, ii);
                        if (STREQ_NULLABLE(found, listenAddr)) {
                            matched = true;
                        }
                        break;
                    }
                }
                if (!matched) {
                    virDomainReportError(VIR_ERR_XML_ERROR,
                                         _("graphics listen attribute %s must match address "
                                           "attribute of first listen element (found %s)"),
                                         listenAddr, found ? found : "none");
                    goto error;
                }
            }
        }
    }

    if (def->type == VIR_DOMAIN_GRAPHICS_TYPE_VNC) {
        char *port = virXMLPropString(node, "port");
        char *autoport;

        if (port) {
            if (virStrToLong_i(port, NULL, 10, &def->data.vnc.port) < 0) {
                virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                     _("cannot parse vnc port %s"), port);
                VIR_FREE(port);
                goto error;
            }
            VIR_FREE(port);
            /* Legacy compat syntax, used -1 for auto-port */
            if (def->data.vnc.port == -1) {
                if (flags & VIR_DOMAIN_XML_INACTIVE)
                    def->data.vnc.port = 0;
                def->data.vnc.autoport = 1;
            }
        } else {
            def->data.vnc.port = 0;
            def->data.vnc.autoport = 1;
        }

        if ((autoport = virXMLPropString(node, "autoport")) != NULL) {
            if (STREQ(autoport, "yes")) {
                if (flags & VIR_DOMAIN_XML_INACTIVE)
                    def->data.vnc.port = 0;
                def->data.vnc.autoport = 1;
            }
            VIR_FREE(autoport);
        }

        def->data.vnc.socket = virXMLPropString(node, "socket");
        def->data.vnc.keymap = virXMLPropString(node, "keymap");

        if (virDomainGraphicsAuthDefParseXML(node, &def->data.vnc.auth,
                                             def->type) < 0)
            goto error;
    } else if (def->type == VIR_DOMAIN_GRAPHICS_TYPE_SDL) {
        char *fullscreen = virXMLPropString(node, "fullscreen");

        if (fullscreen != NULL) {
            if (STREQ(fullscreen, "yes")) {
                def->data.sdl.fullscreen = 1;
            } else if (STREQ(fullscreen, "no")) {
                def->data.sdl.fullscreen = 0;
            } else {
                virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                             _("unknown fullscreen value '%s'"), fullscreen);
                VIR_FREE(fullscreen);
                goto error;
            }
            VIR_FREE(fullscreen);
        } else
            def->data.sdl.fullscreen = 0;
        def->data.sdl.xauth = virXMLPropString(node, "xauth");
        def->data.sdl.display = virXMLPropString(node, "display");
    } else if (def->type == VIR_DOMAIN_GRAPHICS_TYPE_RDP) {
        char *port = virXMLPropString(node, "port");
        char *autoport;
        char *replaceUser;
        char *multiUser;

        if (port) {
            if (virStrToLong_i(port, NULL, 10, &def->data.rdp.port) < 0) {
                virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                     _("cannot parse rdp port %s"), port);
                VIR_FREE(port);
                goto error;
            }
            VIR_FREE(port);
        } else {
            def->data.rdp.port = 0;
            def->data.rdp.autoport = 1;
        }

        if ((autoport = virXMLPropString(node, "autoport")) != NULL) {
            if (STREQ(autoport, "yes")) {
                if (flags & VIR_DOMAIN_XML_INACTIVE)
                    def->data.rdp.port = 0;
                def->data.rdp.autoport = 1;
            }
            VIR_FREE(autoport);
        }

        if ((replaceUser = virXMLPropString(node, "replaceUser")) != NULL) {
            if (STREQ(replaceUser, "yes")) {
                def->data.rdp.replaceUser = 1;
            }
            VIR_FREE(replaceUser);
        }

        if ((multiUser = virXMLPropString(node, "multiUser")) != NULL) {
            if (STREQ(multiUser, "yes")) {
                def->data.rdp.multiUser = 1;
            }
            VIR_FREE(multiUser);
        }

    } else if (def->type == VIR_DOMAIN_GRAPHICS_TYPE_DESKTOP) {
        char *fullscreen = virXMLPropString(node, "fullscreen");

        if (fullscreen != NULL) {
            if (STREQ(fullscreen, "yes")) {
                def->data.desktop.fullscreen = 1;
            } else if (STREQ(fullscreen, "no")) {
                def->data.desktop.fullscreen = 0;
            } else {
                virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                             _("unknown fullscreen value '%s'"), fullscreen);
                VIR_FREE(fullscreen);
                goto error;
            }
            VIR_FREE(fullscreen);
        } else
            def->data.desktop.fullscreen = 0;

        def->data.desktop.display = virXMLPropString(node, "display");
    } else if (def->type == VIR_DOMAIN_GRAPHICS_TYPE_SPICE) {
        xmlNodePtr cur;
        char *port = virXMLPropString(node, "port");
        char *tlsPort;
        char *autoport;

        if (port) {
            if (virStrToLong_i(port, NULL, 10, &def->data.spice.port) < 0) {
                virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                     _("cannot parse spice port %s"), port);
                VIR_FREE(port);
                goto error;
            }
            VIR_FREE(port);
        } else {
            def->data.spice.port = 5900;
        }

        tlsPort = virXMLPropString(node, "tlsPort");
        if (tlsPort) {
            if (virStrToLong_i(tlsPort, NULL, 10, &def->data.spice.tlsPort) < 0) {
                virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                     _("cannot parse spice tlsPort %s"), tlsPort);
                VIR_FREE(tlsPort);
                goto error;
            }
            VIR_FREE(tlsPort);
        } else {
            def->data.spice.tlsPort = 0;
        }

        if ((autoport = virXMLPropString(node, "autoport")) != NULL) {
            if (STREQ(autoport, "yes")) {
                if (flags & VIR_DOMAIN_XML_INACTIVE) {
                    def->data.spice.port = 0;
                    def->data.spice.tlsPort = 0;
                }
                def->data.spice.autoport = 1;
            }
            VIR_FREE(autoport);
        }

        def->data.spice.keymap = virXMLPropString(node, "keymap");

        if (virDomainGraphicsAuthDefParseXML(node, &def->data.spice.auth,
                                             def->type) < 0)
            goto error;

        cur = node->children;
        while (cur != NULL) {
            if (cur->type == XML_ELEMENT_NODE) {
                if (xmlStrEqual(cur->name, BAD_CAST "channel")) {
                    const char *name, *mode;
                    int nameval, modeval;
                    name = virXMLPropString(cur, "name");
                    mode = virXMLPropString(cur, "mode");

                    if (!name || !mode) {
                        virDomainReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                                             _("spice channel missing name/mode"));
                        VIR_FREE(name);
                        VIR_FREE(mode);
                        goto error;
                    }

                    if ((nameval = virDomainGraphicsSpiceChannelNameTypeFromString(name)) < 0) {
                        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                             _("unknown spice channel name %s"),
                                             name);
                        VIR_FREE(name);
                        VIR_FREE(mode);
                        goto error;
                    }
                    if ((modeval = virDomainGraphicsSpiceChannelModeTypeFromString(mode)) < 0) {
                        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                             _("unknown spice channel mode %s"),
                                             mode);
                        VIR_FREE(name);
                        VIR_FREE(mode);
                        goto error;
                    }
                    VIR_FREE(name);
                    VIR_FREE(mode);

                    def->data.spice.channels[nameval] = modeval;
                } else if (xmlStrEqual(cur->name, BAD_CAST "image")) {
                    const char *compression = virXMLPropString(cur, "compression");
                    int compressionVal;

                    if (!compression) {
                        virDomainReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                                             _("spice image missing compression"));
                        goto error;
                    }

                    if ((compressionVal =
                         virDomainGraphicsSpiceImageCompressionTypeFromString(compression)) <= 0) {
                        virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                                             _("unknown spice image compression %s"),
                                             compression);
                        VIR_FREE(compression);
                        goto error;
                    }
                    VIR_FREE(compression);

                    def->data.spice.image = compressionVal;
                } else if (xmlStrEqual(cur->name, BAD_CAST "jpeg")) {
                    const char *compression = virXMLPropString(cur, "compression");
                    int compressionVal;

                    if (!compression) {
                        virDomainReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                                             _("spice jpeg missing compression"));
                        goto error;
                    }

                    if ((compressionVal =
                         virDomainGraphicsSpiceJpegCompressionTypeFromString(compression)) <= 0) {
                        virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                                             _("unknown spice jpeg compression %s"),
                                             compression);
                        VIR_FREE(compression);
                        goto error;
                    }
                    VIR_FREE(compression);

                    def->data.spice.jpeg = compressionVal;
                } else if (xmlStrEqual(cur->name, BAD_CAST "zlib")) {
                    const char *compression = virXMLPropString(cur, "compression");
                    int compressionVal;

                    if (!compression) {
                        virDomainReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                                             _("spice zlib missing compression"));
                        goto error;
                    }

                    if ((compressionVal =
                         virDomainGraphicsSpiceZlibCompressionTypeFromString(compression)) <= 0) {
                        virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                                             _("unknown spice zlib compression %s"),
                                             compression);
                        VIR_FREE(compression);
                        goto error;
                    }
                    VIR_FREE(compression);

                    def->data.spice.zlib = compressionVal;
                } else if (xmlStrEqual(cur->name, BAD_CAST "playback")) {
                    const char *compression = virXMLPropString(cur, "compression");
                    int compressionVal;

                    if (!compression) {
                        virDomainReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                                             _("spice playback missing compression"));
                        goto error;
                    }

                    if ((compressionVal =
                         virDomainGraphicsSpicePlaybackCompressionTypeFromString(compression)) <= 0) {
                        virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                                             _("unknown spice playback compression"));
                        VIR_FREE(compression);
                        goto error;

                    }
                    VIR_FREE(compression);

                    def->data.spice.playback = compressionVal;
                } else if (xmlStrEqual(cur->name, BAD_CAST "streaming")) {
                    const char *mode = virXMLPropString(cur, "mode");
                    int modeVal;

                    if (!mode) {
                        virDomainReportError(VIR_ERR_XML_ERROR, "%s",
                                             _("spice streaming missing mode"));
                        goto error;
                    }
                    if ((modeVal =
                         virDomainGraphicsSpiceStreamingModeTypeFromString(mode)) <= 0) {
                        virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                                             _("unknown spice streaming mode"));
                        VIR_FREE(mode);
                        goto error;

                    }
                    VIR_FREE(mode);

                    def->data.spice.streaming = modeVal;
                } else if (xmlStrEqual(cur->name, BAD_CAST "clipboard")) {
                    const char *copypaste = virXMLPropString(cur, "copypaste");
                    int copypasteVal;

                    if (!copypaste) {
                        virDomainReportError(VIR_ERR_XML_ERROR, "%s",
                                             _("spice clipboard missing copypaste"));
                        goto error;
                    }

                    if ((copypasteVal =
                         virDomainGraphicsSpiceClipboardCopypasteTypeFromString(copypaste)) <= 0) {
                        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                             _("unknown copypaste value '%s'"), copypaste);
                        VIR_FREE(copypaste);
                        goto error;
                    }
                    VIR_FREE(copypaste);

                    def->data.spice.copypaste = copypasteVal;
                }
            }
            cur = cur->next;
        }
    }

cleanup:
    VIR_FREE(type);
    VIR_FREE(listenNodes);
    VIR_FREE(listenAddr);

    ctxt->node = save;
    return def;

error:
    virDomainGraphicsDefFree(def);
    def = NULL;
    goto cleanup;
}


static virDomainSoundDefPtr
virDomainSoundDefParseXML(const xmlNodePtr node,
                          unsigned int flags)
{
    char *model;
    virDomainSoundDefPtr def;

    if (VIR_ALLOC(def) < 0) {
        virReportOOMError();
        return NULL;
    }

    model = virXMLPropString(node, "model");
    if ((def->model = virDomainSoundModelTypeFromString(model)) < 0) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                             _("unknown sound model '%s'"), model);
        goto error;
    }

    if (virDomainDeviceInfoParseXML(node, &def->info, flags) < 0)
        goto error;

cleanup:
    VIR_FREE(model);

    return def;

error:
    virDomainSoundDefFree(def);
    def = NULL;
    goto cleanup;
}


static virDomainWatchdogDefPtr
virDomainWatchdogDefParseXML(const xmlNodePtr node,
                             unsigned int flags)
{

    char *model = NULL;
    char *action = NULL;
    virDomainWatchdogDefPtr def;

    if (VIR_ALLOC (def) < 0) {
        virReportOOMError();
        return NULL;
    }

    model = virXMLPropString (node, "model");
    if (model == NULL) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                             _("watchdog must contain model name"));
        goto error;
    }
    def->model = virDomainWatchdogModelTypeFromString (model);
    if (def->model < 0) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                             _("unknown watchdog model '%s'"), model);
        goto error;
    }

    action = virXMLPropString (node, "action");
    if (action == NULL)
        def->action = VIR_DOMAIN_WATCHDOG_ACTION_RESET;
    else {
        def->action = virDomainWatchdogActionTypeFromString (action);
        if (def->action < 0) {
            virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                 _("unknown watchdog action '%s'"), action);
            goto error;
        }
    }

    if (virDomainDeviceInfoParseXML(node, &def->info, flags) < 0)
        goto error;

cleanup:
    VIR_FREE (action);
    VIR_FREE (model);

    return def;

error:
    virDomainWatchdogDefFree (def);
    def = NULL;
    goto cleanup;
}


static virDomainMemballoonDefPtr
virDomainMemballoonDefParseXML(const xmlNodePtr node,
                               unsigned int flags)
{
    char *model;
    virDomainMemballoonDefPtr def;

    if (VIR_ALLOC(def) < 0) {
        virReportOOMError();
        return NULL;
    }

    model = virXMLPropString(node, "model");
    if (model == NULL) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                             _("balloon memory must contain model name"));
        goto error;
    }
    if ((def->model = virDomainMemballoonModelTypeFromString(model)) < 0) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                             _("unknown memory balloon model '%s'"), model);
        goto error;
    }

    if (virDomainDeviceInfoParseXML(node, &def->info, flags) < 0)
        goto error;

cleanup:
    VIR_FREE(model);

    return def;

error:
    virDomainMemballoonDefFree(def);
    def = NULL;
    goto cleanup;
}

static virSysinfoDefPtr
virSysinfoParseXML(const xmlNodePtr node,
                  xmlXPathContextPtr ctxt)
{
    virSysinfoDefPtr def;
    char *type;

    if (!xmlStrEqual(node->name, BAD_CAST "sysinfo")) {
        virDomainReportError(VIR_ERR_XML_ERROR, "%s",
                        _("XML does not contain expected 'sysinfo' element"));
        return(NULL);
    }

    if (VIR_ALLOC(def) < 0) {
        virReportOOMError();
        return(NULL);
    }

    type = virXMLPropString(node, "type");
    if (type == NULL) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                             _("sysinfo must contain a type attribute"));
        goto error;
    }
    if ((def->type = virSysinfoTypeFromString(type)) < 0) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                             _("unknown sysinfo type '%s'"), type);
        goto error;
    }


    /* Extract BIOS related metadata */
    def->bios_vendor =
        virXPathString("string(bios/entry[@name='vendor'])", ctxt);
    def->bios_version =
        virXPathString("string(bios/entry[@name='version'])", ctxt);
    def->bios_date =
        virXPathString("string(bios/entry[@name='date'])", ctxt);
    def->bios_release =
        virXPathString("string(bios/entry[@name='release'])", ctxt);

    /* Extract system related metadata */
    def->system_manufacturer =
        virXPathString("string(system/entry[@name='manufacturer'])", ctxt);
    def->system_product =
        virXPathString("string(system/entry[@name='product'])", ctxt);
    def->system_version =
        virXPathString("string(system/entry[@name='version'])", ctxt);
    def->system_serial =
        virXPathString("string(system/entry[@name='serial'])", ctxt);
    def->system_uuid =
        virXPathString("string(system/entry[@name='uuid'])", ctxt);
    def->system_sku =
        virXPathString("string(system/entry[@name='sku'])", ctxt);
    def->system_family =
        virXPathString("string(system/entry[@name='family'])", ctxt);

cleanup:
    VIR_FREE(type);
    return(def);

error:
    virSysinfoDefFree(def);
    def = NULL;
    goto cleanup;
}

int
virDomainVideoDefaultRAM(virDomainDefPtr def,
                         int type)
{
    switch (type) {
        /* Wierd, QEMU defaults to 9 MB ??! */
    case VIR_DOMAIN_VIDEO_TYPE_VGA:
    case VIR_DOMAIN_VIDEO_TYPE_CIRRUS:
    case VIR_DOMAIN_VIDEO_TYPE_VMVGA:
        if (def->virtType == VIR_DOMAIN_VIRT_VBOX)
            return 8 * 1024;
        else if (def->virtType == VIR_DOMAIN_VIRT_VMWARE)
            return 4 * 1024;
        else
            return 9 * 1024;
        break;

    case VIR_DOMAIN_VIDEO_TYPE_XEN:
        /* Original Xen PVFB hardcoded to 4 MB */
        return 4 * 1024;

    case VIR_DOMAIN_VIDEO_TYPE_QXL:
        /* QEMU use 64M as the minimal video video memory for qxl device */
        return 64 * 1024;

    default:
        return 0;
    }
}


int
virDomainVideoDefaultType(virDomainDefPtr def)
{
    switch (def->virtType) {
    case VIR_DOMAIN_VIRT_TEST:
    case VIR_DOMAIN_VIRT_QEMU:
    case VIR_DOMAIN_VIRT_KQEMU:
    case VIR_DOMAIN_VIRT_KVM:
    case VIR_DOMAIN_VIRT_XEN:
        if (def->os.type &&
            (STREQ(def->os.type, "xen") ||
             STREQ(def->os.type, "linux")))
            return VIR_DOMAIN_VIDEO_TYPE_XEN;
        else
            return VIR_DOMAIN_VIDEO_TYPE_CIRRUS;

    case VIR_DOMAIN_VIRT_VBOX:
        return VIR_DOMAIN_VIDEO_TYPE_VBOX;

    case VIR_DOMAIN_VIRT_VMWARE:
        return VIR_DOMAIN_VIDEO_TYPE_VMVGA;

    default:
        return -1;
    }
}

static virDomainVideoAccelDefPtr
virDomainVideoAccelDefParseXML(const xmlNodePtr node) {
    xmlNodePtr cur;
    virDomainVideoAccelDefPtr def;
    char *support3d = NULL;
    char *support2d = NULL;

    cur = node->children;
    while (cur != NULL) {
        if (cur->type == XML_ELEMENT_NODE) {
            if ((support3d == NULL) && (support2d == NULL) &&
                xmlStrEqual(cur->name, BAD_CAST "acceleration")) {
                support3d = virXMLPropString(cur, "accel3d");
                support2d = virXMLPropString(cur, "accel2d");
            }
        }
        cur = cur->next;
    }

    if ((support3d == NULL) && (support2d == NULL))
        return(NULL);

    if (VIR_ALLOC(def) < 0) {
        virReportOOMError();
        return NULL;
    }

    if (support3d) {
        if (STREQ(support3d, "yes"))
            def->support3d = 1;
        else
            def->support3d = 0;
        VIR_FREE(support3d);
    }

    if (support2d) {
        if (STREQ(support2d, "yes"))
            def->support2d = 1;
        else
            def->support2d = 0;
        VIR_FREE(support2d);
    }

    return def;
}

static virDomainVideoDefPtr
virDomainVideoDefParseXML(const xmlNodePtr node,
                          virDomainDefPtr dom,
                          unsigned int flags)
{
    virDomainVideoDefPtr def;
    xmlNodePtr cur;
    char *type = NULL;
    char *heads = NULL;
    char *vram = NULL;

    if (VIR_ALLOC(def) < 0) {
        virReportOOMError();
        return NULL;
    }

    cur = node->children;
    while (cur != NULL) {
        if (cur->type == XML_ELEMENT_NODE) {
            if ((type == NULL) && (vram == NULL) && (heads == NULL) &&
                xmlStrEqual(cur->name, BAD_CAST "model")) {
                type = virXMLPropString(cur, "type");
                vram = virXMLPropString(cur, "vram");
                heads = virXMLPropString(cur, "heads");
                def->accel = virDomainVideoAccelDefParseXML(cur);
            }
        }
        cur = cur->next;
    }

    if (type) {
        if ((def->type = virDomainVideoTypeFromString(type)) < 0) {
            virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                 _("unknown video model '%s'"), type);
            goto error;
        }
    } else {
        if ((def->type = virDomainVideoDefaultType(dom)) < 0) {
            virDomainReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                                 _("missing video model and cannot determine default"));
            goto error;
        }
    }

    if (vram) {
        if (virStrToLong_ui(vram, NULL, 10, &def->vram) < 0) {
            virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                 _("cannot parse video ram '%s'"), vram);
            goto error;
        }
    } else {
        def->vram = virDomainVideoDefaultRAM(dom, def->type);
    }

    if (heads) {
        if (virStrToLong_ui(heads, NULL, 10, &def->heads) < 0) {
            virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                 _("cannot parse video heads '%s'"), heads);
            goto error;
        }
    } else {
        def->heads = 1;
    }

    if (virDomainDeviceInfoParseXML(node, &def->info, flags) < 0)
        goto error;

    VIR_FREE(type);
    VIR_FREE(vram);
    VIR_FREE(heads);

    return def;

error:
    virDomainVideoDefFree(def);
    VIR_FREE(type);
    VIR_FREE(vram);
    VIR_FREE(heads);
    return NULL;
}

static int
virDomainHostdevSubsysUsbDefParseXML(const xmlNodePtr node,
                                     virDomainHostdevDefPtr def)
{

    int ret = -1;
    int got_product, got_vendor;
    xmlNodePtr cur;

    /* Product can validly be 0, so we need some extra help to determine
     * if it is uninitialized*/
    got_product = 0;
    got_vendor = 0;

    cur = node->children;
    while (cur != NULL) {
        if (cur->type == XML_ELEMENT_NODE) {
            if (xmlStrEqual(cur->name, BAD_CAST "vendor")) {
                char *vendor = virXMLPropString(cur, "id");

                if (vendor) {
                    got_vendor = 1;
                    if (virStrToLong_ui(vendor, NULL, 0,
                                    &def->source.subsys.u.usb.vendor) < 0) {
                        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                 _("cannot parse vendor id %s"), vendor);
                        VIR_FREE(vendor);
                        goto out;
                    }
                    VIR_FREE(vendor);
                } else {
                    virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                         "%s", _("usb vendor needs id"));
                    goto out;
                }
            } else if (xmlStrEqual(cur->name, BAD_CAST "product")) {
                char* product = virXMLPropString(cur, "id");

                if (product) {
                    got_product = 1;
                    if (virStrToLong_ui(product, NULL, 0,
                                        &def->source.subsys.u.usb.product) < 0) {
                        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                             _("cannot parse product %s"),
                                             product);
                        VIR_FREE(product);
                        goto out;
                    }
                    VIR_FREE(product);
                } else {
                    virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                         "%s", _("usb product needs id"));
                    goto out;
                }
            } else if (xmlStrEqual(cur->name, BAD_CAST "address")) {
                char *bus, *device;

                bus = virXMLPropString(cur, "bus");
                if (bus) {
                    if (virStrToLong_ui(bus, NULL, 0,
                                        &def->source.subsys.u.usb.bus) < 0) {
                        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                             _("cannot parse bus %s"), bus);
                        VIR_FREE(bus);
                        goto out;
                    }
                    VIR_FREE(bus);
                } else {
                    virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                         "%s", _("usb address needs bus id"));
                    goto out;
                }

                device = virXMLPropString(cur, "device");
                if (device) {
                    if (virStrToLong_ui(device, NULL, 0,
                                        &def->source.subsys.u.usb.device) < 0)  {
                        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                             _("cannot parse device %s"),
                                             device);
                        VIR_FREE(device);
                        goto out;
                    }
                    VIR_FREE(device);
                } else {
                    virDomainReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                                         _("usb address needs device id"));
                    goto out;
                }
            } else {
                virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                     _("unknown usb source type '%s'"),
                                     cur->name);
                goto out;
            }
        }
        cur = cur->next;
    }

    if (got_vendor && def->source.subsys.u.usb.vendor == 0) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
            "%s", _("vendor cannot be 0."));
        goto out;
    }

    if (!got_vendor && got_product) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
            "%s", _("missing vendor"));
        goto out;
    }
    if (got_vendor && !got_product) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
            "%s", _("missing product"));
        goto out;
    }

    ret = 0;
out:
    return ret;
}


static int
virDomainHostdevSubsysPciDefParseXML(const xmlNodePtr node,
                                     virDomainHostdevDefPtr def,
                                     unsigned int flags)
{

    int ret = -1;
    xmlNodePtr cur;

    cur = node->children;
    while (cur != NULL) {
        if (cur->type == XML_ELEMENT_NODE) {
            if (xmlStrEqual(cur->name, BAD_CAST "address")) {
                virDomainDevicePCIAddressPtr addr =
                    &def->source.subsys.u.pci;

                if (virDomainDevicePCIAddressParseXML(cur, addr) < 0)
                    goto out;
            } else if ((flags & VIR_DOMAIN_XML_INTERNAL_STATUS) &&
                       xmlStrEqual(cur->name, BAD_CAST "state")) {
                /* Legacy back-compat. Don't add any more attributes here */
                char *devaddr = virXMLPropString(cur, "devaddr");
                if (devaddr &&
                    virDomainParseLegacyDeviceAddress(devaddr,
                                                      &def->info.addr.pci) < 0) {
                    virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                         _("Unable to parse devaddr parameter '%s'"),
                                         devaddr);
                    VIR_FREE(devaddr);
                    goto out;
                }
                def->info.type = VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI;
            } else {
                virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                     _("unknown pci source type '%s'"),
                                     cur->name);
                goto out;
            }
        }
        cur = cur->next;
    }

    ret = 0;
out:
    return ret;
}


static virDomainHostdevDefPtr
virDomainHostdevDefParseXML(const xmlNodePtr node,
                            virBitmapPtr bootMap,
                            unsigned int flags)
{

    xmlNodePtr cur;
    virDomainHostdevDefPtr def;
    char *mode, *type = NULL, *managed = NULL;

    if (VIR_ALLOC(def) < 0) {
        virReportOOMError();
        return NULL;
    }

    mode = virXMLPropString(node, "mode");
    if (mode) {
        if ((def->mode=virDomainHostdevModeTypeFromString(mode)) < 0) {
             virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                  _("unknown hostdev mode '%s'"), mode);
            goto error;
        }
    } else {
        def->mode = VIR_DOMAIN_HOSTDEV_MODE_SUBSYS;
    }

    type = virXMLPropString(node, "type");
    if (type) {
        if ((def->source.subsys.type = virDomainHostdevSubsysTypeFromString(type)) < 0) {
            virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                 _("unknown host device type '%s'"), type);
            goto error;
        }
    } else {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                             "%s", _("missing type in hostdev"));
        goto error;
    }

    managed = virXMLPropString(node, "managed");
    if (managed != NULL) {
        if (STREQ(managed, "yes"))
            def->managed = 1;
        VIR_FREE(managed);
    }

    cur = node->children;
    while (cur != NULL) {
        if (cur->type == XML_ELEMENT_NODE) {
            if (xmlStrEqual(cur->name, BAD_CAST "source")) {
                if (def->mode == VIR_DOMAIN_HOSTDEV_MODE_SUBSYS &&
                    def->source.subsys.type == VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_USB) {
                        if (virDomainHostdevSubsysUsbDefParseXML(cur, def) < 0)
                            goto error;
                }
                if (def->mode == VIR_DOMAIN_HOSTDEV_MODE_SUBSYS &&
                    def->source.subsys.type == VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_PCI) {
                        if (virDomainHostdevSubsysPciDefParseXML(cur, def, flags) < 0)
                            goto error;
                }
            } else if (xmlStrEqual(cur->name, BAD_CAST "address")) {
                /* address is parsed as part of virDomainDeviceInfoParseXML */
            } else if (xmlStrEqual(cur->name, BAD_CAST "alias")) {
                /* alias is parsed as part of virDomainDeviceInfoParseXML */
            } else if (xmlStrEqual(cur->name, BAD_CAST "boot")) {
                if (virDomainDeviceBootParseXML(cur, &def->bootIndex,
                                                bootMap))
                    goto error;
            } else if (xmlStrEqual(cur->name, BAD_CAST "rom")) {
                char *rombar = virXMLPropString(cur, "bar");
                if (!rombar) {
                    virDomainReportError(VIR_ERR_XML_ERROR,
                                         "%s", _("missing rom bar attribute"));
                    goto error;
                }
                if ((def->rombar = virDomainPciRombarModeTypeFromString(rombar)) <= 0) {
                    virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                                         _("unknown rom bar value '%s'"), rombar);
                    VIR_FREE(rombar);
                    goto error;
                }
                VIR_FREE(rombar);
            } else {
                virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                     _("unknown node %s"), cur->name);
            }
        }
        cur = cur->next;
    }

    if (def->info.type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_NONE) {
        if (virDomainDeviceInfoParseXML(node, &def->info, flags) < 0)
            goto error;
    }

    if (def->mode == VIR_DOMAIN_HOSTDEV_MODE_SUBSYS) {
        switch (def->source.subsys.type) {
        case VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_PCI:
            if (def->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_NONE &&
                def->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI) {
                virDomainReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                                     _("PCI host devices must use 'pci' address type"));
                goto error;
            }
            break;
        }
    }

cleanup:
    VIR_FREE(type);
    VIR_FREE(mode);
    return def;

error:
    virDomainHostdevDefFree(def);
    def = NULL;
    goto cleanup;
}


static int virDomainLifecycleParseXML(xmlXPathContextPtr ctxt,
                                      const char *xpath,
                                      int *val,
                                      int defaultVal,
                                      virLifecycleFromStringFunc convFunc)
{
    char *tmp = virXPathString(xpath, ctxt);
    if (tmp == NULL) {
        *val = defaultVal;
    } else {
        *val = convFunc(tmp);
        if (*val < 0) {
            virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                 _("unknown lifecycle action %s"), tmp);
            VIR_FREE(tmp);
            return -1;
        }
        VIR_FREE(tmp);
    }
    return 0;
}

static int
virSecurityLabelDefParseXML(const virDomainDefPtr def,
                            xmlXPathContextPtr ctxt,
                            unsigned int flags)
{
    char *p;

    if (virXPathNode("./seclabel", ctxt) == NULL)
        return 0;

    p = virXPathStringLimit("string(./seclabel/@type)",
                            VIR_SECURITY_LABEL_BUFLEN-1, ctxt);
    if (p == NULL) {
        virDomainReportError(VIR_ERR_XML_ERROR,
                             "%s", _("missing security type"));
        goto error;
    }
    def->seclabel.type = virDomainSeclabelTypeFromString(p);
    VIR_FREE(p);
    if (def->seclabel.type < 0) {
        virDomainReportError(VIR_ERR_XML_ERROR,
                             "%s", _("invalid security type"));
        goto error;
    }
    p = virXPathStringLimit("string(./seclabel/@relabel)",
                            VIR_SECURITY_LABEL_BUFLEN-1, ctxt);
    if (p != NULL) {
        if (STREQ(p, "yes")) {
            def->seclabel.norelabel = false;
        } else if (STREQ(p, "no")) {
            def->seclabel.norelabel = true;
        } else {
            virDomainReportError(VIR_ERR_XML_ERROR,
                                 _("invalid security relabel value %s"), p);
            VIR_FREE(p);
            goto error;
        }
        VIR_FREE(p);
        if (def->seclabel.type == VIR_DOMAIN_SECLABEL_DYNAMIC &&
            def->seclabel.norelabel) {
            virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                                 "%s", _("dynamic label type must use resource relabeling"));
            goto error;
        }
    } else {
        if (def->seclabel.type == VIR_DOMAIN_SECLABEL_STATIC)
            def->seclabel.norelabel = true;
        else
            def->seclabel.norelabel = false;
    }

    /* Only parse label, if using static labels, or
     * if the 'live' VM XML is requested
     */
    if (def->seclabel.type == VIR_DOMAIN_SECLABEL_STATIC ||
        !(flags & VIR_DOMAIN_XML_INACTIVE)) {
        p = virXPathStringLimit("string(./seclabel/label[1])",
                                VIR_SECURITY_LABEL_BUFLEN-1, ctxt);
        if (p == NULL) {
            virDomainReportError(VIR_ERR_XML_ERROR,
                                 "%s", _("security label is missing"));
            goto error;
        }

        def->seclabel.label = p;
    }

    /* Only parse imagelabel, if requested live XML with relabeling */
    if (!def->seclabel.norelabel &&
        !(flags & VIR_DOMAIN_XML_INACTIVE)) {
        p = virXPathStringLimit("string(./seclabel/imagelabel[1])",
                                VIR_SECURITY_LABEL_BUFLEN-1, ctxt);
        if (p == NULL) {
            virDomainReportError(VIR_ERR_XML_ERROR,
                                 "%s", _("security imagelabel is missing"));
            goto error;
        }
        def->seclabel.imagelabel = p;
    }

    /* Only parse baselabel, for dynamic label */
    if (def->seclabel.type == VIR_DOMAIN_SECLABEL_DYNAMIC) {
        p = virXPathStringLimit("string(./seclabel/baselabel[1])",
                                VIR_SECURITY_LABEL_BUFLEN-1, ctxt);
        if (p != NULL)
            def->seclabel.baselabel = p;
    }

    /* Only parse model, if static labelling, or a base
     * label is set, or doing active XML
     */
    if (def->seclabel.type == VIR_DOMAIN_SECLABEL_STATIC ||
        def->seclabel.baselabel ||
        !(flags & VIR_DOMAIN_XML_INACTIVE)) {
        p = virXPathStringLimit("string(./seclabel/@model)",
                                VIR_SECURITY_MODEL_BUFLEN-1, ctxt);
        if (p == NULL) {
            virDomainReportError(VIR_ERR_XML_ERROR,
                                 "%s", _("missing security model"));
            goto error;
        }
        def->seclabel.model = p;
    }

    return 0;

error:
    virSecurityLabelDefFree(def);
    return -1;
}

virDomainDeviceDefPtr virDomainDeviceDefParse(virCapsPtr caps,
                                              const virDomainDefPtr def,
                                              const char *xmlStr,
                                              unsigned int flags)
{
    xmlDocPtr xml;
    xmlNodePtr node;
    xmlXPathContextPtr ctxt = NULL;
    virDomainDeviceDefPtr dev = NULL;

    if (!(xml = virXMLParseStringCtxt(xmlStr, "device.xml", &ctxt))) {
        goto error;
    }
    node = ctxt->node;

    if (VIR_ALLOC(dev) < 0) {
        virReportOOMError();
        goto error;
    }

    if (xmlStrEqual(node->name, BAD_CAST "disk")) {
        dev->type = VIR_DOMAIN_DEVICE_DISK;
        if (!(dev->data.disk = virDomainDiskDefParseXML(caps, node,
                                                        NULL, flags)))
            goto error;
    } else if (xmlStrEqual(node->name, BAD_CAST "lease")) {
        dev->type = VIR_DOMAIN_DEVICE_LEASE;
        if (!(dev->data.lease = virDomainLeaseDefParseXML(node)))
            goto error;
    } else if (xmlStrEqual(node->name, BAD_CAST "filesystem")) {
        dev->type = VIR_DOMAIN_DEVICE_FS;
        if (!(dev->data.fs = virDomainFSDefParseXML(node, flags)))
            goto error;
    } else if (xmlStrEqual(node->name, BAD_CAST "interface")) {
        dev->type = VIR_DOMAIN_DEVICE_NET;
        if (!(dev->data.net = virDomainNetDefParseXML(caps, node, ctxt, NULL, 
                                                      flags)))
            goto error;
    } else if (xmlStrEqual(node->name, BAD_CAST "input")) {
        dev->type = VIR_DOMAIN_DEVICE_INPUT;
        if (!(dev->data.input = virDomainInputDefParseXML(def->os.type,
                                                          node, flags)))
            goto error;
    } else if (xmlStrEqual(node->name, BAD_CAST "sound")) {
        dev->type = VIR_DOMAIN_DEVICE_SOUND;
        if (!(dev->data.sound = virDomainSoundDefParseXML(node, flags)))
            goto error;
    } else if (xmlStrEqual(node->name, BAD_CAST "watchdog")) {
        dev->type = VIR_DOMAIN_DEVICE_WATCHDOG;
        if (!(dev->data.watchdog = virDomainWatchdogDefParseXML(node, flags)))
            goto error;
    } else if (xmlStrEqual(node->name, BAD_CAST "video")) {
        dev->type = VIR_DOMAIN_DEVICE_VIDEO;
        if (!(dev->data.video = virDomainVideoDefParseXML(node, def, flags)))
            goto error;
    } else if (xmlStrEqual(node->name, BAD_CAST "hostdev")) {
        dev->type = VIR_DOMAIN_DEVICE_HOSTDEV;
        if (!(dev->data.hostdev = virDomainHostdevDefParseXML(node, NULL,
                                                              flags)))
            goto error;
    } else if (xmlStrEqual(node->name, BAD_CAST "controller")) {
        dev->type = VIR_DOMAIN_DEVICE_CONTROLLER;
        if (!(dev->data.controller = virDomainControllerDefParseXML(node, flags)))
            goto error;
    } else if (xmlStrEqual(node->name, BAD_CAST "graphics")) {
        dev->type = VIR_DOMAIN_DEVICE_GRAPHICS;
        if (!(dev->data.graphics = virDomainGraphicsDefParseXML(node, ctxt, flags)))
            goto error;
    } else {
        virDomainReportError(VIR_ERR_XML_ERROR,
                             "%s", _("unknown device type"));
        goto error;
    }

    xmlFreeDoc(xml);
    xmlXPathFreeContext(ctxt);
    return dev;

  error:
    xmlFreeDoc(xml);
    xmlXPathFreeContext(ctxt);
    VIR_FREE(dev);
    return NULL;
}


static const char *
virDomainChrTargetTypeToString(int deviceType,
                               int targetType)
{
    const char *type = NULL;

    switch (deviceType) {
    case VIR_DOMAIN_CHR_DEVICE_TYPE_CHANNEL:
        type = virDomainChrChannelTargetTypeToString(targetType);
        break;
    case VIR_DOMAIN_CHR_DEVICE_TYPE_CONSOLE:
        type = virDomainChrConsoleTargetTypeToString(targetType);
        break;
    default:
        break;
    }

    return type;
}

int
virDomainDiskIndexByName(virDomainDefPtr def, const char *name,
                         bool allow_ambiguous)
{
    virDomainDiskDefPtr vdisk;
    int i;
    int candidate = -1;

    /* We prefer the <target dev='name'/> name (it's shorter, required
     * for all disks, and should be unambiguous), but also support
     * <source file='name'/> (if unambiguous).  Assume dst if there is
     * no leading slash, source name otherwise.  */
    for (i = 0; i < def->ndisks; i++) {
        vdisk = def->disks[i];
        if (*name != '/') {
            if (STREQ(vdisk->dst, name))
                return i;
        } else if (vdisk->src &&
                   STREQ(vdisk->src, name)) {
            if (allow_ambiguous)
                return i;
            if (candidate >= 0)
                return -1;
            candidate = i;
        }
    }
    return candidate;
}

/* Return the path to a disk image if a string identifies at least one
 * disk belonging to the domain (both device strings 'vda' and paths
 * '/path/to/file' are converted into '/path/to/file').  */
const char *
virDomainDiskPathByName(virDomainDefPtr def, const char *name)
{
    int i = virDomainDiskIndexByName(def, name, true);

    return i < 0 ? NULL : def->disks[i]->src;
}

int virDomainDiskInsert(virDomainDefPtr def,
                        virDomainDiskDefPtr disk)
{

    if (VIR_REALLOC_N(def->disks, def->ndisks+1) < 0)
        return -1;

    virDomainDiskInsertPreAlloced(def, disk);

    return 0;
}

void virDomainDiskInsertPreAlloced(virDomainDefPtr def,
                                   virDomainDiskDefPtr disk)
{
    int i;
    /* Tenatively plan to insert disk at the end. */
    int insertAt = -1;

    /* Then work backwards looking for disks on
     * the same bus. If we find a disk with a drive
     * index greater than the new one, insert at
     * that position
     */
    for (i = (def->ndisks - 1) ; i >= 0 ; i--) {
        /* If bus matches and current disk is after
         * new disk, then new disk should go here */
        if (def->disks[i]->bus == disk->bus &&
            (virDiskNameToIndex(def->disks[i]->dst) >
             virDiskNameToIndex(disk->dst))) {
            insertAt = i;
        } else if (def->disks[i]->bus == disk->bus &&
                   insertAt == -1) {
            /* Last disk with match bus is before the
             * new disk, then put new disk just after
             */
            insertAt = i + 1;
        }
    }

    /* No disks with this bus yet, so put at end of list */
    if (insertAt == -1)
        insertAt = def->ndisks;

    if (insertAt < def->ndisks)
        memmove(def->disks + insertAt + 1,
                def->disks + insertAt,
                (sizeof(def->disks[0]) * (def->ndisks-insertAt)));

    def->disks[insertAt] = disk;
    def->ndisks++;
}


void virDomainDiskRemove(virDomainDefPtr def, size_t i)
{
    if (def->ndisks > 1) {
        memmove(def->disks + i,
                def->disks + i + 1,
                sizeof(*def->disks) *
                (def->ndisks - (i + 1)));
        def->ndisks--;
        if (VIR_REALLOC_N(def->disks, def->ndisks) < 0) {
            /* ignore, harmless */
        }
    } else {
        VIR_FREE(def->disks);
        def->ndisks = 0;
    }
}

int virDomainDiskRemoveByName(virDomainDefPtr def, const char *name)
{
    int i = virDomainDiskIndexByName(def, name, false);
    if (i < 0)
        return -1;
    virDomainDiskRemove(def, i);
    return 0;
}

int virDomainNetInsert(virDomainDefPtr def, virDomainNetDefPtr net)
{
    if (VIR_REALLOC_N(def->nets, def->nnets + 1) < 0)
        return -1;
    def->nets[def->nnets]  = net;
    def->nnets++;
    return 0;
}

int virDomainNetIndexByMac(virDomainDefPtr def, const unsigned char *mac)
{
    int i;

    for (i = 0; i < def->nnets; i++)
        if (!memcmp(def->nets[i]->mac, mac, VIR_MAC_BUFLEN))
            return i;
    return -1;
}

static void virDomainNetRemove(virDomainDefPtr def, size_t i)
{
    if (def->nnets > 1) {
        memmove(def->nets + i,
                def->nets + i + 1,
                sizeof(*def->nets) * (def->nnets - (i + 1)));
        def->nnets--;
        if (VIR_REALLOC_N(def->nets, def->nnets) < 0) {
            /* ignore harmless */
        }
    } else {
        VIR_FREE(def->nets);
        def->nnets = 0;
    }
}

int virDomainNetRemoveByMac(virDomainDefPtr def, const unsigned char *mac)
{
    int i = virDomainNetIndexByMac(def, mac);

    if (i < 0)
        return -1;
    virDomainNetRemove(def, i);
    return 0;
}


int virDomainControllerInsert(virDomainDefPtr def,
                              virDomainControllerDefPtr controller)
{

    if (VIR_REALLOC_N(def->controllers, def->ncontrollers+1) < 0)
        return -1;

    virDomainControllerInsertPreAlloced(def, controller);

    return 0;
}

void virDomainControllerInsertPreAlloced(virDomainDefPtr def,
                                         virDomainControllerDefPtr controller)
{
    int i;
    /* Tenatively plan to insert controller at the end. */
    int insertAt = -1;

    /* Then work backwards looking for controllers of
     * the same type. If we find a controller with a
     * index greater than the new one, insert at
     * that position
     */
    for (i = (def->ncontrollers - 1) ; i >= 0 ; i--) {
        /* If bus matches and current controller is after
         * new controller, then new controller should go here */
        if ((def->controllers[i]->type == controller->type) &&
            (def->controllers[i]->idx > controller->idx)) {
            insertAt = i;
        } else if (def->controllers[i]->type == controller->type &&
                   insertAt == -1) {
            /* Last controller with match bus is before the
             * new controller, then put new controller just after
             */
            insertAt = i + 1;
        }
    }

    /* No controllers with this bus yet, so put at end of list */
    if (insertAt == -1)
        insertAt = def->ncontrollers;

    if (insertAt < def->ncontrollers)
        memmove(def->controllers + insertAt + 1,
                def->controllers + insertAt,
                (sizeof(def->controllers[0]) * (def->ncontrollers-insertAt)));

    def->controllers[insertAt] = controller;
    def->ncontrollers++;
}


int virDomainLeaseIndex(virDomainDefPtr def,
                        virDomainLeaseDefPtr lease)
{
    virDomainLeaseDefPtr vlease;
    int i;

    for (i = 0; i < def->nleases; i++) {
        vlease = def->leases[i];
        /* Either both must have lockspaces present which  match.. */
        if (vlease->lockspace && lease->lockspace &&
            STRNEQ(vlease->lockspace, lease->lockspace))
            continue;
        /* ...or neither must have a lockspace present */
        if (vlease->lockspace || lease->lockspace)
            continue;
        if (STREQ(vlease->key, lease->key))
            return i;
    }
    return -1;
}


int virDomainLeaseInsertPreAlloc(virDomainDefPtr def)
{
    if (VIR_EXPAND_N(def->leases, def->nleases, 1) < 0) {
        virReportOOMError();
        return -1;
    }
    return 0;
}

int virDomainLeaseInsert(virDomainDefPtr def,
                         virDomainLeaseDefPtr lease)
{
    if (virDomainLeaseInsertPreAlloc(def) < 0)
        return -1;

    virDomainLeaseInsertPreAlloced(def, lease);
    return 0;
}


void virDomainLeaseInsertPreAlloced(virDomainDefPtr def,
                                    virDomainLeaseDefPtr lease)
{
    if (lease == NULL)
        VIR_SHRINK_N(def->leases, def->nleases, 1);
    else
        def->leases[def->nleases-1] = lease;
}


void virDomainLeaseRemoveAt(virDomainDefPtr def, size_t i)
{
    if (def->nleases > 1) {
        memmove(def->leases + i,
                def->leases + i + 1,
                sizeof(*def->leases) *
                (def->nleases - (i + 1)));
        VIR_SHRINK_N(def->leases, def->nleases, 1);
    } else {
        VIR_FREE(def->leases);
        def->nleases = 0;
    }
}


int virDomainLeaseRemove(virDomainDefPtr def,
                         virDomainLeaseDefPtr lease)
{
    int i = virDomainLeaseIndex(def, lease);
    if (i < 0)
        return -1;
    virDomainLeaseRemoveAt(def, i);
    return 0;
}


static char *virDomainDefDefaultEmulator(virDomainDefPtr def,
                                         virCapsPtr caps) {
    const char *type;
    const char *emulator;
    char *retemu;

    type = virDomainVirtTypeToString(def->virtType);
    if (!type) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                             "%s", _("unknown virt type"));
        return NULL;
    }

    emulator = virCapabilitiesDefaultGuestEmulator(caps,
                                                   def->os.type,
                                                   def->os.arch,
                                                   type);

    if (!emulator) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                             _("no emulator for domain %s os type %s on architecture %s"),
                             type, def->os.type, def->os.arch);
        return NULL;
    }

    retemu = strdup(emulator);
    if (!retemu)
        virReportOOMError();

    return retemu;
}

static int
virDomainDefParseBootXML(xmlXPathContextPtr ctxt,
                         virDomainDefPtr def,
                         unsigned long *bootCount)
{
    xmlNodePtr *nodes = NULL;
    int i, n;
    char *bootstr;
    char *useserial = NULL;
    int ret = -1;
    unsigned long deviceBoot, serialPorts;

    if (virXPathULong("count(./devices/disk[boot]"
                      "|./devices/interface[boot]"
                      "|./devices/hostdev[boot])", ctxt, &deviceBoot) < 0) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                             _("cannot count boot devices"));
        goto cleanup;
    }

    /* analysis of the boot devices */
    if ((n = virXPathNodeSet("./os/boot", ctxt, &nodes)) < 0) {
        goto cleanup;
    }

    if (n > 0 && deviceBoot) {
        virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                             _("per-device boot elements cannot be used"
                               " together with os/boot elements"));
        goto cleanup;
    }

    for (i = 0 ; i < n && i < VIR_DOMAIN_BOOT_LAST ; i++) {
        int val;
        char *dev = virXMLPropString(nodes[i], "dev");
        if (!dev) {
            virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                 "%s", _("missing boot device"));
            goto cleanup;
        }
        if ((val = virDomainBootTypeFromString(dev)) < 0) {
            virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                 _("unknown boot device '%s'"),
                                 dev);
            VIR_FREE(dev);
            goto cleanup;
        }
        VIR_FREE(dev);
        def->os.bootDevs[def->os.nBootDevs++] = val;
    }
    if (def->os.nBootDevs == 0 && !deviceBoot) {
        def->os.nBootDevs = 1;
        def->os.bootDevs[0] = VIR_DOMAIN_BOOT_DISK;
    }

    bootstr = virXPathString("string(./os/bootmenu[1]/@enable)", ctxt);
    if (bootstr) {
        if (STREQ(bootstr, "yes"))
            def->os.bootmenu = VIR_DOMAIN_BOOT_MENU_ENABLED;
        else
            def->os.bootmenu = VIR_DOMAIN_BOOT_MENU_DISABLED;
        VIR_FREE(bootstr);
    }

    useserial = virXPathString("string(./os/bios[1]/@useserial)", ctxt);
    if (useserial) {
        if (STREQ(useserial, "yes")) {
            if (virXPathULong("count(./devices/serial)",
                              ctxt, &serialPorts) < 0) {
                virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                                     _("need at least one serial port "
                                       "for useserial"));
                goto cleanup;
            }
            def->os.bios.useserial = VIR_DOMAIN_BIOS_USESERIAL_YES;
        } else {
            def->os.bios.useserial = VIR_DOMAIN_BIOS_USESERIAL_NO;
        }
    }

    *bootCount = deviceBoot;
    ret = 0;

cleanup:
    VIR_FREE(useserial);
    VIR_FREE(nodes);
    return ret;
}

/* Parse the XML definition for a vcpupin */
static virDomainVcpuPinDefPtr
virDomainVcpuPinDefParseXML(const xmlNodePtr node,
                            xmlXPathContextPtr ctxt,
                            int maxvcpus)
{
    virDomainVcpuPinDefPtr def;
    xmlNodePtr oldnode = ctxt->node;
    unsigned int vcpuid;
    char *tmp = NULL;
    int ret;

    if (VIR_ALLOC(def) < 0) {
        virReportOOMError();
        return NULL;
    }

    ctxt->node = node;

    ret = virXPathUInt("string(./@vcpu)", ctxt, &vcpuid);
    if (ret == -2) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                             "%s", _("vcpu id must be an unsigned integer"));
        goto error;
    } else if (ret == -1) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                             "%s", _("can't parse vcpupin node"));
        goto error;
    }

    if (vcpuid >= maxvcpus) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                             "%s", _("vcpu id must be less than maxvcpus"));
        goto error;
    }

    def->vcpuid = vcpuid;

    tmp = virXMLPropString(node, "cpuset");

    if (tmp) {
        char *set = tmp;
        int cpumasklen = VIR_DOMAIN_CPUMASK_LEN;

        if (VIR_ALLOC_N(def->cpumask, cpumasklen) < 0) {
            virReportOOMError();
            goto error;
        }
        if (virDomainCpuSetParse((const char **)&set,
                                 0, def->cpumask,
                                 cpumasklen) < 0)
           goto error;
        VIR_FREE(tmp);
    } else {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                             "%s", _("missing cpuset for vcpupin"));
        goto error;
    }

cleanup:
    ctxt->node = oldnode;
    return def;

error:
    VIR_FREE(def);
    goto cleanup;
}


static virDomainDefPtr virDomainDefParseXML(virCapsPtr caps,
                                            xmlDocPtr xml,
                                            xmlNodePtr root,
                                            xmlXPathContextPtr ctxt,
                                            unsigned int expectedVirtTypes,
                                            unsigned int flags)
{
    xmlNodePtr *nodes = NULL, node = NULL;
    char *tmp = NULL;
    int i, n;
    long id = -1;
    virDomainDefPtr def;
    unsigned long count;
    bool uuid_generated = false;
    virBitmapPtr bootMap = NULL;
    unsigned long bootMapSize = 0;

    if (VIR_ALLOC(def) < 0) {
        virReportOOMError();
        return NULL;
    }

    if (!(flags & VIR_DOMAIN_XML_INACTIVE))
        if ((virXPathLong("string(./@id)", ctxt, &id)) < 0)
            id = -1;
    def->id = (int)id;

    /* Find out what type of virtualization to use */
    if (!(tmp = virXPathString("string(./@type)", ctxt))) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                             "%s", _("missing domain type attribute"));
        goto error;
    }

    if ((def->virtType = virDomainVirtTypeFromString(tmp)) < 0) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                             _("invalid domain type %s"), tmp);
        goto error;
    }
    VIR_FREE(tmp);

    if ((expectedVirtTypes & (1 << def->virtType)) == 0) {
        if (count_one_bits(expectedVirtTypes) == 1) {
            virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                 _("unexpected domain type %s, expecting %s"),
                                 virDomainVirtTypeToString(def->virtType),
                                 virDomainVirtTypeToString(ffs(expectedVirtTypes) - 1));
        } else {
            virBuffer buffer = VIR_BUFFER_INITIALIZER;
            char *string;

            for (i = 0; i < VIR_DOMAIN_VIRT_LAST; ++i) {
                if ((expectedVirtTypes & (1 << i)) != 0) {
                    if (virBufferUse(&buffer) > 0)
                        virBufferAddLit(&buffer, ", ");

                    virBufferAdd(&buffer, virDomainVirtTypeToString(i), -1);
                }
            }

            if (virBufferError(&buffer)) {
                virReportOOMError();
                virBufferFreeAndReset(&buffer);
                goto error;
            }

            string = virBufferContentAndReset(&buffer);

            virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                 _("unexpected domain type %s, "
                                   "expecting one of these: %s"),
                                 virDomainVirtTypeToString(def->virtType),
                                 string);

            VIR_FREE(string);
        }

        goto error;
    }

    /* Extract domain name */
    if (!(def->name = virXPathString("string(./name[1])", ctxt))) {
        virDomainReportError(VIR_ERR_NO_NAME, NULL);
        goto error;
    }

    /* Extract domain uuid. If both uuid and sysinfo/system/entry/uuid
     * exist, they must match; and if only the latter exists, it can
     * also serve as the uuid. */
    tmp = virXPathString("string(./uuid[1])", ctxt);
    if (!tmp) {
        if (virUUIDGenerate(def->uuid)) {
            virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                 "%s", _("Failed to generate UUID"));
            goto error;
        }
        uuid_generated = true;
    } else {
        if (virUUIDParse(tmp, def->uuid) < 0) {
            virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                 "%s", _("malformed uuid element"));
            goto error;
        }
        VIR_FREE(tmp);
    }

    /* Extract documentation if present */
    def->description = virXPathString("string(./description[1])", ctxt);

    /* Extract domain memory */
    if (virXPathULong("string(./memory[1])", ctxt,
                      &def->mem.max_balloon) < 0) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                             "%s", _("missing memory element"));
        goto error;
    }

    if (virXPathULong("string(./currentMemory[1])", ctxt,
                      &def->mem.cur_balloon) < 0)
        def->mem.cur_balloon = def->mem.max_balloon;

    node = virXPathNode("./memoryBacking/hugepages", ctxt);
    if (node)
        def->mem.hugepage_backed = 1;

    node = virXPathNode("./memoryBacking/nosharepages", ctxt);
    if (node)
        def->mem.ksm_disabled = 1;

    /* Extract blkio cgroup tunables */
    if (virXPathUInt("string(./blkiotune/weight)", ctxt,
                     &def->blkio.weight) < 0)
        def->blkio.weight = 0;

    /* Extract other memory tunables */
    if (virXPathULong("string(./memtune/hard_limit)", ctxt,
                      &def->mem.hard_limit) < 0)
        def->mem.hard_limit = 0;

    if (virXPathULong("string(./memtune/soft_limit[1])", ctxt,
                      &def->mem.soft_limit) < 0)
        def->mem.soft_limit = 0;

    if (virXPathULong("string(./memtune/min_guarantee[1])", ctxt,
                      &def->mem.min_guarantee) < 0)
        def->mem.min_guarantee = 0;

    if (virXPathULong("string(./memtune/swap_hard_limit[1])", ctxt,
                      &def->mem.swap_hard_limit) < 0)
        def->mem.swap_hard_limit = 0;

    n = virXPathULong("string(./vcpu[1])", ctxt, &count);
    if (n == -2) {
        virDomainReportError(VIR_ERR_XML_ERROR, "%s",
                             _("maximum vcpus must be an integer"));
        goto error;
    } else if (n < 0) {
        def->maxvcpus = 1;
    } else {
        def->maxvcpus = count;
        if (count == 0) {
            virDomainReportError(VIR_ERR_XML_ERROR,
                                 _("invalid maxvcpus %lu"), count);
            goto error;
        }
    }

    n = virXPathULong("string(./vcpu[1]/@current)", ctxt, &count);
    if (n == -2) {
        virDomainReportError(VIR_ERR_XML_ERROR, "%s",
                             _("current vcpus must be an integer"));
        goto error;
    } else if (n < 0) {
        def->vcpus = def->maxvcpus;
    } else {
        def->vcpus = count;
        if (count == 0) {
            virDomainReportError(VIR_ERR_XML_ERROR,
                                 _("invalid current vcpus %lu"), count);
            goto error;
        }

        if (def->maxvcpus < count) {
            virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                _("maxvcpus must not be less than current vcpus (%d < %lu)"),
                def->maxvcpus, count);
            goto error;
        }
    }

    tmp = virXPathString("string(./vcpu[1]/@cpuset)", ctxt);
    if (tmp) {
        char *set = tmp;
        def->cpumasklen = VIR_DOMAIN_CPUMASK_LEN;
        if (VIR_ALLOC_N(def->cpumask, def->cpumasklen) < 0) {
            goto no_memory;
        }
        if (virDomainCpuSetParse((const char **)&set,
                                 0, def->cpumask,
                                 def->cpumasklen) < 0)
            goto error;
        VIR_FREE(tmp);
    }

    /* Extract cpu tunables. */
    if (virXPathULong("string(./cputune/shares[1])", ctxt,
                      &def->cputune.shares) < 0)
        def->cputune.shares = 0;

    if (virXPathULongLong("string(./cputune/period[1])", ctxt,
                          &def->cputune.period) < 0)
        def->cputune.period = 0;

    if (virXPathLongLong("string(./cputune/quota[1])", ctxt,
                         &def->cputune.quota) < 0)
        def->cputune.quota = 0;

    if ((n = virXPathNodeSet("./cputune/vcpupin", ctxt, &nodes)) < 0) {
        goto error;
    }

    if (n && VIR_ALLOC_N(def->cputune.vcpupin, n) < 0)
        goto no_memory;

    if (n > def->maxvcpus) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                             "%s", _("vcpupin nodes must be less than maxvcpus"));
        goto error;
    }

    for (i = 0 ; i < n ; i++) {
        virDomainVcpuPinDefPtr vcpupin = NULL;
        vcpupin = virDomainVcpuPinDefParseXML(nodes[i], ctxt, def->maxvcpus);

        if (!vcpupin)
            goto error;

        if (virDomainVcpuPinIsDuplicate(def->cputune.vcpupin,
                                        def->cputune.nvcpupin,
                                        vcpupin->vcpuid)) {
            virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                 "%s", _("duplicate vcpupin for same vcpu"));
            VIR_FREE(vcpupin);
            goto error;
        }

        def->cputune.vcpupin[def->cputune.nvcpupin++] = vcpupin;
    }
    VIR_FREE(nodes);

    /* Extract numatune if exists. */
    if ((n = virXPathNodeSet("./numatune", ctxt, NULL)) < 0) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                             "%s", _("cannot extract numatune nodes"));
        goto error;
    }

    if (n) {
        tmp = virXPathString("string(./numatune/memory/@nodeset)", ctxt);
        if (tmp) {
            char *set = tmp;
            int nodemasklen = VIR_DOMAIN_CPUMASK_LEN;

            if (VIR_ALLOC_N(def->numatune.memory.nodemask, nodemasklen) < 0) {
                goto no_memory;
            }

            /* "nodeset" leads same syntax with "cpuset". */
            if (virDomainCpuSetParse((const char **)&set,
                                     0, def->numatune.memory.nodemask,
                                     nodemasklen) < 0)
               goto error;
            VIR_FREE(tmp);
        } else {
            virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                "%s", _("nodeset for NUMA memory tuning must be set"));
            goto error;
        }

        tmp = virXPathString("string(./numatune/memory/@mode)", ctxt);
        if (tmp) {
            if ((def->numatune.memory.mode =
                virDomainNumatuneMemModeTypeFromString(tmp)) < 0) {
                virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                    _("Unsupported NUMA memory tuning mode '%s'"),
                                    tmp);
                goto error;
            }
            VIR_FREE(tmp);
        } else {
            def->numatune.memory.mode = VIR_DOMAIN_NUMATUNE_MEM_STRICT;
        }
    }

    n = virXPathNodeSet("./features/*", ctxt, &nodes);
    if (n < 0)
        goto error;
    if (n) {
        for (i = 0 ; i < n ; i++) {
            int val = virDomainFeatureTypeFromString((const char *)nodes[i]->name);
            if (val < 0) {
                virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                     _("unexpected feature %s"),
                                     nodes[i]->name);
                goto error;
            }
            def->features |= (1 << val);
        }
        VIR_FREE(nodes);
    }

    if (virDomainLifecycleParseXML(ctxt, "string(./on_reboot[1])",
                                   &def->onReboot, VIR_DOMAIN_LIFECYCLE_RESTART,
                                   virDomainLifecycleTypeFromString) < 0)
        goto error;

    if (virDomainLifecycleParseXML(ctxt, "string(./on_poweroff[1])",
                                   &def->onPoweroff, VIR_DOMAIN_LIFECYCLE_DESTROY,
                                   virDomainLifecycleTypeFromString) < 0)
        goto error;

    if (virDomainLifecycleParseXML(ctxt, "string(./on_crash[1])",
                                        &def->onCrash,
                                   VIR_DOMAIN_LIFECYCLE_CRASH_DESTROY,
                                   virDomainLifecycleCrashTypeFromString) < 0)
        goto error;

    tmp = virXPathString("string(./clock/@offset)", ctxt);
    if (tmp) {
        if ((def->clock.offset = virDomainClockOffsetTypeFromString(tmp)) < 0) {
            virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                 _("unknown clock offset '%s'"), tmp);
            goto error;
        }
        VIR_FREE(tmp);
    } else {
        def->clock.offset = VIR_DOMAIN_CLOCK_OFFSET_UTC;
    }
    switch (def->clock.offset) {
    case VIR_DOMAIN_CLOCK_OFFSET_VARIABLE:
        if (virXPathLongLong("number(./clock/@adjustment)", ctxt,
                             &def->clock.data.adjustment) < 0)
            def->clock.data.adjustment = 0;
        break;

    case VIR_DOMAIN_CLOCK_OFFSET_TIMEZONE:
        def->clock.data.timezone = virXPathString("string(./clock/@timezone)", ctxt);
        if (!def->clock.data.timezone) {
            virDomainReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                                 _("missing 'timezone' attribute for clock with offset='timezone'"));
            goto error;
        }
        break;
    }

    if ((n = virXPathNodeSet("./clock/timer", ctxt, &nodes)) < 0) {
        goto error;
    }
    if (n && VIR_ALLOC_N(def->clock.timers, n) < 0)
        goto no_memory;
    for (i = 0 ; i < n ; i++) {
        virDomainTimerDefPtr timer = virDomainTimerDefParseXML(nodes[i],
                                                               ctxt);
        if (!timer)
            goto error;

        def->clock.timers[def->clock.ntimers++] = timer;
    }
    VIR_FREE(nodes);

    def->os.bootloader = virXPathString("string(./bootloader)", ctxt);
    def->os.bootloaderArgs = virXPathString("string(./bootloader_args)", ctxt);

    def->os.type = virXPathString("string(./os/type[1])", ctxt);
    if (!def->os.type) {
        if (def->os.bootloader) {
            def->os.type = strdup("xen");
            if (!def->os.type) {
                goto no_memory;
            }
        } else {
            virDomainReportError(VIR_ERR_OS_TYPE,
                                 "%s", _("no OS type"));
            goto error;
        }
    }
    /*
     * HACK: For xen driver we previously used bogus 'linux' as the
     * os type for paravirt, whereas capabilities declare it to
     * be 'xen'. So we accept the former and convert
     */
    if (STREQ(def->os.type, "linux") &&
        def->virtType == VIR_DOMAIN_VIRT_XEN) {
        VIR_FREE(def->os.type);
        if (!(def->os.type = strdup("xen"))) {
            goto no_memory;
        }
    }

    if (!virCapabilitiesSupportsGuestOSType(caps, def->os.type)) {
        virDomainReportError(VIR_ERR_OS_TYPE,
                             "%s", def->os.type);
        goto error;
    }

    def->os.arch = virXPathString("string(./os/type[1]/@arch)", ctxt);
    if (def->os.arch) {
        if (!virCapabilitiesSupportsGuestArch(caps, def->os.type, def->os.arch)) {
            virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                 _("os type '%s' & arch '%s' combination is not supported"),
                                 def->os.type, def->os.arch);
            goto error;
        }
    } else {
        const char *defaultArch = virCapabilitiesDefaultGuestArch(caps, def->os.type, virDomainVirtTypeToString(def->virtType));
        if (defaultArch == NULL) {
            virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                 _("no supported architecture for os type '%s'"),
                                 def->os.type);
            goto error;
        }
        if (!(def->os.arch = strdup(defaultArch))) {
            goto no_memory;
        }
    }

    def->os.machine = virXPathString("string(./os/type[1]/@machine)", ctxt);
    if (!def->os.machine) {
        const char *defaultMachine = virCapabilitiesDefaultGuestMachine(caps,
                                                                        def->os.type,
                                                                        def->os.arch,
                                                                        virDomainVirtTypeToString(def->virtType));
        if (defaultMachine != NULL) {
            if (!(def->os.machine = strdup(defaultMachine))) {
                goto no_memory;
            }
        }
    }

    /*
     * Booting options for different OS types....
     *
     *   - A bootloader (and optional kernel+initrd)  (xen)
     *   - A kernel + initrd                          (xen)
     *   - A boot device (and optional kernel+initrd) (hvm)
     *   - An init script                             (exe)
     */

    if (STREQ(def->os.type, "exe")) {
        def->os.init = virXPathString("string(./os/init[1])", ctxt);
        if (!def->os.init) {
            if (caps->defaultInitPath) {
                def->os.init = strdup(caps->defaultInitPath);
                if (!def->os.init) {
                    goto no_memory;
                }
            } else {
                virDomainReportError(VIR_ERR_XML_ERROR, "%s",
                                     _("init binary must be specified"));
                goto error;
            }
        }
    }

    if (STREQ(def->os.type, "xen") ||
        STREQ(def->os.type, "hvm") ||
        STREQ(def->os.type, "uml")) {
        def->os.kernel = virXPathString("string(./os/kernel[1])", ctxt);
        def->os.initrd = virXPathString("string(./os/initrd[1])", ctxt);
        def->os.cmdline = virXPathString("string(./os/cmdline[1])", ctxt);
        def->os.root = virXPathString("string(./os/root[1])", ctxt);
        def->os.loader = virXPathString("string(./os/loader[1])", ctxt);
    }

    if (STREQ(def->os.type, "hvm")) {
        if (virDomainDefParseBootXML(ctxt, def, &bootMapSize) < 0)
            goto error;
        if (bootMapSize && !(bootMap = virBitmapAlloc(bootMapSize)))
            goto no_memory;
    }

    def->emulator = virXPathString("string(./devices/emulator[1])", ctxt);
    if (!def->emulator && virCapabilitiesIsEmulatorRequired(caps)) {
        def->emulator = virDomainDefDefaultEmulator(def, caps);
        if (!def->emulator)
            goto error;
    }

    /* analysis of the disk devices */
    if ((n = virXPathNodeSet("./devices/disk", ctxt, &nodes)) < 0) {
        goto error;
    }
    if (n && VIR_ALLOC_N(def->disks, n) < 0)
        goto no_memory;
    for (i = 0 ; i < n ; i++) {
        virDomainDiskDefPtr disk = virDomainDiskDefParseXML(caps,
                                                            nodes[i],
                                                            bootMap,
                                                            flags);
        if (!disk)
            goto error;

        virDomainDiskInsertPreAlloced(def, disk);
    }
    VIR_FREE(nodes);

    /* analysis of the controller devices */
    if ((n = virXPathNodeSet("./devices/controller", ctxt, &nodes)) < 0) {
        goto error;
    }
    if (n && VIR_ALLOC_N(def->controllers, n) < 0)
        goto no_memory;
    for (i = 0 ; i < n ; i++) {
        virDomainControllerDefPtr controller = virDomainControllerDefParseXML(nodes[i],
                                                                              flags);
        if (!controller)
            goto error;

        virDomainControllerInsertPreAlloced(def, controller);
    }
    VIR_FREE(nodes);

    /* analysis of the resource leases */
    if ((n = virXPathNodeSet("./devices/lease", ctxt, &nodes)) < 0) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                             "%s", _("cannot extract device leases"));
        goto error;
    }
    if (n && VIR_ALLOC_N(def->leases, n) < 0)
        goto no_memory;
    for (i = 0 ; i < n ; i++) {
        virDomainLeaseDefPtr lease = virDomainLeaseDefParseXML(nodes[i]);
        if (!lease)
            goto error;

        def->leases[def->nleases++] = lease;
    }
    VIR_FREE(nodes);

    /* analysis of the filesystems */
    if ((n = virXPathNodeSet("./devices/filesystem", ctxt, &nodes)) < 0) {
        goto error;
    }
    if (n && VIR_ALLOC_N(def->fss, n) < 0)
        goto no_memory;
    for (i = 0 ; i < n ; i++) {
        virDomainFSDefPtr fs = virDomainFSDefParseXML(nodes[i],
                                                      flags);
        if (!fs)
            goto error;

        def->fss[def->nfss++] = fs;
    }
    VIR_FREE(nodes);

    /* analysis of the network devices */
    if ((n = virXPathNodeSet("./devices/interface", ctxt, &nodes)) < 0) {
        goto error;
    }
    if (n && VIR_ALLOC_N(def->nets, n) < 0)
        goto no_memory;
    for (i = 0 ; i < n ; i++) {
        virDomainNetDefPtr net = virDomainNetDefParseXML(caps,
                                                         nodes[i],
                                                         ctxt,
                                                         bootMap,
                                                         flags);
        if (!net)
            goto error;

        def->nets[def->nnets++] = net;
    }
    VIR_FREE(nodes);


    /* analysis of the smartcard devices */
    if ((n = virXPathNodeSet("./devices/smartcard", ctxt, &nodes)) < 0) {
        goto error;
    }
    if (n && VIR_ALLOC_N(def->smartcards, n) < 0)
        goto no_memory;

    for (i = 0 ; i < n ; i++) {
        virDomainSmartcardDefPtr card = virDomainSmartcardDefParseXML(nodes[i],
                                                                      flags);
        if (!card)
            goto error;

        def->smartcards[def->nsmartcards++] = card;
    }
    VIR_FREE(nodes);


    /* analysis of the character devices */
    if ((n = virXPathNodeSet("./devices/parallel", ctxt, &nodes)) < 0) {
        goto error;
    }
    if (n && VIR_ALLOC_N(def->parallels, n) < 0)
        goto no_memory;

    for (i = 0 ; i < n ; i++) {
        virDomainChrDefPtr chr = virDomainChrDefParseXML(caps,
                                                         nodes[i],
                                                         flags);
        if (!chr)
            goto error;

        if (chr->target.port == -1) {
            int maxport = -1;
            int j;
            for (j = 0 ; j < i ; j++) {
                if (def->parallels[j]->target.port > maxport)
                    maxport = def->parallels[j]->target.port;
            }
            chr->target.port = maxport + 1;
        }
        def->parallels[def->nparallels++] = chr;
    }
    VIR_FREE(nodes);

    if ((n = virXPathNodeSet("./devices/serial", ctxt, &nodes)) < 0) {
        goto error;
    }
    if (n && VIR_ALLOC_N(def->serials, n) < 0)
        goto no_memory;

    for (i = 0 ; i < n ; i++) {
        virDomainChrDefPtr chr = virDomainChrDefParseXML(caps,
                                                         nodes[i],
                                                         flags);
        if (!chr)
            goto error;

        if (chr->target.port == -1) {
            int maxport = -1;
            int j;
            for (j = 0 ; j < i ; j++) {
                if (def->serials[j]->target.port > maxport)
                    maxport = def->serials[j]->target.port;
            }
            chr->target.port = maxport + 1;
        }
        def->serials[def->nserials++] = chr;
    }
    VIR_FREE(nodes);

    if ((node = virXPathNode("./devices/console[1]", ctxt)) != NULL) {
        virDomainChrDefPtr chr = virDomainChrDefParseXML(caps,
                                                         node,
                                                         flags);
        if (!chr)
            goto error;

        chr->target.port = 0;
        /*
         * For HVM console actually created a serial device
         * while for non-HVM it was a parvirt console
         */
        if (STREQ(def->os.type, "hvm") &&
            chr->targetType == VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_SERIAL) {
            if (def->nserials != 0) {
                virDomainChrDefFree(chr);
            } else {
                if (VIR_ALLOC_N(def->serials, 1) < 0) {
                    virDomainChrDefFree(chr);
                    goto no_memory;
                }
                def->nserials = 1;
                def->serials[0] = chr;
                chr->deviceType = VIR_DOMAIN_CHR_DEVICE_TYPE_SERIAL;
            }
        } else {
            def->console = chr;
        }
    }

    if ((n = virXPathNodeSet("./devices/channel", ctxt, &nodes)) < 0) {
        goto error;
    }
    if (n && VIR_ALLOC_N(def->channels, n) < 0)
        goto no_memory;

    for (i = 0 ; i < n ; i++) {
        virDomainChrDefPtr chr = virDomainChrDefParseXML(caps,
                                                         nodes[i],
                                                         flags);
        if (!chr)
            goto error;

        def->channels[def->nchannels++] = chr;

        if (chr->deviceType == VIR_DOMAIN_CHR_DEVICE_TYPE_CHANNEL &&
            chr->targetType == VIR_DOMAIN_CHR_CHANNEL_TARGET_TYPE_VIRTIO &&
            chr->info.type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_NONE)
            chr->info.type = VIR_DOMAIN_DEVICE_ADDRESS_TYPE_VIRTIO_SERIAL;

        if (chr->info.type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_VIRTIO_SERIAL &&
            chr->info.addr.vioserial.port == 0) {
            int maxport = 0;
            int j;
            for (j = 0 ; j < i ; j++) {
                virDomainChrDefPtr thischr = def->channels[j];
                if (thischr->info.type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_VIRTIO_SERIAL &&
                    thischr->info.addr.vioserial.controller == chr->info.addr.vioserial.controller &&
                    thischr->info.addr.vioserial.bus == chr->info.addr.vioserial.bus &&
                    (int)thischr->info.addr.vioserial.port > maxport)
                    maxport = thischr->info.addr.vioserial.port;
            }
            chr->info.addr.vioserial.port = maxport + 1;
        }
    }
    VIR_FREE(nodes);


    /* analysis of the input devices */
    if ((n = virXPathNodeSet("./devices/input", ctxt, &nodes)) < 0) {
        goto error;
    }
    if (n && VIR_ALLOC_N(def->inputs, n) < 0)
        goto no_memory;

    for (i = 0 ; i < n ; i++) {
        virDomainInputDefPtr input = virDomainInputDefParseXML(def->os.type,
                                                               nodes[i],
                                                               flags);
        if (!input)
            goto error;


        /* With QEMU / KVM / Xen graphics, mouse + PS/2 is implicit
         * with graphics, so don't store it.
         * XXX will this be true for other virt types ? */
        if ((STREQ(def->os.type, "hvm") &&
             input->bus == VIR_DOMAIN_INPUT_BUS_PS2 &&
             input->type == VIR_DOMAIN_INPUT_TYPE_MOUSE) ||
            (STRNEQ(def->os.type, "hvm") &&
             input->bus == VIR_DOMAIN_INPUT_BUS_XEN &&
             input->type == VIR_DOMAIN_INPUT_TYPE_MOUSE)) {
            virDomainInputDefFree(input);
            continue;
        }

        def->inputs[def->ninputs++] = input;
    }
    VIR_FREE(nodes);

    /* analysis of the graphics devices */
    if ((n = virXPathNodeSet("./devices/graphics", ctxt, &nodes)) < 0) {
        goto error;
    }
    if (n && VIR_ALLOC_N(def->graphics, n) < 0)
        goto no_memory;
    for (i = 0 ; i < n ; i++) {
        virDomainGraphicsDefPtr graphics = virDomainGraphicsDefParseXML(nodes[i],
                                                                        ctxt,
                                                                        flags);
        if (!graphics)
            goto error;

        def->graphics[def->ngraphics++] = graphics;
    }
    VIR_FREE(nodes);

    /* If graphics are enabled, there's an implicit PS2 mouse */
    if (def->ngraphics > 0) {
        virDomainInputDefPtr input;

        if (VIR_ALLOC(input) < 0) {
            goto no_memory;
        }
        if (STREQ(def->os.type, "hvm")) {
            input->type = VIR_DOMAIN_INPUT_TYPE_MOUSE;
            input->bus = VIR_DOMAIN_INPUT_BUS_PS2;
        } else {
            input->type = VIR_DOMAIN_INPUT_TYPE_MOUSE;
            input->bus = VIR_DOMAIN_INPUT_BUS_XEN;
        }

        if (VIR_REALLOC_N(def->inputs, def->ninputs + 1) < 0) {
            virDomainInputDefFree(input);
            goto no_memory;
        }
        def->inputs[def->ninputs] = input;
        def->ninputs++;
    }


    /* analysis of the sound devices */
    if ((n = virXPathNodeSet("./devices/sound", ctxt, &nodes)) < 0) {
        goto error;
    }
    if (n && VIR_ALLOC_N(def->sounds, n) < 0)
        goto no_memory;
    for (i = 0 ; i < n ; i++) {
        virDomainSoundDefPtr sound = virDomainSoundDefParseXML(nodes[i],
                                                               flags);
        if (!sound)
            goto error;

        def->sounds[def->nsounds++] = sound;
    }
    VIR_FREE(nodes);

    /* analysis of the video devices */
    if ((n = virXPathNodeSet("./devices/video", ctxt, &nodes)) < 0) {
        goto error;
    }
    if (n && VIR_ALLOC_N(def->videos, n) < 0)
        goto no_memory;
    for (i = 0 ; i < n ; i++) {
        virDomainVideoDefPtr video = virDomainVideoDefParseXML(nodes[i],
                                                               def,
                                                               flags);
        if (!video)
            goto error;
        def->videos[def->nvideos++] = video;
    }
    VIR_FREE(nodes);

    /* For backwards compatability, if no <video> tag is set but there
     * is a <graphics> tag, then we add a single video tag */
    if (def->ngraphics && !def->nvideos) {
        virDomainVideoDefPtr video;
        if (VIR_ALLOC(video) < 0)
            goto no_memory;
        video->type = virDomainVideoDefaultType(def);
        if (video->type < 0) {
            virDomainReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                                 _("cannot determine default video type"));
            VIR_FREE(video);
            goto error;
        }
        video->vram = virDomainVideoDefaultRAM(def, video->type);
        video->heads = 1;
        if (VIR_ALLOC_N(def->videos, 1) < 0) {
            virDomainVideoDefFree(video);
            goto no_memory;
        }
        def->videos[def->nvideos++] = video;
    }

    /* analysis of the host devices */
    if ((n = virXPathNodeSet("./devices/hostdev", ctxt, &nodes)) < 0) {
        goto error;
    }
    if (n && VIR_ALLOC_N(def->hostdevs, n) < 0)
        goto no_memory;
    for (i = 0 ; i < n ; i++) {
        virDomainHostdevDefPtr hostdev = virDomainHostdevDefParseXML(nodes[i],
                                                                     bootMap,
                                                                     flags);
        if (!hostdev)
            goto error;

        def->hostdevs[def->nhostdevs++] = hostdev;
    }
    VIR_FREE(nodes);

    /* analysis of the watchdog devices */
    def->watchdog = NULL;
    if ((n = virXPathNodeSet("./devices/watchdog", ctxt, &nodes)) < 0) {
        goto error;
    }
    if (n > 1) {
        virDomainReportError (VIR_ERR_INTERNAL_ERROR,
                              "%s", _("only a single watchdog device is supported"));
        goto error;
    }
    if (n > 0) {
        virDomainWatchdogDefPtr watchdog =
            virDomainWatchdogDefParseXML(nodes[0], flags);
        if (!watchdog)
            goto error;

        def->watchdog = watchdog;
        VIR_FREE(nodes);
    }

    /* analysis of the memballoon devices */
    def->memballoon = NULL;
    if ((n = virXPathNodeSet("./devices/memballoon", ctxt, &nodes)) < 0) {
        goto error;
    }
    if (n > 1) {
        virDomainReportError (VIR_ERR_INTERNAL_ERROR,
                              "%s", _("only a single memory balloon device is supported"));
        goto error;
    }
    if (n > 0) {
        virDomainMemballoonDefPtr memballoon =
            virDomainMemballoonDefParseXML(nodes[0], flags);
        if (!memballoon)
            goto error;

        def->memballoon = memballoon;
        VIR_FREE(nodes);
    } else {
        if (def->virtType == VIR_DOMAIN_VIRT_XEN ||
            def->virtType == VIR_DOMAIN_VIRT_QEMU ||
            def->virtType == VIR_DOMAIN_VIRT_KQEMU ||
            def->virtType == VIR_DOMAIN_VIRT_KVM) {
            virDomainMemballoonDefPtr memballoon;
            if (VIR_ALLOC(memballoon) < 0)
                goto no_memory;
            memballoon->model = def->virtType == VIR_DOMAIN_VIRT_XEN ?
                VIR_DOMAIN_MEMBALLOON_MODEL_XEN :
                VIR_DOMAIN_MEMBALLOON_MODEL_VIRTIO;
            def->memballoon = memballoon;
        }
    }

    /* analysis of security label */
    if (virSecurityLabelDefParseXML(def, ctxt, flags) == -1)
        goto error;

    if ((node = virXPathNode("./cpu[1]", ctxt)) != NULL) {
        xmlNodePtr oldnode = ctxt->node;
        ctxt->node = node;
        def->cpu = virCPUDefParseXML(node, ctxt, VIR_CPU_TYPE_GUEST);
        ctxt->node = oldnode;

        if (def->cpu == NULL)
            goto error;
    }

    if ((node = virXPathNode("./sysinfo[1]", ctxt)) != NULL) {
        xmlNodePtr oldnode = ctxt->node;
        ctxt->node = node;
        def->sysinfo = virSysinfoParseXML(node, ctxt);
        ctxt->node = oldnode;

        if (def->sysinfo == NULL)
            goto error;
        if (def->sysinfo->system_uuid != NULL) {
            unsigned char uuidbuf[VIR_UUID_BUFLEN];
            if (virUUIDParse(def->sysinfo->system_uuid, uuidbuf) < 0) {
                virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                     "%s", _("malformed uuid element"));
                goto error;
            }
            if (uuid_generated)
                memcpy(def->uuid, uuidbuf, VIR_UUID_BUFLEN);
            else if (memcmp(def->uuid, uuidbuf, VIR_UUID_BUFLEN) != 0) {
                virDomainReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                                     _("UUID mismatch between <uuid> and "
                                       "<sysinfo>"));
                goto error;
            }
        }
    }
    tmp = virXPathString("string(./os/smbios/@mode)", ctxt);
    if (tmp) {
        int mode;

        if ((mode = virDomainSmbiosModeTypeFromString(tmp)) < 0) {
            virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                 _("unknown smbios mode '%s'"), tmp);
            goto error;
        }
        def->os.smbios_mode = mode;
        VIR_FREE(tmp);
    } else {
        def->os.smbios_mode = VIR_DOMAIN_SMBIOS_NONE; /* not present */
    }

    /* we have to make a copy of all of the callback pointers here since
     * we won't have the virCaps structure available during free
     */
    def->ns = caps->ns;

    if (def->ns.parse) {
        if ((def->ns.parse)(xml, root, ctxt, &def->namespaceData) < 0)
            goto error;
    }

    /* Auto-add any implied controllers which aren't present
     */
    if (virDomainDefAddImplicitControllers(def) < 0)
        goto error;

    virBitmapFree(bootMap);

    return def;

no_memory:
    virReportOOMError();
    /* fallthrough */

 error:
    VIR_FREE(tmp);
    VIR_FREE(nodes);
    virBitmapFree(bootMap);
    virDomainDefFree(def);
    return NULL;
}


static virDomainObjPtr virDomainObjParseXML(virCapsPtr caps,
                                            xmlDocPtr xml,
                                            xmlXPathContextPtr ctxt,
                                            unsigned int expectedVirtTypes,
                                            unsigned int flags)
{
    char *tmp = NULL;
    long val;
    xmlNodePtr config;
    xmlNodePtr oldnode;
    virDomainObjPtr obj;
    xmlNodePtr *nodes = NULL;
    int i, n;
    int state;
    int reason = 0;

    if (!(obj = virDomainObjNew(caps)))
        return NULL;

    if (!(config = virXPathNode("./domain", ctxt))) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                             "%s", _("no domain config"));
        goto error;
    }

    oldnode = ctxt->node;
    ctxt->node = config;
    obj->def = virDomainDefParseXML(caps, xml, config, ctxt, expectedVirtTypes,
                                    flags);
    ctxt->node = oldnode;
    if (!obj->def)
        goto error;

    if (!(tmp = virXPathString("string(./@state)", ctxt))) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                             "%s", _("missing domain state"));
        goto error;
    }
    if ((state = virDomainStateTypeFromString(tmp)) < 0) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                             _("invalid domain state '%s'"), tmp);
        VIR_FREE(tmp);
        goto error;
    }
    VIR_FREE(tmp);

    if ((tmp = virXPathString("string(./@reason)", ctxt))) {
        if ((reason = virDomainStateReasonFromString(state, tmp)) < 0) {
            virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                 _("invalid domain state reason '%s'"), tmp);
            VIR_FREE(tmp);
            goto error;
        }
        VIR_FREE(tmp);
    }

    virDomainObjSetState(obj, state, reason);

    if ((virXPathLong("string(./@pid)", ctxt, &val)) < 0) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                             "%s", _("invalid pid"));
        goto error;
    }
    obj->pid = (pid_t)val;

    if ((n = virXPathNodeSet("./taint", ctxt, &nodes)) < 0) {
        goto error;
    }
    for (i = 0 ; i < n ; i++) {
        char *str = virXMLPropString(nodes[i], "flag");
        if (str) {
            int flag = virDomainTaintTypeFromString(str);
            if (flag < 0) {
                virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                     _("Unknown taint flag %s"), str);
                VIR_FREE(str);
                goto error;
            }
            VIR_FREE(str);
            virDomainObjTaint(obj, flag);
        }
    }
    VIR_FREE(nodes);

    if (caps->privateDataXMLParse &&
        ((caps->privateDataXMLParse)(ctxt, obj->privateData)) < 0)
        goto error;

    return obj;

error:
    /* obj was never shared, so unref should return 0 */
    ignore_value(virDomainObjUnref(obj));
    VIR_FREE(nodes);
    return NULL;
}


static virDomainDefPtr
virDomainDefParse(const char *xmlStr,
                  const char *filename,
                  virCapsPtr caps,
                  unsigned int expectedVirtTypes,
                  unsigned int flags)
{
    xmlDocPtr xml;
    virDomainDefPtr def = NULL;

    if ((xml = virXMLParse(filename, xmlStr, "domain.xml"))) {
        def = virDomainDefParseNode(caps, xml, xmlDocGetRootElement(xml),
                                    expectedVirtTypes, flags);
        xmlFreeDoc(xml);
    }

    return def;
}

virDomainDefPtr virDomainDefParseString(virCapsPtr caps,
                                        const char *xmlStr,
                                        unsigned int expectedVirtTypes,
                                        unsigned int flags)
{
    return virDomainDefParse(xmlStr, NULL, caps, expectedVirtTypes, flags);
}

virDomainDefPtr virDomainDefParseFile(virCapsPtr caps,
                                      const char *filename,
                                      unsigned int expectedVirtTypes,
                                      unsigned int flags)
{
    return virDomainDefParse(NULL, filename, caps, expectedVirtTypes, flags);
}


virDomainDefPtr virDomainDefParseNode(virCapsPtr caps,
                                      xmlDocPtr xml,
                                      xmlNodePtr root,
                                      unsigned int expectedVirtTypes,
                                      unsigned int flags)
{
    xmlXPathContextPtr ctxt = NULL;
    virDomainDefPtr def = NULL;

    if (!xmlStrEqual(root->name, BAD_CAST "domain")) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                              "%s", _("incorrect root element"));
        goto cleanup;
    }

    ctxt = xmlXPathNewContext(xml);
    if (ctxt == NULL) {
        virReportOOMError();
        goto cleanup;
    }

    ctxt->node = root;
    def = virDomainDefParseXML(caps, xml, root, ctxt, expectedVirtTypes, flags);

cleanup:
    xmlXPathFreeContext(ctxt);
    return def;
}


static virDomainObjPtr
virDomainObjParseNode(virCapsPtr caps,
                      xmlDocPtr xml,
                      xmlNodePtr root,
                      unsigned int expectedVirtTypes,
                      unsigned int flags)
{
    xmlXPathContextPtr ctxt = NULL;
    virDomainObjPtr obj = NULL;

    if (!xmlStrEqual(root->name, BAD_CAST "domstatus")) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                             "%s", _("incorrect root element"));
        goto cleanup;
    }

    ctxt = xmlXPathNewContext(xml);
    if (ctxt == NULL) {
        virReportOOMError();
        goto cleanup;
    }

    ctxt->node = root;
    obj = virDomainObjParseXML(caps, xml, ctxt, expectedVirtTypes, flags);

cleanup:
    xmlXPathFreeContext(ctxt);
    return obj;
}


static virDomainObjPtr
virDomainObjParseFile(virCapsPtr caps,
                      const char *filename,
                      unsigned int expectedVirtTypes,
                      unsigned int flags)
{
    xmlDocPtr xml;
    virDomainObjPtr obj = NULL;

    if ((xml = virXMLParseFile(filename))) {
        obj = virDomainObjParseNode(caps, xml,
                                    xmlDocGetRootElement(xml),
                                    expectedVirtTypes, flags);
        xmlFreeDoc(xml);
    }

    return obj;
}


static bool virDomainTimerDefCheckABIStability(virDomainTimerDefPtr src,
                                              virDomainTimerDefPtr dst)
{
    bool identical = false;

    if (src->name != dst->name) {
        virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                             _("Target timer %s does not match source %s"),
                             virDomainTimerNameTypeToString(dst->name),
                             virDomainTimerNameTypeToString(src->name));
        goto cleanup;
    }

    if (src->present != dst->present) {
        virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                             _("Target timer presence %d does not match source %d"),
                             dst->present, src->present);
        goto cleanup;
    }

    if (src->name == VIR_DOMAIN_TIMER_NAME_TSC) {
        if (src->frequency != dst->frequency) {
            virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                                 _("Target TSC frequency %lu does not match source %lu"),
                                 dst->frequency, src->frequency);
            goto cleanup;
        }

        if (src->mode != dst->mode) {
            virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                                 _("Target TSC mode %s does not match source %s"),
                                 virDomainTimerModeTypeToString(dst->mode),
                                 virDomainTimerModeTypeToString(src->mode));
            goto cleanup;
        }
    }

    identical = true;

cleanup:
    return identical;
}


static bool virDomainDeviceInfoCheckABIStability(virDomainDeviceInfoPtr src,
                                                 virDomainDeviceInfoPtr dst)
{
    bool identical = false;

    if (src->type != dst->type) {
        virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                             _("Target device address type %s does not match source %s"),
                             virDomainDeviceAddressTypeToString(dst->type),
                             virDomainDeviceAddressTypeToString(src->type));
        goto cleanup;
    }

    switch (src->type) {
    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI:
        if (src->addr.pci.domain != dst->addr.pci.domain ||
            src->addr.pci.bus != dst->addr.pci.bus ||
            src->addr.pci.slot != dst->addr.pci.slot ||
            src->addr.pci.function != dst->addr.pci.function) {
            virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                                 _("Target device PCI address %04x:%02x:%02x.%02x does not match source %04x:%02x:%02x.%02x"),
                                 dst->addr.pci.domain, dst->addr.pci.bus,
                                 dst->addr.pci.slot, dst->addr.pci.function,
                                 src->addr.pci.domain, src->addr.pci.bus,
                                 src->addr.pci.slot, src->addr.pci.function);
            goto cleanup;
        }
        break;

    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_DRIVE:
        if (src->addr.drive.controller != dst->addr.drive.controller ||
            src->addr.drive.bus != dst->addr.drive.bus ||
            src->addr.drive.unit != dst->addr.drive.unit) {
            virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                                 _("Target device drive address %d:%d:%d does not match source %d:%d:%d"),
                                 dst->addr.drive.controller, dst->addr.drive.bus,
                                 dst->addr.drive.unit,
                                 src->addr.drive.controller, src->addr.drive.bus,
                                 src->addr.drive.unit);
            goto cleanup;
        }
        break;

    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_VIRTIO_SERIAL:
        if (src->addr.vioserial.controller != dst->addr.vioserial.controller ||
            src->addr.vioserial.bus != dst->addr.vioserial.bus ||
            src->addr.vioserial.port != dst->addr.vioserial.port) {
            virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                                 _("Target device virtio serial address %d:%d:%d does not match source %d:%d:%d"),
                                 dst->addr.vioserial.controller, dst->addr.vioserial.bus,
                                 dst->addr.vioserial.port,
                                 src->addr.vioserial.controller, src->addr.vioserial.bus,
                                 src->addr.vioserial.port);
            goto cleanup;
        }
        break;

    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_CCID:
        if (src->addr.ccid.controller != dst->addr.ccid.controller ||
            src->addr.ccid.slot != dst->addr.ccid.slot) {
            virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                                 _("Target device ccid address %d:%d does not match source %d:%d"),
                                 dst->addr.ccid.controller,
                                 dst->addr.ccid.slot,
                                 src->addr.ccid.controller,
                                 src->addr.ccid.slot);
            goto cleanup;
        }
        break;
    }

    identical = true;

cleanup:
    return identical;
}


static bool virDomainDiskDefCheckABIStability(virDomainDiskDefPtr src,
                                              virDomainDiskDefPtr dst)
{
    bool identical = false;

    if (src->device != dst->device) {
        virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                             _("Target disk device %s does not match source %s"),
                             virDomainDiskDeviceTypeToString(dst->device),
                             virDomainDiskDeviceTypeToString(src->device));
        goto cleanup;
    }

    if (src->bus != dst->bus) {
        virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                             _("Target disk bus %s does not match source %s"),
                             virDomainDiskBusTypeToString(dst->bus),
                             virDomainDiskBusTypeToString(src->bus));
        goto cleanup;
    }

    if (STRNEQ(src->dst, dst->dst)) {
        virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                             _("Target disk %s does not match source %s"),
                             dst->dst, src->dst);
        goto cleanup;
    }

    if (STRNEQ_NULLABLE(src->serial, dst->serial)) {
        virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                             _("Target disk serial %s does not match source %s"),
                             NULLSTR(dst->serial), NULLSTR(src->serial));
        goto cleanup;
    }

    if (src->readonly != dst->readonly || src->shared != dst->shared) {
        virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                             _("Target disk access mode does not match source"));
        goto cleanup;
    }

    if (!virDomainDeviceInfoCheckABIStability(&src->info, &dst->info))
        goto cleanup;

    identical = true;

cleanup:
    return identical;
}


static bool virDomainControllerDefCheckABIStability(virDomainControllerDefPtr src,
                                                    virDomainControllerDefPtr dst)
{
    bool identical = false;

    if (src->type != dst->type) {
        virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                             _("Target controller type %s does not match source %s"),
                             virDomainControllerTypeToString(dst->type),
                             virDomainControllerTypeToString(src->type));
        goto cleanup;
    }

    if (src->idx != dst->idx) {
        virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                             _("Target controller index %d does not match source %d"),
                             dst->idx, src->idx);
        goto cleanup;
    }

    if (src->model != dst->model) {
        virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                             _("Target controller model %d does not match source %d"),
                             dst->model, src->model);
        goto cleanup;
    }

    if (src->type == VIR_DOMAIN_CONTROLLER_TYPE_VIRTIO_SERIAL) {
        if (src->opts.vioserial.ports != dst->opts.vioserial.ports) {
            virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                                 _("Target controller ports %d does not match source %d"),
                                 dst->opts.vioserial.ports, src->opts.vioserial.ports);
            goto cleanup;
        }

        if (src->opts.vioserial.vectors != dst->opts.vioserial.vectors) {
            virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                                 _("Target controller vectors %d does not match source %d"),
                                 dst->opts.vioserial.vectors, src->opts.vioserial.vectors);
            goto cleanup;
        }
    }

    if (!virDomainDeviceInfoCheckABIStability(&src->info, &dst->info))
        goto cleanup;

    identical = true;

cleanup:
    return identical;
}


static bool virDomainFsDefCheckABIStability(virDomainFSDefPtr src,
                                            virDomainFSDefPtr dst)
{
    bool identical = false;

    if (STRNEQ(src->dst, dst->dst)) {
        virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                             _("Target filesystem guest target %s does not match source %s"),
                             dst->dst, src->dst);
        goto cleanup;
    }

    if (src->readonly != dst->readonly) {
        virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                             _("Target filesystem access mode does not match source"));
        goto cleanup;
    }

    if (!virDomainDeviceInfoCheckABIStability(&src->info, &dst->info))
        goto cleanup;

    identical = true;

cleanup:
    return identical;
}


static bool virDomainNetDefCheckABIStability(virDomainNetDefPtr src,
                                             virDomainNetDefPtr dst)
{
    bool identical = false;

    if (memcmp(src->mac, dst->mac, VIR_MAC_BUFLEN) != 0) {
        virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                             _("Target network card mac %02x:%02x:%02x:%02x:%02x:%02x"
                               "does not match source %02x:%02x:%02x:%02x:%02x:%02x"),
                             dst->mac[0], dst->mac[1], dst->mac[2],
                             dst->mac[3], dst->mac[4], dst->mac[5],
                             src->mac[0], src->mac[1], src->mac[2],
                             src->mac[3], src->mac[4], src->mac[5]);
        goto cleanup;
    }

    if (STRNEQ_NULLABLE(src->model, dst->model)) {
        virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                             _("Target network card model %s does not match source %s"),
                             NULLSTR(dst->model), NULLSTR(src->model));
        goto cleanup;
    }

    if (!virDomainDeviceInfoCheckABIStability(&src->info, &dst->info))
        goto cleanup;

    identical = true;

cleanup:
    return identical;
}


static bool virDomainInputDefCheckABIStability(virDomainInputDefPtr src,
                                               virDomainInputDefPtr dst)
{
    bool identical = false;

    if (src->type != dst->type) {
        virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                             _("Target input device type %s does not match source %s"),
                             virDomainInputTypeToString(dst->type),
                             virDomainInputTypeToString(src->type));
        goto cleanup;
    }

    if (src->bus != dst->bus) {
        virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                             _("Target input device bus %s does not match source %s"),
                             virDomainInputBusTypeToString(dst->bus),
                             virDomainInputBusTypeToString(src->bus));
        goto cleanup;
    }

    if (!virDomainDeviceInfoCheckABIStability(&src->info, &dst->info))
        goto cleanup;

    identical = true;

cleanup:
    return identical;
}


static bool virDomainSoundDefCheckABIStability(virDomainSoundDefPtr src,
                                               virDomainSoundDefPtr dst)
{
    bool identical = false;

    if (src->model != dst->model) {
        virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                             _("Target sound card model %s does not match source %s"),
                             virDomainSoundModelTypeToString(dst->model),
                             virDomainSoundModelTypeToString(src->model));
        goto cleanup;
    }

    if (!virDomainDeviceInfoCheckABIStability(&src->info, &dst->info))
        goto cleanup;

    identical = true;

cleanup:
    return identical;
}


static bool virDomainVideoDefCheckABIStability(virDomainVideoDefPtr src,
                                               virDomainVideoDefPtr dst)
{
    bool identical = false;

    if (src->type != dst->type) {
        virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                             _("Target video card model %s does not match source %s"),
                             virDomainVideoTypeToString(dst->type),
                             virDomainVideoTypeToString(src->type));
        goto cleanup;
    }

    if (src->vram != dst->vram) {
        virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                             _("Target video card vram %u does not match source %u"),
                             dst->vram, src->vram);
        goto cleanup;
    }

    if (src->heads != dst->heads) {
        virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                             _("Target video card heads %u does not match source %u"),
                             dst->heads, src->heads);
        goto cleanup;
    }

    if ((src->accel && !dst->accel) ||
        (!src->accel && dst->accel)) {
        virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                             _("Target video card acceleration does not match source"));
        goto cleanup;
    }

    if (src->accel) {
        if (src->accel->support2d != dst->accel->support2d) {
            virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                                 _("Target video card 2d accel %u does not match source %u"),
                                 dst->accel->support2d, src->accel->support2d);
            goto cleanup;
        }

        if (src->accel->support3d != dst->accel->support3d) {
            virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                                 _("Target video card 3d accel %u does not match source %u"),
                                 dst->accel->support3d, src->accel->support3d);
            goto cleanup;
        }
    }

    if (!virDomainDeviceInfoCheckABIStability(&src->info, &dst->info))
        goto cleanup;

    identical = true;

cleanup:
    return identical;
}


static bool virDomainHostdevDefCheckABIStability(virDomainHostdevDefPtr src,
                                                 virDomainHostdevDefPtr dst)
{
    bool identical = false;

    if (src->mode != dst->mode) {
        virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                             _("Target host device mode %s does not match source %s"),
                             virDomainHostdevModeTypeToString(dst->mode),
                             virDomainHostdevModeTypeToString(src->mode));
        goto cleanup;
    }

    if (src->mode == VIR_DOMAIN_HOSTDEV_MODE_SUBSYS) {
        if (src->source.subsys.type != dst->source.subsys.type) {
            virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                                 _("Target host device subsystem %s does not match source %s"),
                                 virDomainHostdevSubsysTypeToString(dst->source.subsys.type),
                                 virDomainHostdevSubsysTypeToString(src->source.subsys.type));
            goto cleanup;
        }
    }

    if (!virDomainDeviceInfoCheckABIStability(&src->info, &dst->info))
        goto cleanup;

    identical = true;

cleanup:
    return identical;
}


static bool virDomainSmartcardDefCheckABIStability(virDomainSmartcardDefPtr src,
                                                   virDomainSmartcardDefPtr dst)
{
    bool identical = false;

    if (!virDomainDeviceInfoCheckABIStability(&src->info, &dst->info))
        goto cleanup;

    identical = true;

cleanup:
    return identical;
}


static bool virDomainSerialDefCheckABIStability(virDomainChrDefPtr src,
                                                virDomainChrDefPtr dst)
{
    bool identical = false;

    if (src->target.port != dst->target.port) {
        virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                             _("Target serial port %d does not match source %d"),
                             dst->target.port, src->target.port);
        goto cleanup;
    }

    if (!virDomainDeviceInfoCheckABIStability(&src->info, &dst->info))
        goto cleanup;

    identical = true;

cleanup:
    return identical;
}


static bool virDomainParallelDefCheckABIStability(virDomainChrDefPtr src,
                                                  virDomainChrDefPtr dst)
{
    bool identical = false;

    if (src->target.port != dst->target.port) {
        virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                             _("Target serial port %d does not match source %d"),
                             dst->target.port, src->target.port);
        goto cleanup;
    }

    if (!virDomainDeviceInfoCheckABIStability(&src->info, &dst->info))
        goto cleanup;

    identical = true;

cleanup:
    return identical;
}


static bool virDomainChannelDefCheckABIStability(virDomainChrDefPtr src,
                                                 virDomainChrDefPtr dst)
{
    bool identical = false;

    if (src->targetType != dst->targetType) {
        virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                             _("Target channel type %s does not match source %s"),
                             virDomainChrChannelTargetTypeToString(dst->targetType),
                             virDomainChrChannelTargetTypeToString(src->targetType));
        goto cleanup;
    }

    switch (src->targetType) {
    case VIR_DOMAIN_CHR_CHANNEL_TARGET_TYPE_VIRTIO:
        if (STRNEQ(src->target.name, dst->target.name)) {
            virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                                 _("Target channel name %s does not match source %s"),
                                 dst->target.name, src->target.name);
            goto cleanup;
        }
        break;
    case VIR_DOMAIN_CHR_CHANNEL_TARGET_TYPE_GUESTFWD:
        if (memcmp(src->target.addr, dst->target.addr,
                   sizeof(*src->target.addr)) != 0) {
            char *saddr = virSocketFormatAddrFull(src->target.addr, true, ":");
            char *daddr = virSocketFormatAddrFull(dst->target.addr, true, ":");
            virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                                 _("Target channel addr %s does not match source %s"),
                                 NULLSTR(daddr), NULLSTR(saddr));
            VIR_FREE(saddr);
            VIR_FREE(daddr);
            goto cleanup;
        }
        break;
    }

    if (!virDomainDeviceInfoCheckABIStability(&src->info, &dst->info))
        goto cleanup;

    identical = true;

cleanup:
    return identical;
}


static bool virDomainConsoleDefCheckABIStability(virDomainChrDefPtr src,
                                                 virDomainChrDefPtr dst)
{
    bool identical = false;

    if (src->targetType != dst->targetType) {
        virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                             _("Target console type %s does not match source %s"),
                             virDomainChrConsoleTargetTypeToString(dst->targetType),
                             virDomainChrConsoleTargetTypeToString(src->targetType));
        goto cleanup;
    }

    if (!virDomainDeviceInfoCheckABIStability(&src->info, &dst->info))
        goto cleanup;

    identical = true;

cleanup:
    return identical;
}


static bool virDomainWatchdogDefCheckABIStability(virDomainWatchdogDefPtr src,
                                                  virDomainWatchdogDefPtr dst)
{
    bool identical = false;

    if (src->model != dst->model) {
        virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                             _("Target watchdog model %s does not match source %s"),
                             virDomainWatchdogModelTypeToString(dst->model),
                             virDomainWatchdogModelTypeToString(src->model));
        goto cleanup;
    }

    if (!virDomainDeviceInfoCheckABIStability(&src->info, &dst->info))
        goto cleanup;

    identical = true;

cleanup:
    return identical;
}


static bool virDomainMemballoonDefCheckABIStability(virDomainMemballoonDefPtr src,
                                                    virDomainMemballoonDefPtr dst)
{
    bool identical = false;

    if (src->model != dst->model) {
        virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                             _("Target balloon model %s does not match source %s"),
                             virDomainMemballoonModelTypeToString(dst->model),
                             virDomainMemballoonModelTypeToString(src->model));
        goto cleanup;
    }

    if (!virDomainDeviceInfoCheckABIStability(&src->info, &dst->info))
        goto cleanup;

    identical = true;

cleanup:
    return identical;
}


/* This compares two configurations and looks for any differences
 * which will affect the guest ABI. This is primarily to allow
 * validation of custom XML config passed in during migration
 */
bool virDomainDefCheckABIStability(virDomainDefPtr src,
                                   virDomainDefPtr dst)
{
    bool identical = false;
    int i;

    if (src->virtType != dst->virtType) {
        virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                             _("Target domain virt type %s does not match source %s"),
                             virDomainVirtTypeToString(dst->virtType),
                             virDomainVirtTypeToString(src->virtType));
        goto cleanup;
    }

    if (memcmp(src->uuid, dst->uuid, VIR_UUID_BUFLEN) != 0) {
        char uuidsrc[VIR_UUID_STRING_BUFLEN];
        char uuiddst[VIR_UUID_STRING_BUFLEN];
        virUUIDFormat(src->uuid, uuidsrc);
        virUUIDFormat(dst->uuid, uuiddst);
        virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                             _("Target domain uuid %s does not match source %s"),
                             uuiddst, uuidsrc);
        goto cleanup;
    }

    if (src->mem.max_balloon != dst->mem.max_balloon) {
        virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                             _("Target domain max memory %ld does not match source %ld"),
                             dst->mem.max_balloon, src->mem.max_balloon);
        goto cleanup;
    }
    if (src->mem.cur_balloon != dst->mem.cur_balloon) {
        virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                             _("Target domain current memory %ld does not match source %ld"),
                             dst->mem.cur_balloon, src->mem.cur_balloon);
        goto cleanup;
    }
    if (src->mem.hugepage_backed != dst->mem.hugepage_backed) {
        virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                             _("Target domain huge page backing %ld does not match source %ld"),
                             dst->mem.hugepage_backed,
                             src->mem.hugepage_backed);
        goto cleanup;
    }

    if (src->vcpus != dst->vcpus) {
        virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                             _("Target domain vpu count %d does not match source %d"),
                             dst->vcpus, src->vcpus);
        goto cleanup;
    }
    if (src->maxvcpus != dst->maxvcpus) {
        virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                             _("Target domain vpu max %d does not match source %d"),
                             dst->maxvcpus, src->maxvcpus);
        goto cleanup;
    }

    if (STRNEQ(src->os.type, dst->os.type)) {
        virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                             _("Target domain OS type %s does not match source %s"),
                             dst->os.type, src->os.type);
        goto cleanup;
    }
    if (STRNEQ(src->os.arch, dst->os.arch)) {
        virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                             _("Target domain architecture %s does not match source %s"),
                             dst->os.arch, src->os.arch);
        goto cleanup;
    }
    if (STRNEQ(src->os.machine, dst->os.machine)) {
        virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                             _("Target domain OS type %s does not match source %s"),
                             dst->os.machine, src->os.machine);
        goto cleanup;
    }

    if (src->os.smbios_mode != dst->os.smbios_mode) {
        virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                             _("Target domain SMBIOS mode %s does not match source %s"),
                             virDomainSmbiosModeTypeToString(dst->os.smbios_mode),
                             virDomainSmbiosModeTypeToString(src->os.smbios_mode));
        goto cleanup;
    }

    if (src->features != dst->features) {
        virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                             _("Target domain features %d does not match source %d"),
                             dst->features, src->features);
        goto cleanup;
    }

    if (src->clock.ntimers != dst->clock.ntimers) {
        virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                             _("Target domain timers do not match source"));
        goto cleanup;
    }

    for (i = 0 ; i < src->clock.ntimers ; i++) {
        if (!virDomainTimerDefCheckABIStability(src->clock.timers[i], dst->clock.timers[i]))
            goto cleanup;
    }

    if (!virCPUDefIsEqual(src->cpu, dst->cpu))
        goto cleanup;

    if (!virSysinfoIsEqual(src->sysinfo, dst->sysinfo))
        goto cleanup;

    if (src->ndisks != dst->ndisks) {
        virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                             _("Target domain disk count %d does not match source %d"),
                             dst->ndisks, src->ndisks);
        goto cleanup;
    }

    for (i = 0 ; i < src->ndisks ; i++)
        if (!virDomainDiskDefCheckABIStability(src->disks[i], dst->disks[i]))
            goto cleanup;

    if (src->ncontrollers != dst->ncontrollers) {
        virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                             _("Target domain disk controller count %d does not match source %d"),
                             dst->ncontrollers, src->ncontrollers);
        goto cleanup;
    }

    for (i = 0 ; i < src->ncontrollers ; i++)
        if (!virDomainControllerDefCheckABIStability(src->controllers[i], dst->controllers[i]))
            goto cleanup;

    if (src->nfss != dst->nfss) {
        virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                             _("Target domain filesystem count %d does not match source %d"),
                             dst->nfss, src->nfss);
        goto cleanup;
    }

    for (i = 0 ; i < src->nfss ; i++)
        if (!virDomainFsDefCheckABIStability(src->fss[i], dst->fss[i]))
            goto cleanup;

    if (src->nnets != dst->nnets) {
        virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                             _("Target domain net card count %d does not match source %d"),
                             dst->nnets, src->nnets);
        goto cleanup;
    }

    for (i = 0 ; i < src->nnets ; i++)
        if (!virDomainNetDefCheckABIStability(src->nets[i], dst->nets[i]))
            goto cleanup;

    if (src->ninputs != dst->ninputs) {
        virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                             _("Target domain input device count %d does not match source %d"),
                             dst->ninputs, src->ninputs);
        goto cleanup;
    }

    for (i = 0 ; i < src->ninputs ; i++)
        if (!virDomainInputDefCheckABIStability(src->inputs[i], dst->inputs[i]))
            goto cleanup;

    if (src->nsounds != dst->nsounds) {
        virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                             _("Target domain sound card count %d does not match source %d"),
                             dst->nsounds, src->nsounds);
        goto cleanup;
    }

    for (i = 0 ; i < src->nsounds ; i++)
        if (!virDomainSoundDefCheckABIStability(src->sounds[i], dst->sounds[i]))
            goto cleanup;

    if (src->nvideos != dst->nvideos) {
        virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                             _("Target domain video card count %d does not match source %d"),
                             dst->nvideos, src->nvideos);
        goto cleanup;
    }

    for (i = 0 ; i < src->nvideos ; i++)
        if (!virDomainVideoDefCheckABIStability(src->videos[i], dst->videos[i]))
            goto cleanup;

    if (src->nhostdevs != dst->nhostdevs) {
        virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                             _("Target domain host device count %d does not match source %d"),
                             dst->nhostdevs, src->nhostdevs);
        goto cleanup;
    }

    for (i = 0 ; i < src->nhostdevs ; i++)
        if (!virDomainHostdevDefCheckABIStability(src->hostdevs[i], dst->hostdevs[i]))
            goto cleanup;

    if (src->nsmartcards != dst->nsmartcards) {
        virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                             _("Target domain smartcard count %d does not match source %d"),
                             dst->nsmartcards, src->nsmartcards);
        goto cleanup;
    }

    for (i = 0 ; i < src->nsmartcards ; i++)
        if (!virDomainSmartcardDefCheckABIStability(src->smartcards[i], dst->smartcards[i]))
            goto cleanup;

    if (src->nserials != dst->nserials) {
        virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                             _("Target domain serial port count %d does not match source %d"),
                             dst->nserials, src->nserials);
        goto cleanup;
    }

    for (i = 0 ; i < src->nserials ; i++)
        if (!virDomainSerialDefCheckABIStability(src->serials[i], dst->serials[i]))
            goto cleanup;

    if (src->nparallels != dst->nparallels) {
        virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                             _("Target domain parallel port count %d does not match source %d"),
                             dst->nparallels, src->nparallels);
        goto cleanup;
    }

    for (i = 0 ; i < src->nparallels ; i++)
        if (!virDomainParallelDefCheckABIStability(src->parallels[i], dst->parallels[i]))
            goto cleanup;

    if (src->nchannels != dst->nchannels) {
        virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                             _("Target domain channel count %d does not match source %d"),
                             dst->nchannels, src->nchannels);
        goto cleanup;
    }

    for (i = 0 ; i < src->nchannels ; i++)
        if (!virDomainChannelDefCheckABIStability(src->channels[i], dst->channels[i]))
            goto cleanup;

    if ((!src->console && dst->console) ||
        (src->console && !dst->console)) {
        virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                             _("Target domain console count %d does not match source %d"),
                             dst->console ? 1 : 0, src->console ? 1 : 0);
        goto cleanup;
    }

    if (src->console &&
        !virDomainConsoleDefCheckABIStability(src->console, dst->console))
        goto cleanup;

    if ((!src->watchdog && dst->watchdog) ||
        (src->watchdog && !dst->watchdog)) {
        virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                             _("Target domain watchdog count %d does not match source %d"),
                             dst->watchdog ? 1 : 0, src->watchdog ? 1 : 0);
        goto cleanup;
    }

    if (src->watchdog &&
        !virDomainWatchdogDefCheckABIStability(src->watchdog, dst->watchdog))
        goto cleanup;

    if ((!src->memballoon && dst->memballoon) ||
        (src->memballoon && !dst->memballoon)) {
        virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                             _("Target domain memory balloon count %d does not match source %d"),
                             dst->memballoon ? 1 : 0, src->memballoon ? 1 : 0);
        goto cleanup;
    }

    if (src->memballoon &&
        !virDomainMemballoonDefCheckABIStability(src->memballoon, dst->memballoon))
        goto cleanup;

    identical = true;

cleanup:
    return identical;
}


static int virDomainDefMaybeAddController(virDomainDefPtr def,
                                          int type,
                                          int idx)
{
    int found = 0;
    int i;
    virDomainControllerDefPtr cont;

    for (i = 0 ; (i < def->ncontrollers) && !found; i++) {
        if (def->controllers[i]->type == type &&
            def->controllers[i]->idx == idx)
            found = 1;
    }

    if (found)
        return 0;

    if (VIR_ALLOC(cont) < 0) {
        virReportOOMError();
        return -1;
    }

    cont->type = type;
    cont->idx = idx;
    cont->model = -1;

    if (cont->type == VIR_DOMAIN_CONTROLLER_TYPE_VIRTIO_SERIAL) {
        cont->opts.vioserial.ports = -1;
        cont->opts.vioserial.vectors = -1;
    }


    if (VIR_REALLOC_N(def->controllers, def->ncontrollers+1) < 0) {
        VIR_FREE(cont);
        virReportOOMError();
        return -1;
    }
    def->controllers[def->ncontrollers] = cont;
    def->ncontrollers++;

    return 0;
}

static int virDomainDefAddDiskControllersForType(virDomainDefPtr def,
                                                 int controllerType,
                                                 int diskBus)
{
    int i;
    int maxController = -1;

    for (i = 0 ; i < def->ndisks ; i++) {
        if (def->disks[i]->bus != diskBus)
            continue;

        if (def->disks[i]->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_DRIVE)
            continue;

        if ((int)def->disks[i]->info.addr.drive.controller > maxController)
            maxController = def->disks[i]->info.addr.drive.controller;
    }

    for (i = 0 ; i <= maxController ; i++) {
        if (virDomainDefMaybeAddController(def, controllerType, i) < 0)
            return -1;
    }

    return 0;
}


static int virDomainDefMaybeAddVirtioSerialController(virDomainDefPtr def)
{
    /* Look for any virtio serial or virtio console devs */
    int i;

    for (i = 0 ; i < def->nchannels ; i++) {
        virDomainChrDefPtr channel = def->channels[i];

        if (channel->targetType == VIR_DOMAIN_CHR_CHANNEL_TARGET_TYPE_VIRTIO) {
            int idx = 0;
            if (channel->info.type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_VIRTIO_SERIAL)
                idx = channel->info.addr.vioserial.controller;

            if (virDomainDefMaybeAddController(def,
                VIR_DOMAIN_CONTROLLER_TYPE_VIRTIO_SERIAL, idx) < 0)
                return -1;
        }
    }

    if (def->console) {
        virDomainChrDefPtr console = def->console;

        if (console->targetType == VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_VIRTIO) {
            int idx = 0;
            if (console->info.type ==
                VIR_DOMAIN_DEVICE_ADDRESS_TYPE_VIRTIO_SERIAL)
                idx = console->info.addr.vioserial.controller;

            if (virDomainDefMaybeAddController(def,
                VIR_DOMAIN_CONTROLLER_TYPE_VIRTIO_SERIAL, idx) < 0)
                return -1;
        }
    }

    return 0;
}


static int
virDomainDefMaybeAddSmartcardController(virDomainDefPtr def)
{
    /* Look for any smartcard devs */
    int i;

    for (i = 0 ; i < def->nsmartcards ; i++) {
        virDomainSmartcardDefPtr smartcard = def->smartcards[i];
        int idx = 0;

        if (smartcard->info.type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_CCID) {
            idx = smartcard->info.addr.ccid.controller;
        } else if (smartcard->info.type
                   == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_NONE) {
            int j;
            int max = -1;

            for (j = 0; j < def->nsmartcards; j++) {
                virDomainDeviceInfoPtr info = &def->smartcards[j]->info;
                if (info->type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_CCID &&
                    info->addr.ccid.controller == 0 &&
                    (int) info->addr.ccid.slot > max)
                    max = info->addr.ccid.slot;
            }
            smartcard->info.type = VIR_DOMAIN_DEVICE_ADDRESS_TYPE_CCID;
            smartcard->info.addr.ccid.controller = 0;
            smartcard->info.addr.ccid.slot = max + 1;
        }

        if (virDomainDefMaybeAddController(def,
                                           VIR_DOMAIN_CONTROLLER_TYPE_CCID,
                                           idx) < 0)
            return -1;
    }

    return 0;
}


/*
 * Based on the declared <address/> info for any devices,
 * add neccessary drive controllers which are not already present
 * in the XML. This is for compat with existing apps which will
 * not know/care about <controller> info in the XML
 */
int virDomainDefAddImplicitControllers(virDomainDefPtr def)
{
    if (virDomainDefAddDiskControllersForType(def,
                                              VIR_DOMAIN_CONTROLLER_TYPE_SCSI,
                                              VIR_DOMAIN_DISK_BUS_SCSI) < 0)
        return -1;

    if (virDomainDefAddDiskControllersForType(def,
                                              VIR_DOMAIN_CONTROLLER_TYPE_FDC,
                                              VIR_DOMAIN_DISK_BUS_FDC) < 0)
        return -1;

    if (virDomainDefAddDiskControllersForType(def,
                                              VIR_DOMAIN_CONTROLLER_TYPE_IDE,
                                              VIR_DOMAIN_DISK_BUS_IDE) < 0)
        return -1;

    if (virDomainDefMaybeAddVirtioSerialController(def) < 0)
        return -1;

    if (virDomainDefMaybeAddSmartcardController(def) < 0)
        return -1;

    return 0;
}


/************************************************************************
 *                                                                        *
 * Parser and converter for the CPUset strings used in libvirt                *
 *                                                                        *
 ************************************************************************/
/**
 * virDomainCpuNumberParse
 * @str: pointer to the char pointer used
 * @maxcpu: maximum CPU number allowed
 *
 * Parse a CPU number
 *
 * Returns the CPU number or -1 in case of error. @str will be
 *         updated to skip the number.
 */
static int
virDomainCpuNumberParse(const char **str, int maxcpu)
{
    int ret = 0;
    const char *cur = *str;

    if (!c_isdigit(*cur))
        return (-1);

    while (c_isdigit(*cur)) {
        ret = ret * 10 + (*cur - '0');
        if (ret >= maxcpu)
            return (-1);
        cur++;
    }
    *str = cur;
    return (ret);
}

/**
 * virDomainCpuSetFormat:
 * @conn: connection
 * @cpuset: pointer to a char array for the CPU set
 * @maxcpu: number of elements available in @cpuset
 *
 * Serialize the cpuset to a string
 *
 * Returns the new string NULL in case of error. The string needs to be
 *         freed by the caller.
 */
char *
virDomainCpuSetFormat(char *cpuset, int maxcpu)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    int start, cur;
    int first = 1;

    if ((cpuset == NULL) || (maxcpu <= 0) || (maxcpu > 100000))
        return (NULL);

    cur = 0;
    start = -1;
    while (cur < maxcpu) {
        if (cpuset[cur]) {
            if (start == -1)
                start = cur;
        } else if (start != -1) {
            if (!first)
                virBufferAddLit(&buf, ",");
            else
                first = 0;
            if (cur == start + 1)
                virBufferAsprintf(&buf, "%d", start);
            else
                virBufferAsprintf(&buf, "%d-%d", start, cur - 1);
            start = -1;
        }
        cur++;
    }
    if (start != -1) {
        if (!first)
            virBufferAddLit(&buf, ",");
        if (maxcpu == start + 1)
            virBufferAsprintf(&buf, "%d", start);
        else
            virBufferAsprintf(&buf, "%d-%d", start, maxcpu - 1);
    }

    if (virBufferError(&buf)) {
        virBufferFreeAndReset(&buf);
        virReportOOMError();
        return NULL;
    }

    return virBufferContentAndReset(&buf);
}

/**
 * virDomainCpuSetParse:
 * @conn: connection
 * @str: pointer to a CPU set string pointer
 * @sep: potential character used to mark the end of string if not 0
 * @cpuset: pointer to a char array for the CPU set
 * @maxcpu: number of elements available in @cpuset
 *
 * Parse the cpu set, it will set the value for enabled CPUs in the @cpuset
 * to 1, and 0 otherwise. The syntax allows comma separated entries; each
 * can be either a CPU number, ^N to unset that CPU, or N-M for ranges.
 *
 * Returns the number of CPU found in that set, or -1 in case of error.
 *         @cpuset is modified accordingly to the value parsed.
 *         @str is updated to the end of the part parsed
 */
int
virDomainCpuSetParse(const char **str, char sep,
                     char *cpuset, int maxcpu)
{
    const char *cur;
    int ret = 0;
    int i, start, last;
    int neg = 0;

    if ((str == NULL) || (cpuset == NULL) || (maxcpu <= 0) ||
        (maxcpu > 100000))
        return (-1);

    cur = *str;
    virSkipSpaces(&cur);
    if (*cur == 0)
        goto parse_error;

    /* initialize cpumap to all 0s */
    for (i = 0; i < maxcpu; i++)
        cpuset[i] = 0;
    ret = 0;

    while ((*cur != 0) && (*cur != sep)) {
        /*
         * 3 constructs are allowed:
         *     - N   : a single CPU number
         *     - N-M : a range of CPU numbers with N < M
         *     - ^N  : remove a single CPU number from the current set
         */
        if (*cur == '^') {
            cur++;
            neg = 1;
        }

        if (!c_isdigit(*cur))
            goto parse_error;
        start = virDomainCpuNumberParse(&cur, maxcpu);
        if (start < 0)
            goto parse_error;
        virSkipSpaces(&cur);
        if ((*cur == ',') || (*cur == 0) || (*cur == sep)) {
            if (neg) {
                if (cpuset[start] == 1) {
                    cpuset[start] = 0;
                    ret--;
                }
            } else {
                if (cpuset[start] == 0) {
                    cpuset[start] = 1;
                    ret++;
                }
            }
        } else if (*cur == '-') {
            if (neg)
                goto parse_error;
            cur++;
            virSkipSpaces(&cur);
            last = virDomainCpuNumberParse(&cur, maxcpu);
            if (last < start)
                goto parse_error;
            for (i = start; i <= last; i++) {
                if (cpuset[i] == 0) {
                    cpuset[i] = 1;
                    ret++;
                }
            }
            virSkipSpaces(&cur);
        }
        if (*cur == ',') {
            cur++;
            virSkipSpaces(&cur);
            neg = 0;
        } else if ((*cur == 0) || (*cur == sep)) {
            break;
        } else
            goto parse_error;
    }
    *str = cur;
    return (ret);

  parse_error:
    virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                         "%s", _("topology cpuset syntax error"));
    return (-1);
}


/* Check if vcpupin with same vcpuid already exists.
 * Return 1 if exists, 0 if not. */
int
virDomainVcpuPinIsDuplicate(virDomainVcpuPinDefPtr *def,
                            int nvcpupin,
                            int vcpu)
{
    int i;

    if (!def || !nvcpupin)
        return 0;

    for (i = 0; i < nvcpupin; i++) {
        if (def[i]->vcpuid == vcpu)
            return 1;
    }

    return 0;
}

virDomainVcpuPinDefPtr
virDomainVcpuPinFindByVcpu(virDomainVcpuPinDefPtr *def,
                           int nvcpupin,
                           int vcpu)
{
    int i;

    if (!def || !nvcpupin)
        return NULL;

    for (i = 0; i < nvcpupin; i++) {
        if (def[i]->vcpuid == vcpu)
            return def[i];
    }

    return NULL;
}

int
virDomainVcpuPinAdd(virDomainDefPtr def,
                    unsigned char *cpumap,
                    int maplen,
                    int vcpu)
{
    virDomainVcpuPinDefPtr *vcpupin_list = NULL;
    virDomainVcpuPinDefPtr vcpupin = NULL;
    char *cpumask = NULL;
    int i;

    if (VIR_ALLOC_N(cpumask, VIR_DOMAIN_CPUMASK_LEN) < 0) {
        virReportOOMError();
        goto cleanup;
    }

    /* Reset cpumask to all 0s. */
    for (i = 0; i < VIR_DOMAIN_CPUMASK_LEN; i++)
        cpumask[i] = 0;

    /* Convert bitmap (cpumap) to cpumask, which is byte map? */
    for (i = 0; i < maplen; i++) {
        int cur;

        for (cur = 0; cur < 8; cur++) {
            if (cpumap[i] & (1 << cur))
                cpumask[i * 8 + cur] = 1;
        }
    }

    /* No vcpupin exists yet. */
    if (!def->cputune.nvcpupin) {
        if (VIR_ALLOC(vcpupin) < 0) {
            virReportOOMError();
            goto cleanup;
        }

        if (VIR_ALLOC(vcpupin_list) < 0) {
            virReportOOMError();
            VIR_FREE(vcpupin);
            goto cleanup;
        }

        vcpupin->vcpuid = vcpu;
        vcpupin->cpumask = cpumask;
        vcpupin_list[def->cputune.nvcpupin++] = vcpupin;

        def->cputune.vcpupin = vcpupin_list;
    } else {
        if (virDomainVcpuPinIsDuplicate(def->cputune.vcpupin,
                                        def->cputune.nvcpupin,
                                        vcpu)) {
            vcpupin = virDomainVcpuPinFindByVcpu(def->cputune.vcpupin,
                                                 def->cputune.nvcpupin,
                                                 vcpu);
            vcpupin->vcpuid = vcpu;
            vcpupin->cpumask = cpumask;
        } else {
            if (VIR_ALLOC(vcpupin) < 0) {
                virReportOOMError();
                goto cleanup;
            }

            if (VIR_REALLOC_N(def->cputune.vcpupin, def->cputune.nvcpupin + 1) < 0) {
                virReportOOMError();
                VIR_FREE(vcpupin);
                goto cleanup;
            }

            vcpupin->vcpuid = vcpu;
            vcpupin->cpumask = cpumask;
            def->cputune.vcpupin[def->cputune.nvcpupin++] = vcpupin;
       }
    }

    return 0;

cleanup:
    VIR_FREE(cpumask);
    return -1;
}

int
virDomainVcpuPinDel(virDomainDefPtr def, int vcpu)
{
    int n;
    bool deleted = false;
    virDomainVcpuPinDefPtr *vcpupin_list = def->cputune.vcpupin;

    /* No vcpupin exists yet */
    if (!def->cputune.nvcpupin) {
        return 0;
    }

    for (n = 0; n < def->cputune.nvcpupin; n++) {
        if (vcpupin_list[n]->vcpuid == vcpu) {
            VIR_FREE(vcpupin_list[n]->cpumask);
            VIR_FREE(vcpupin_list[n]);
            memmove(&vcpupin_list[n],
                    &vcpupin_list[n+1],
                    (def->cputune.nvcpupin - n - 1) * sizeof(virDomainVcpuPinDef *));
            deleted = true;
            break;
        }
    }

    if (!deleted)
        return 0;

    if (--def->cputune.nvcpupin == 0) {
        VIR_FREE(def->cputune.vcpupin);
    } else {
        if (VIR_REALLOC_N(def->cputune.vcpupin, def->cputune.nvcpupin) < 0) {
            virReportOOMError();
            return -1;
        }
    }

    return 0;
}

static int
virDomainLifecycleDefFormat(virBufferPtr buf,
                            int type,
                            const char *name,
                            virLifecycleToStringFunc convFunc)
{
    const char *typeStr = convFunc(type);
    if (!typeStr) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                             _("unexpected lifecycle type %d"), type);
        return -1;
    }

    virBufferAsprintf(buf, "  <%s>%s</%s>\n", name, typeStr, name);

    return 0;
}


static int
virDomainLeaseDefFormat(virBufferPtr buf,
                        virDomainLeaseDefPtr def)
{
    virBufferAddLit(buf, "    <lease>\n");
    virBufferEscapeString(buf, "      <lockspace>%s</lockspace>\n", def->lockspace);
    virBufferEscapeString(buf, "      <key>%s</key>\n", def->key);
    virBufferEscapeString(buf, "      <target path='%s'", def->path);
    if (def->offset)
        virBufferAsprintf(buf, " offset='%llu'", def->offset);
    virBufferAddLit(buf, "/>\n");
    virBufferAddLit(buf, "    </lease>\n");

    return 0;
}

static int
virDomainDiskDefFormat(virBufferPtr buf,
                       virDomainDiskDefPtr def,
                       unsigned int flags)
{
    const char *type = virDomainDiskTypeToString(def->type);
    const char *device = virDomainDiskDeviceTypeToString(def->device);
    const char *bus = virDomainDiskBusTypeToString(def->bus);
    const char *cachemode = virDomainDiskCacheTypeToString(def->cachemode);
    const char *error_policy = virDomainDiskErrorPolicyTypeToString(def->error_policy);
    const char *iomode = virDomainDiskIoTypeToString(def->iomode);
    const char *ioeventfd = virDomainIoEventFdTypeToString(def->ioeventfd);
    const char *event_idx = virDomainVirtioEventIdxTypeToString(def->event_idx);

    if (!type) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                             _("unexpected disk type %d"), def->type);
        return -1;
    }
    if (!device) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                             _("unexpected disk device %d"), def->device);
        return -1;
    }
    if (!bus) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                             _("unexpected disk bus %d"), def->bus);
        return -1;
    }
    if (!cachemode) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                             _("unexpected disk cache mode %d"), def->cachemode);
        return -1;
    }
    if (!iomode) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                             _("unexpected disk io mode %d"), def->iomode);
        return -1;
    }

    virBufferAsprintf(buf,
                      "    <disk type='%s' device='%s'",
                      type, device);
    if (def->snapshot &&
        !(def->snapshot == VIR_DOMAIN_DISK_SNAPSHOT_NO && def->readonly))
        virBufferAsprintf(buf, " snapshot='%s'",
                          virDomainDiskSnapshotTypeToString(def->snapshot));
    virBufferAddLit(buf, ">\n");

    if (def->driverName || def->driverType || def->cachemode ||
        def->ioeventfd || def->event_idx) {
        virBufferAsprintf(buf, "      <driver");
        if (def->driverName)
            virBufferAsprintf(buf, " name='%s'", def->driverName);
        if (def->driverType)
            virBufferAsprintf(buf, " type='%s'", def->driverType);
        if (def->cachemode)
            virBufferAsprintf(buf, " cache='%s'", cachemode);
        if (def->error_policy)
            virBufferAsprintf(buf, " error_policy='%s'", error_policy);
        if (def->iomode)
            virBufferAsprintf(buf, " io='%s'", iomode);
        if (def->ioeventfd)
            virBufferAsprintf(buf, " ioeventfd='%s'", ioeventfd);
        if (def->event_idx)
            virBufferAsprintf(buf, " event_idx='%s'", event_idx);
        virBufferAsprintf(buf, "/>\n");
    }

    if (def->src || def->nhosts > 0) {
        switch (def->type) {
        case VIR_DOMAIN_DISK_TYPE_FILE:
            virBufferEscapeString(buf, "      <source file='%s'/>\n",
                                  def->src);
            break;
        case VIR_DOMAIN_DISK_TYPE_BLOCK:
            virBufferEscapeString(buf, "      <source dev='%s'/>\n",
                                  def->src);
            break;
        case VIR_DOMAIN_DISK_TYPE_DIR:
            virBufferEscapeString(buf, "      <source dir='%s'/>\n",
                                  def->src);
            break;
        case VIR_DOMAIN_DISK_TYPE_NETWORK:
            virBufferAsprintf(buf, "      <source protocol='%s'",
                              virDomainDiskProtocolTypeToString(def->protocol));
            if (def->src) {
                virBufferEscapeString(buf, " name='%s'", def->src);
            }
            if (def->nhosts == 0) {
                virBufferAsprintf(buf, "/>\n");
            } else {
                int i;

                virBufferAsprintf(buf, ">\n");
                for (i = 0; i < def->nhosts; i++) {
                    virBufferEscapeString(buf, "        <host name='%s'",
                                          def->hosts[i].name);
                    virBufferEscapeString(buf, " port='%s'/>\n",
                                          def->hosts[i].port);
                }
                virBufferAsprintf(buf, "      </source>\n");
            }
            break;
        default:
            virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                 _("unexpected disk type %s"),
                                 virDomainDiskTypeToString(def->type));
            return -1;
        }
    }

    virBufferAsprintf(buf, "      <target dev='%s' bus='%s'/>\n",
                      def->dst, bus);

    if (def->bootIndex)
        virBufferAsprintf(buf, "      <boot order='%d'/>\n", def->bootIndex);
    if (def->readonly)
        virBufferAddLit(buf, "      <readonly/>\n");
    if (def->shared)
        virBufferAddLit(buf, "      <shareable/>\n");
    if (def->transient)
        virBufferAddLit(buf, "      <transient/>\n");
    if (def->serial)
        virBufferEscapeString(buf, "      <serial>%s</serial>\n",
                              def->serial);
    if (def->encryption != NULL &&
        virStorageEncryptionFormat(buf, def->encryption, 6) < 0)
        return -1;

    if (virDomainDeviceInfoFormat(buf, &def->info, flags) < 0)
        return -1;

    virBufferAddLit(buf, "    </disk>\n");

    return 0;
}

static int
virDomainControllerDefFormat(virBufferPtr buf,
                             virDomainControllerDefPtr def,
                             unsigned int flags)
{
    const char *type = virDomainControllerTypeToString(def->type);
    const char *model = NULL;

    if (!type) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                             _("unexpected controller type %d"), def->type);
        return -1;
    }

    if (def->model != -1) {
        model = virDomainControllerModelTypeToString(def->model);

        if (!model) {
            virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                 _("unexpected model type %d"), def->model);
            return -1;
        }
    }

    virBufferAsprintf(buf,
                      "    <controller type='%s' index='%d'",
                      type, def->idx);

    if (model) {
        virBufferEscapeString(buf, " model='%s'", model);
    }

    switch (def->type) {
    case VIR_DOMAIN_CONTROLLER_TYPE_VIRTIO_SERIAL:
        if (def->opts.vioserial.ports != -1) {
            virBufferAsprintf(buf, " ports='%d'",
                              def->opts.vioserial.ports);
        }
        if (def->opts.vioserial.vectors != -1) {
            virBufferAsprintf(buf, " vectors='%d'",
                              def->opts.vioserial.vectors);
        }
        break;

    default:
        break;
    }

    if (virDomainDeviceInfoIsSet(&def->info, flags)) {
        virBufferAddLit(buf, ">\n");
        if (virDomainDeviceInfoFormat(buf, &def->info, flags) < 0)
            return -1;
        virBufferAddLit(buf, "    </controller>\n");
    } else {
        virBufferAddLit(buf, "/>\n");
    }

    return 0;
}

static int
virDomainFSDefFormat(virBufferPtr buf,
                     virDomainFSDefPtr def,
                     unsigned int flags)
{
    const char *type = virDomainFSTypeToString(def->type);
    const char *accessmode = virDomainFSAccessModeTypeToString(def->accessmode);

    if (!type) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                             _("unexpected filesystem type %d"), def->type);
        return -1;
    }

   if (!accessmode) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                             _("unexpected accessmode %d"), def->accessmode);
        return -1;
    }


    virBufferAsprintf(buf,
                      "    <filesystem type='%s' accessmode='%s'>\n",
                      type, accessmode);

    if (def->src) {
        switch (def->type) {
        case VIR_DOMAIN_FS_TYPE_MOUNT:
            virBufferEscapeString(buf, "      <source dir='%s'/>\n",
                                  def->src);
            break;

        case VIR_DOMAIN_FS_TYPE_BLOCK:
            virBufferEscapeString(buf, "      <source dev='%s'/>\n",
                                  def->src);
            break;

        case VIR_DOMAIN_FS_TYPE_FILE:
            virBufferEscapeString(buf, "      <source file='%s'/>\n",
                                  def->src);
            break;

        case VIR_DOMAIN_FS_TYPE_TEMPLATE:
            virBufferEscapeString(buf, "      <source name='%s'/>\n",
                                  def->src);
        }
    }

    virBufferEscapeString(buf, "      <target dir='%s'/>\n",
                          def->dst);

    if (def->readonly)
        virBufferAddLit(buf, "      <readonly/>\n");

    if (virDomainDeviceInfoFormat(buf, &def->info, flags) < 0)
        return -1;

    virBufferAddLit(buf, "    </filesystem>\n");

    return 0;
}

static int
virDomainActualNetDefFormat(virBufferPtr buf,
                            virDomainActualNetDefPtr def)
{
    int ret = -1;
    const char *type;
    const char *mode;
    char *vlan;

    if (!def)
        return 0;

    type = virDomainNetTypeToString(def->type);
    if (!type) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                             _("unexpected net type %d"), def->type);
        return ret;
    }

    if (def->type != VIR_DOMAIN_NET_TYPE_BRIDGE &&
        def->type != VIR_DOMAIN_NET_TYPE_DIRECT &&
        def->type != VIR_DOMAIN_NET_TYPE_NETWORK) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                             _("unexpected net type %s"), type);
        goto error;
    }
    virBufferAsprintf(buf, "      <actual type='%s'>\n", type);

    switch (def->type) {
    case VIR_DOMAIN_NET_TYPE_BRIDGE:
        if (def->data.bridge.brname) {
            virBufferEscapeString(buf, "        <source bridge='%s'/>\n",
                                  def->data.bridge.brname);
        }
        break;

    case VIR_DOMAIN_NET_TYPE_DIRECT:
        virBufferAddLit(buf, "        <source");
        if (def->data.direct.linkdev)
            virBufferEscapeString(buf, " dev='%s'",
                                  def->data.direct.linkdev);

        mode = virMacvtapModeTypeToString(def->data.direct.mode);
        if (!mode) {
            virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                 _("unexpected source mode %d"),
                                 def->data.direct.mode);
            return ret;
        }
        virBufferAsprintf(buf, " mode='%s'", mode);
        if (def->data.direct.vf_pci_addr)
            virBufferEscapeString(buf, " vf_pci_addr='%s'",
                                  def->data.direct.vf_pci_addr);
        if (def->data.direct.vlan) {
            if (virAsprintf(&vlan, "%d", def->data.direct.vlan) < 0)
                goto error;
            virBufferEscapeString(buf, " vlan=%s'", vlan);
        }
        virBufferAddLit(buf, "/>\n"); 
        virVirtualPortProfileFormat(buf, def->data.direct.virtPortProfile,
                                    "        ");
        break;
    default:
        break;
    }

    if (virBandwidthDefFormat(buf, def->bandwidth, "      ") < 0)
        goto error;

    virBufferAddLit(buf, "      </actual>\n");

    ret = 0;
error:
    return ret;
}

static int
virDomainNetDefFormat(virBufferPtr buf,
                      virDomainNetDefPtr def,
                      unsigned int flags)
{
    const char *type = virDomainNetTypeToString(def->type);
    char *attrs;

    if (!type) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                             _("unexpected net type %d"), def->type);
        return -1;
    }

    virBufferAsprintf(buf, "    <interface type='%s'>\n", type);

    virBufferAsprintf(buf,
                      "      <mac address='%02x:%02x:%02x:%02x:%02x:%02x'/>\n",
                      def->mac[0], def->mac[1], def->mac[2],
                      def->mac[3], def->mac[4], def->mac[5]);

    switch (def->type) {
    case VIR_DOMAIN_NET_TYPE_NETWORK:
        virBufferEscapeString(buf, "      <source network='%s'",
                              def->data.network.name);
        if (def->data.network.portgroup) {
           virBufferEscapeString(buf, " portgroup='%s'",
                                 def->data.network.portgroup);
        }
        virBufferAddLit(buf, "/>\n");
        virVirtualPortProfileFormat(buf, def->data.network.virtPortProfile,
                                    "      ");
        if ((flags & VIR_DOMAIN_XML_INTERNAL_ACTUAL_NET) &&
            (virDomainActualNetDefFormat(buf, def->data.network.actual) < 0))
            return -1;
        break;

    case VIR_DOMAIN_NET_TYPE_ETHERNET:
        if (def->data.ethernet.dev)
            virBufferEscapeString(buf, "      <source dev='%s'/>\n",
                                  def->data.ethernet.dev);
        if (def->data.ethernet.ipaddr)
            virBufferAsprintf(buf, "      <ip address='%s'/>\n",
                              def->data.ethernet.ipaddr);
        if (def->data.ethernet.script)
            virBufferEscapeString(buf, "      <script path='%s'/>\n",
                                  def->data.ethernet.script);
        break;

    case VIR_DOMAIN_NET_TYPE_BRIDGE:
        virBufferEscapeString(buf, "      <source bridge='%s'/>\n",
                              def->data.bridge.brname);
        if (def->data.bridge.ipaddr)
            virBufferAsprintf(buf, "      <ip address='%s'/>\n",
                              def->data.bridge.ipaddr);
        if (def->data.bridge.script)
            virBufferEscapeString(buf, "      <script path='%s'/>\n",
                                  def->data.bridge.script);
        break;

    case VIR_DOMAIN_NET_TYPE_SERVER:
    case VIR_DOMAIN_NET_TYPE_CLIENT:
    case VIR_DOMAIN_NET_TYPE_MCAST:
        if (def->data.socket.address)
            virBufferAsprintf(buf, "      <source address='%s' port='%d'/>\n",
                              def->data.socket.address, def->data.socket.port);
        else
            virBufferAsprintf(buf, "      <source port='%d'/>\n",
                              def->data.socket.port);
        break;

    case VIR_DOMAIN_NET_TYPE_INTERNAL:
        virBufferEscapeString(buf, "      <source name='%s'/>\n",
                              def->data.internal.name);
        break;

    case VIR_DOMAIN_NET_TYPE_DIRECT:
        virBufferEscapeString(buf, "      <source dev='%s'",
                              def->data.direct.linkdev);
        virBufferAsprintf(buf, " mode='%s'",
                   virMacvtapModeTypeToString(def->data.direct.mode));
        virBufferEscapeString(buf, " vf_pci_addr='%s'",
                              def->data.direct.vf_pci_addr);
        // Add formatting for VLAN here: SSHAH
        virBufferAddLit(buf, "/>\n");
        virVirtualPortProfileFormat(buf, def->data.direct.virtPortProfile,
                                    "      ");
        break;

    case VIR_DOMAIN_NET_TYPE_USER:
    case VIR_DOMAIN_NET_TYPE_LAST:
        break;
    }


    if (def->ifname &&
        !((flags & VIR_DOMAIN_XML_INACTIVE) &&
          (STRPREFIX(def->ifname, VIR_NET_GENERATED_PREFIX)))) {
        /* Skip auto-generated target names for inactive config. */
        virBufferEscapeString(buf, "      <target dev='%s'/>\n",
                              def->ifname);
    }
    if (def->model) {
        virBufferEscapeString(buf, "      <model type='%s'/>\n",
                              def->model);
        if (STREQ(def->model, "virtio") &&
            (def->driver.virtio.name || def->driver.virtio.txmode)) {
            virBufferAddLit(buf, "      <driver");
            if (def->driver.virtio.name) {
                virBufferAsprintf(buf, " name='%s'",
                                  virDomainNetBackendTypeToString(def->driver.virtio.name));
            }
            if (def->driver.virtio.txmode) {
                virBufferAsprintf(buf, " txmode='%s'",
                                  virDomainNetVirtioTxModeTypeToString(def->driver.virtio.txmode));
            }
            if (def->driver.virtio.ioeventfd) {
                virBufferAsprintf(buf, " ioeventfd='%s'",
                                  virDomainIoEventFdTypeToString(def->driver.virtio.ioeventfd));
            }
            if (def->driver.virtio.event_idx) {
                virBufferAsprintf(buf, " event_idx='%s'",
                                  virDomainVirtioEventIdxTypeToString(def->driver.virtio.event_idx));
            }
            virBufferAddLit(buf, "/>\n");
        }
    }
    if (def->filter) {
        virBufferEscapeString(buf, "      <filterref filter='%s'",
                              def->filter);
        attrs = virNWFilterFormatParamAttributes(def->filterparams,
                                                 "        ");
        if (!attrs || strlen(attrs) <= 1)
            virBufferAddLit(buf, "/>\n");
        else
            virBufferAsprintf(buf, ">\n%s      </filterref>\n", attrs);
        VIR_FREE(attrs);
    }
    if (def->bootIndex)
        virBufferAsprintf(buf, "      <boot order='%d'/>\n", def->bootIndex);

    if (def->tune.sndbuf_specified) {
        virBufferAddLit(buf,   "      <tune>\n");
        virBufferAsprintf(buf, "        <sndbuf>%lu</sndbuf>\n", def->tune.sndbuf);
        virBufferAddLit(buf,   "      </tune>\n");
    }

    if (virBandwidthDefFormat(buf, def->bandwidth, "      ") < 0)
        return -1;

    if (virDomainDeviceInfoFormat(buf, &def->info, flags) < 0)
        return -1;

    virBufferAddLit(buf, "    </interface>\n");

    return 0;
}


/* Assumes that "<device" has already been generated, and starts
 * output at " type='type'>". */
static int
virDomainChrSourceDefFormat(virBufferPtr buf,
                            virDomainChrSourceDefPtr def,
                            bool tty_compat,
                            unsigned int flags)
{
    const char *type = virDomainChrTypeToString(def->type);

    if (!type) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                             _("unexpected char type %d"), def->type);
        return -1;
    }

    /* Compat with legacy  <console tty='/dev/pts/5'/> syntax */
    virBufferAsprintf(buf, " type='%s'", type);
    if (tty_compat) {
        virBufferEscapeString(buf, " tty='%s'",
                              def->data.file.path);
    }
    virBufferAddLit(buf, ">\n");

    switch (def->type) {
    case VIR_DOMAIN_CHR_TYPE_NULL:
    case VIR_DOMAIN_CHR_TYPE_VC:
    case VIR_DOMAIN_CHR_TYPE_STDIO:
    case VIR_DOMAIN_CHR_TYPE_SPICEVMC:
        /* nada */
        break;

    case VIR_DOMAIN_CHR_TYPE_PTY:
    case VIR_DOMAIN_CHR_TYPE_DEV:
    case VIR_DOMAIN_CHR_TYPE_FILE:
    case VIR_DOMAIN_CHR_TYPE_PIPE:
        if (def->type != VIR_DOMAIN_CHR_TYPE_PTY ||
            (def->data.file.path &&
             !(flags & VIR_DOMAIN_XML_INACTIVE))) {
            virBufferEscapeString(buf, "      <source path='%s'/>\n",
                                  def->data.file.path);
        }
        break;

    case VIR_DOMAIN_CHR_TYPE_UDP:
        if (def->data.udp.bindService &&
            def->data.udp.bindHost) {
            virBufferAsprintf(buf,
                              "      <source mode='bind' host='%s' "
                              "service='%s'/>\n",
                              def->data.udp.bindHost,
                              def->data.udp.bindService);
        } else if (def->data.udp.bindHost) {
            virBufferAsprintf(buf, "      <source mode='bind' host='%s'/>\n",
                              def->data.udp.bindHost);
        } else if (def->data.udp.bindService) {
            virBufferAsprintf(buf, "      <source mode='bind' service='%s'/>\n",
                              def->data.udp.bindService);
        }

        if (def->data.udp.connectService &&
            def->data.udp.connectHost) {
            virBufferAsprintf(buf,
                              "      <source mode='connect' host='%s' "
                              "service='%s'/>\n",
                              def->data.udp.connectHost,
                              def->data.udp.connectService);
        } else if (def->data.udp.connectHost) {
            virBufferAsprintf(buf, "      <source mode='connect' host='%s'/>\n",
                              def->data.udp.connectHost);
        } else if (def->data.udp.connectService) {
            virBufferAsprintf(buf,
                              "      <source mode='connect' service='%s'/>\n",
                              def->data.udp.connectService);
        }
        break;

    case VIR_DOMAIN_CHR_TYPE_TCP:
        virBufferAsprintf(buf,
                          "      <source mode='%s' host='%s' service='%s'/>\n",
                          def->data.tcp.listen ? "bind" : "connect",
                          def->data.tcp.host,
                          def->data.tcp.service);
        virBufferAsprintf(buf, "      <protocol type='%s'/>\n",
                          virDomainChrTcpProtocolTypeToString(
                              def->data.tcp.protocol));
        break;

    case VIR_DOMAIN_CHR_TYPE_UNIX:
        virBufferAsprintf(buf, "      <source mode='%s'",
                          def->data.nix.listen ? "bind" : "connect");
        virBufferEscapeString(buf, " path='%s'/>\n",
                              def->data.nix.path);
        break;
    }

    return 0;
}

static int
virDomainChrDefFormat(virBufferPtr buf,
                      virDomainChrDefPtr def,
                      unsigned int flags)
{
    const char *elementName = virDomainChrDeviceTypeToString(def->deviceType);
    const char *targetType = virDomainChrTargetTypeToString(def->deviceType,
                                                            def->targetType);
    bool tty_compat;

    int ret = 0;

    if (!elementName) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                             _("unexpected char device type %d"),
                             def->deviceType);
        return -1;
    }

    virBufferAsprintf(buf, "    <%s", elementName);
    tty_compat = (def->deviceType == VIR_DOMAIN_CHR_DEVICE_TYPE_CONSOLE &&
                  def->target.port == 0 &&
                  def->source.type == VIR_DOMAIN_CHR_TYPE_PTY &&
                  !(flags & VIR_DOMAIN_XML_INACTIVE) &&
                  def->source.data.file.path);
    if (virDomainChrSourceDefFormat(buf, &def->source, tty_compat, flags) < 0)
        return -1;

    /* Format <target> block */
    switch (def->deviceType) {
    case VIR_DOMAIN_CHR_DEVICE_TYPE_CHANNEL: {
        if (!targetType) {
            virDomainReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                                 _("Could not format channel target type"));
            return -1;
        }
        virBufferAsprintf(buf, "      <target type='%s'", targetType);

        switch (def->targetType) {
        case VIR_DOMAIN_CHR_CHANNEL_TARGET_TYPE_GUESTFWD: {
            int port = virSocketGetPort(def->target.addr);
            if (port < 0) {
                virDomainReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                                     _("Unable to format guestfwd port"));
                return -1;
            }

            const char *addr = virSocketFormatAddr(def->target.addr);
            if (addr == NULL)
                return -1;

            virBufferAsprintf(buf, " address='%s' port='%d'",
                              addr, port);
            VIR_FREE(addr);
            break;
        }

        case VIR_DOMAIN_CHR_CHANNEL_TARGET_TYPE_VIRTIO: {
            if (def->target.name) {
                virBufferEscapeString(buf, " name='%s'", def->target.name);
            }
            break;
        }

        }
        virBufferAddLit(buf, "/>\n");
        break;
    }

    case VIR_DOMAIN_CHR_DEVICE_TYPE_CONSOLE:
        virBufferAsprintf(buf,
                          "      <target type='%s' port='%d'/>\n",
                          virDomainChrTargetTypeToString(def->deviceType,
                                                         def->targetType),
                          def->target.port);
        break;

    default:
        virBufferAsprintf(buf, "      <target port='%d'/>\n",
                          def->target.port);
        break;
    }

    if (virDomainDeviceInfoIsSet(&def->info, flags)) {
        if (virDomainDeviceInfoFormat(buf, &def->info, flags) < 0)
            return -1;
    }

    virBufferAsprintf(buf, "    </%s>\n",
                      elementName);

    return ret;
}

static int
virDomainSmartcardDefFormat(virBufferPtr buf,
                            virDomainSmartcardDefPtr def,
                            unsigned int flags)
{
    const char *mode = virDomainSmartcardTypeToString(def->type);
    size_t i;

    if (!mode) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                             _("unexpected smartcard type %d"), def->type);
        return -1;
    }

    virBufferAsprintf(buf, "    <smartcard mode='%s'", mode);
    switch (def->type) {
    case VIR_DOMAIN_SMARTCARD_TYPE_HOST:
        if (!virDomainDeviceInfoIsSet(&def->info, flags)) {
            virBufferAddLit(buf, "/>\n");
            return 0;
        }
        virBufferAddLit(buf, ">\n");
        break;

    case VIR_DOMAIN_SMARTCARD_TYPE_HOST_CERTIFICATES:
        virBufferAddLit(buf, ">\n");
        for (i = 0; i < VIR_DOMAIN_SMARTCARD_NUM_CERTIFICATES; i++)
            virBufferEscapeString(buf, "      <certificate>%s</certificate>\n",
                                  def->data.cert.file[i]);
        if (def->data.cert.database)
            virBufferEscapeString(buf, "      <database>%s</database>\n",
                                  def->data.cert.database);
        break;

    case VIR_DOMAIN_SMARTCARD_TYPE_PASSTHROUGH:
        if (virDomainChrSourceDefFormat(buf, &def->data.passthru, false,
                                        flags) < 0)
            return -1;
        break;

    default:
        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                             _("unexpected smartcard type %d"), def->type);
        return -1;
    }
    if (virDomainDeviceInfoFormat(buf, &def->info, flags) < 0)
        return -1;
    virBufferAddLit(buf, "    </smartcard>\n");
    return 0;
}

static int
virDomainSoundDefFormat(virBufferPtr buf,
                        virDomainSoundDefPtr def,
                        unsigned int flags)
{
    const char *model = virDomainSoundModelTypeToString(def->model);

    if (!model) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                             _("unexpected sound model %d"), def->model);
        return -1;
    }

    virBufferAsprintf(buf, "    <sound model='%s'",
                      model);

    if (virDomainDeviceInfoIsSet(&def->info, flags)) {
        virBufferAddLit(buf, ">\n");
        if (virDomainDeviceInfoFormat(buf, &def->info, flags) < 0)
            return -1;
        virBufferAddLit(buf, "    </sound>\n");
    } else {
        virBufferAddLit(buf, "/>\n");
    }

    return 0;
}


static int
virDomainMemballoonDefFormat(virBufferPtr buf,
                             virDomainMemballoonDefPtr def,
                             unsigned int flags)
{
    const char *model = virDomainMemballoonModelTypeToString(def->model);

    if (!model) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                             _("unexpected memballoon model %d"), def->model);
        return -1;
    }

    virBufferAsprintf(buf, "    <memballoon model='%s'",
                      model);

    if (virDomainDeviceInfoIsSet(&def->info, flags)) {
        virBufferAddLit(buf, ">\n");
        if (virDomainDeviceInfoFormat(buf, &def->info, flags) < 0)
            return -1;
        virBufferAddLit(buf, "    </memballoon>\n");
    } else {
        virBufferAddLit(buf, "/>\n");
    }

    return 0;
}

static int
virDomainSysinfoDefFormat(virBufferPtr buf,
                          virSysinfoDefPtr def)
{
    char *format = virSysinfoFormat(def, "  ");

    if (!format)
        return -1;
    virBufferAdd(buf, format, strlen(format));
    VIR_FREE(format);
    return 0;
}


static int
virDomainWatchdogDefFormat(virBufferPtr buf,
                           virDomainWatchdogDefPtr def,
                           unsigned int flags)
{
    const char *model = virDomainWatchdogModelTypeToString (def->model);
    const char *action = virDomainWatchdogActionTypeToString (def->action);

    if (!model) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                             _("unexpected watchdog model %d"), def->model);
        return -1;
    }

    if (!action) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                             _("unexpected watchdog action %d"), def->action);
        return -1;
    }

    virBufferAsprintf(buf, "    <watchdog model='%s' action='%s'",
                      model, action);

    if (virDomainDeviceInfoIsSet(&def->info, flags)) {
        virBufferAddLit(buf, ">\n");
        if (virDomainDeviceInfoFormat(buf, &def->info, flags) < 0)
            return -1;
        virBufferAddLit(buf, "    </watchdog>\n");
    } else {
        virBufferAddLit(buf, "/>\n");
    }

    return 0;
}


static void
virDomainVideoAccelDefFormat(virBufferPtr buf,
                             virDomainVideoAccelDefPtr def)
{
    virBufferAsprintf(buf, "        <acceleration accel3d='%s'",
                      def->support3d ? "yes" : "no");
    virBufferAsprintf(buf, " accel2d='%s'",
                      def->support2d ? "yes" : "no");
    virBufferAddLit(buf, "/>\n");
}


static int
virDomainVideoDefFormat(virBufferPtr buf,
                        virDomainVideoDefPtr def,
                        unsigned int flags)
{
    const char *model = virDomainVideoTypeToString(def->type);

    if (!model) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                             _("unexpected video model %d"), def->type);
        return -1;
    }

    virBufferAddLit(buf, "    <video>\n");
    virBufferAsprintf(buf, "      <model type='%s'",
                      model);
    if (def->vram)
        virBufferAsprintf(buf, " vram='%u'", def->vram);
    if (def->heads)
        virBufferAsprintf(buf, " heads='%u'", def->heads);
    if (def->accel) {
        virBufferAddLit(buf, ">\n");
        virDomainVideoAccelDefFormat(buf, def->accel);
        virBufferAddLit(buf, "      </model>\n");
    } else {
        virBufferAddLit(buf, "/>\n");
    }

    if (virDomainDeviceInfoFormat(buf, &def->info, flags) < 0)
        return -1;

    virBufferAddLit(buf, "    </video>\n");

    return 0;
}

static int
virDomainInputDefFormat(virBufferPtr buf,
                        virDomainInputDefPtr def,
                        unsigned int flags)
{
    const char *type = virDomainInputTypeToString(def->type);
    const char *bus = virDomainInputBusTypeToString(def->bus);

    if (!type) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                             _("unexpected input type %d"), def->type);
        return -1;
    }
    if (!bus) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                             _("unexpected input bus type %d"), def->bus);
        return -1;
    }

    virBufferAsprintf(buf, "    <input type='%s' bus='%s'",
                      type, bus);

    if (virDomainDeviceInfoIsSet(&def->info, flags)) {
        virBufferAddLit(buf, ">\n");
        if (virDomainDeviceInfoFormat(buf, &def->info, flags) < 0)
            return -1;
        virBufferAddLit(buf, "    </input>\n");
    } else {
        virBufferAddLit(buf, "/>\n");
    }

    return 0;
}


static int
virDomainTimerDefFormat(virBufferPtr buf,
                        virDomainTimerDefPtr def)
{
    const char *name = virDomainTimerNameTypeToString(def->name);

    if (!name) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                             _("unexpected timer name %d"), def->name);
        return -1;
    }
    virBufferAsprintf(buf, "    <timer name='%s'", name);

    if (def->present == 0) {
        virBufferAddLit(buf, " present='no'");
    } else if (def->present == 1) {
        virBufferAddLit(buf, " present='yes'");
    }

    if (def->tickpolicy != -1) {
        const char *tickpolicy
            = virDomainTimerTickpolicyTypeToString(def->tickpolicy);
        if (!tickpolicy) {
            virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                 _("unexpected timer tickpolicy %d"),
                                 def->tickpolicy);
            return -1;
        }
        virBufferAsprintf(buf, " tickpolicy='%s'", tickpolicy);
    }

    if ((def->name == VIR_DOMAIN_TIMER_NAME_PLATFORM)
        || (def->name == VIR_DOMAIN_TIMER_NAME_RTC)) {
        if (def->track != -1) {
            const char *track
                = virDomainTimerTrackTypeToString(def->track);
            if (!track) {
                virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                     _("unexpected timer track %d"),
                                     def->track);
                return -1;
            }
            virBufferAsprintf(buf, " track='%s'", track);
        }
    }

    if (def->name == VIR_DOMAIN_TIMER_NAME_TSC) {
        if (def->frequency > 0) {
            virBufferAsprintf(buf, " frequency='%lu'", def->frequency);
        }

        if (def->mode != -1) {
            const char *mode
                = virDomainTimerModeTypeToString(def->mode);
            if (!mode) {
                virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                     _("unexpected timer mode %d"),
                                     def->mode);
                return -1;
            }
            virBufferAsprintf(buf, " mode='%s'", mode);
        }
    }

    if ((def->catchup.threshold == 0)
        && (def->catchup.slew == 0)
        && (def->catchup.limit == 0)) {
        virBufferAddLit(buf, "/>\n");
    } else {
        virBufferAddLit(buf, ">\n      <catchup ");
        if (def->catchup.threshold > 0) {
            virBufferAsprintf(buf, " threshold='%lu'", def->catchup.threshold);
        }
        if (def->catchup.slew > 0) {
            virBufferAsprintf(buf, " slew='%lu'", def->catchup.slew);
        }
        if (def->catchup.limit > 0) {
            virBufferAsprintf(buf, " limit='%lu'", def->catchup.limit);
        }
        virBufferAddLit(buf, "/>\n    </timer>\n");
    }

    return 0;
}

static void
virDomainGraphicsAuthDefFormatAttr(virBufferPtr buf,
                                   virDomainGraphicsAuthDefPtr def,
                                   unsigned int flags)
{
    if (!def->passwd)
        return;

    if (flags & VIR_DOMAIN_XML_SECURE)
        virBufferEscapeString(buf, " passwd='%s'",
                              def->passwd);

    if (def->expires) {
        char strbuf[100];
        struct tm tmbuf, *tm;
        tm = gmtime_r(&def->validTo, &tmbuf);
        strftime(strbuf, sizeof(strbuf), "%Y-%m-%dT%H:%M:%S", tm);
        virBufferAsprintf(buf, " passwdValidTo='%s'", strbuf);
    }

    if (def->connected)
        virBufferEscapeString(buf, " connected='%s'",
                              virDomainGraphicsAuthConnectedTypeToString(def->connected));
}


static void
virDomainGraphicsListenDefFormat(virBufferPtr buf,
                                 virDomainGraphicsListenDefPtr def,
                                 unsigned int flags)
{
    virBufferAddLit(buf, "      <listen");

    if (def->type) {
        virBufferAsprintf(buf, " type='%s'",
                          virDomainGraphicsListenTypeToString(def->type));
    }

    if (def->address &&
        ((def->type == VIR_DOMAIN_GRAPHICS_LISTEN_TYPE_ADDRESS) ||
         ((def->type == VIR_DOMAIN_GRAPHICS_LISTEN_TYPE_NETWORK) &&
          !(flags & VIR_DOMAIN_XML_INACTIVE)))) {
        /* address may also be set to show current status when type='network',
         * but we don't want to print that if INACTIVE data is requested. */
        virBufferAsprintf(buf, " address='%s'", def->address);
    }

    if (def->network &&
        (def->type == VIR_DOMAIN_GRAPHICS_LISTEN_TYPE_NETWORK)) {
        virBufferEscapeString(buf, " network='%s'", def->network);
    }

    virBufferAddLit(buf, "/>\n");
}


static int
virDomainGraphicsDefFormat(virBufferPtr buf,
                           virDomainGraphicsDefPtr def,
                           unsigned int flags)
{
    const char *type = virDomainGraphicsTypeToString(def->type);
    const char *listenAddr = NULL;
    int children = 0;
    int i;

    if (!type) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                             _("unexpected net type %d"), def->type);
        return -1;
    }

    /* find the first listen element of type='address' and duplicate
    * its address attribute as the listen attribute of
    * <graphics>. This is done to improve backward compatibility. */
    for (i = 0; i < def->nListens; i++) {
        if (virDomainGraphicsListenGetType(def, i)
            == VIR_DOMAIN_GRAPHICS_LISTEN_TYPE_ADDRESS) {
            listenAddr = virDomainGraphicsListenGetAddress(def, i);
            break;
        }
    }

    virBufferAsprintf(buf, "    <graphics type='%s'", type);

    switch (def->type) {
    case VIR_DOMAIN_GRAPHICS_TYPE_VNC:
        if (def->data.vnc.socket) {
            if (def->data.vnc.socket)
                virBufferAsprintf(buf, " socket='%s'",
                                  def->data.vnc.socket);
        } else {
            if (def->data.vnc.port &&
                (!def->data.vnc.autoport || !(flags & VIR_DOMAIN_XML_INACTIVE)))
                virBufferAsprintf(buf, " port='%d'",
                                  def->data.vnc.port);
            else if (def->data.vnc.autoport)
                virBufferAddLit(buf, " port='-1'");

            virBufferAsprintf(buf, " autoport='%s'",
                              def->data.vnc.autoport ? "yes" : "no");

            if (listenAddr)
                virBufferAsprintf(buf, " listen='%s'", listenAddr);
        }

        if (def->data.vnc.keymap)
            virBufferEscapeString(buf, " keymap='%s'",
                                  def->data.vnc.keymap);

        virDomainGraphicsAuthDefFormatAttr(buf, &def->data.vnc.auth, flags);
        break;

    case VIR_DOMAIN_GRAPHICS_TYPE_SDL:
        if (def->data.sdl.display)
            virBufferEscapeString(buf, " display='%s'",
                                  def->data.sdl.display);

        if (def->data.sdl.xauth)
            virBufferEscapeString(buf, " xauth='%s'",
                                  def->data.sdl.xauth);
        if (def->data.sdl.fullscreen)
            virBufferAddLit(buf, " fullscreen='yes'");

        break;

    case VIR_DOMAIN_GRAPHICS_TYPE_RDP:
        if (def->data.rdp.port)
            virBufferAsprintf(buf, " port='%d'",
                              def->data.rdp.port);
        else if (def->data.rdp.autoport)
            virBufferAddLit(buf, " port='0'");

        if (def->data.rdp.autoport)
            virBufferAsprintf(buf, " autoport='yes'");

        if (def->data.rdp.replaceUser)
            virBufferAsprintf(buf, " replaceUser='yes'");

        if (def->data.rdp.multiUser)
            virBufferAsprintf(buf, " multiUser='yes'");

        if (listenAddr)
            virBufferAsprintf(buf, " listen='%s'", listenAddr);

        break;

    case VIR_DOMAIN_GRAPHICS_TYPE_DESKTOP:
        if (def->data.desktop.display)
            virBufferEscapeString(buf, " display='%s'",
                                  def->data.desktop.display);

        if (def->data.desktop.fullscreen)
            virBufferAddLit(buf, " fullscreen='yes'");

        break;

    case VIR_DOMAIN_GRAPHICS_TYPE_SPICE:
        if (def->data.spice.port)
            virBufferAsprintf(buf, " port='%d'",
                              def->data.spice.port);

        if (def->data.spice.tlsPort)
            virBufferAsprintf(buf, " tlsPort='%d'",
                              def->data.spice.tlsPort);

        virBufferAsprintf(buf, " autoport='%s'",
                          def->data.spice.autoport ? "yes" : "no");

        if (listenAddr)
            virBufferAsprintf(buf, " listen='%s'", listenAddr);

        if (def->data.spice.keymap)
            virBufferEscapeString(buf, " keymap='%s'",
                                  def->data.spice.keymap);

        virDomainGraphicsAuthDefFormatAttr(buf, &def->data.spice.auth, flags);
        break;

    }

    for (i = 0; i < def->nListens; i++) {
        if (virDomainGraphicsListenGetType(def, i)
            == VIR_DOMAIN_GRAPHICS_LISTEN_TYPE_NONE)
            continue;
        if (!children) {
            virBufferAddLit(buf, ">\n");
            children = 1;
        }
        virDomainGraphicsListenDefFormat(buf, &def->listens[i], flags);
    }

    if (def->type == VIR_DOMAIN_GRAPHICS_TYPE_SPICE) {
        for (i = 0 ; i < VIR_DOMAIN_GRAPHICS_SPICE_CHANNEL_LAST ; i++) {
            int mode = def->data.spice.channels[i];
            if (mode == VIR_DOMAIN_GRAPHICS_SPICE_CHANNEL_MODE_ANY)
                continue;

            if (!children) {
                virBufferAddLit(buf, ">\n");
                children = 1;
            }

            virBufferAsprintf(buf, "      <channel name='%s' mode='%s'/>\n",
                              virDomainGraphicsSpiceChannelNameTypeToString(i),
                              virDomainGraphicsSpiceChannelModeTypeToString(mode));
        }
        if (!children && (def->data.spice.image || def->data.spice.jpeg ||
                          def->data.spice.zlib || def->data.spice.playback ||
                          def->data.spice.streaming || def->data.spice.copypaste)) {
            virBufferAddLit(buf, ">\n");
            children = 1;
        }
        if (def->data.spice.image)
            virBufferAsprintf(buf, "      <image compression='%s'/>\n",
                              virDomainGraphicsSpiceImageCompressionTypeToString(def->data.spice.image));
        if (def->data.spice.jpeg)
            virBufferAsprintf(buf, "      <jpeg compression='%s'/>\n",
                              virDomainGraphicsSpiceJpegCompressionTypeToString(def->data.spice.jpeg));
        if (def->data.spice.zlib)
            virBufferAsprintf(buf, "      <zlib compression='%s'/>\n",
                              virDomainGraphicsSpiceZlibCompressionTypeToString(def->data.spice.zlib));
        if (def->data.spice.playback)
            virBufferAsprintf(buf, "      <playback compression='%s'/>\n",
                              virDomainGraphicsSpicePlaybackCompressionTypeToString(def->data.spice.playback));
        if (def->data.spice.streaming)
            virBufferAsprintf(buf, "      <streaming mode='%s'/>\n",
                              virDomainGraphicsSpiceStreamingModeTypeToString(def->data.spice.streaming));
        if (def->data.spice.copypaste)
            virBufferAsprintf(buf, "      <clipboard copypaste='%s'/>\n",
                              virDomainGraphicsSpiceClipboardCopypasteTypeToString(def->data.spice.copypaste));
    }

    if (children) {
        virBufferAddLit(buf, "    </graphics>\n");
    } else {
        virBufferAddLit(buf, "/>\n");
    }

    return 0;
}


static int
virDomainHostdevDefFormat(virBufferPtr buf,
                          virDomainHostdevDefPtr def,
                          unsigned int flags)
{
    const char *mode = virDomainHostdevModeTypeToString(def->mode);
    const char *type;

    if (!mode || def->mode != VIR_DOMAIN_HOSTDEV_MODE_SUBSYS) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                             _("unexpected hostdev mode %d"), def->mode);
        return -1;
    }

    type = virDomainHostdevSubsysTypeToString(def->source.subsys.type);
    if (!type || (def->source.subsys.type != VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_USB && def->source.subsys.type != VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_PCI) ) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                             _("unexpected hostdev type %d"),
                             def->source.subsys.type);
        return -1;
    }

    virBufferAsprintf(buf, "    <hostdev mode='%s' type='%s' managed='%s'>\n",
                      mode, type, def->managed ? "yes" : "no");
    virBufferAddLit(buf, "      <source>\n");

    if (def->source.subsys.type == VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_USB) {
        if (def->source.subsys.u.usb.vendor) {
            virBufferAsprintf(buf, "        <vendor id='0x%.4x'/>\n",
                              def->source.subsys.u.usb.vendor);
            virBufferAsprintf(buf, "        <product id='0x%.4x'/>\n",
                              def->source.subsys.u.usb.product);
        }
        if (def->source.subsys.u.usb.bus ||
            def->source.subsys.u.usb.device)
            virBufferAsprintf(buf, "        <address bus='%d' device='%d'/>\n",
                              def->source.subsys.u.usb.bus,
                              def->source.subsys.u.usb.device);
    } else if (def->source.subsys.type == VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_PCI) {
        virBufferAsprintf(buf, "        <address domain='0x%.4x' bus='0x%.2x' slot='0x%.2x' function='0x%.1x'/>\n",
                          def->source.subsys.u.pci.domain,
                          def->source.subsys.u.pci.bus,
                          def->source.subsys.u.pci.slot,
                          def->source.subsys.u.pci.function);
    }

    virBufferAddLit(buf, "      </source>\n");

    if (def->bootIndex)
        virBufferAsprintf(buf, "      <boot order='%d'/>\n", def->bootIndex);

    if (virDomainDeviceInfoFormat(buf, &def->info, flags) < 0)
        return -1;

    if (def->rombar) {
        const char *rombar
            = virDomainPciRombarModeTypeToString(def->rombar);
        if (!rombar) {
            virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                 _("unexpected rom bar value %d"),
                                 def->rombar);
            return -1;
        }
        virBufferAsprintf(buf, "      <rom bar='%s'/>\n", rombar);
    }

    virBufferAddLit(buf, "    </hostdev>\n");

    return 0;
}


#define DUMPXML_FLAGS                           \
    (VIR_DOMAIN_XML_SECURE |                    \
     VIR_DOMAIN_XML_INACTIVE |                  \
     VIR_DOMAIN_XML_UPDATE_CPU)

verify(((VIR_DOMAIN_XML_INTERNAL_STATUS |
         VIR_DOMAIN_XML_INTERNAL_ACTUAL_NET)
        & DUMPXML_FLAGS) == 0);

/* This internal version can accept VIR_DOMAIN_XML_INTERNAL_*,
 * whereas the public version cannot.  Also, it appends to an existing
 * buffer, rather than flattening to string.  Return -1 on failure.  */
static int
virDomainDefFormatInternal(virDomainDefPtr def,
                           unsigned int flags,
                           virBufferPtr buf)
{
    /* XXX Also need to take an indentation parameter - either int or
     * string prefix, so that snapshot xml gets uniform indentation.  */
    unsigned char *uuid;
    char uuidstr[VIR_UUID_STRING_BUFLEN];
    const char *type = NULL;
    int n, allones = 1;

    virCheckFlags(DUMPXML_FLAGS |
                  VIR_DOMAIN_XML_INTERNAL_STATUS |
                  VIR_DOMAIN_XML_INTERNAL_ACTUAL_NET |
                  VIR_DOMAIN_XML_NO_EPHEMERAL_DEVICES,
                  -1);

    if (!(type = virDomainVirtTypeToString(def->virtType))) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                         _("unexpected domain type %d"), def->virtType);
        goto cleanup;
    }

    if (def->id == -1)
        flags |= VIR_DOMAIN_XML_INACTIVE;

    virBufferAsprintf(buf, "<domain type='%s'", type);
    if (!(flags & VIR_DOMAIN_XML_INACTIVE))
        virBufferAsprintf(buf, " id='%d'", def->id);
    if (def->namespaceData && def->ns.href)
        virBufferAsprintf(buf, " %s", (def->ns.href)());
    virBufferAddLit(buf, ">\n");

    virBufferEscapeString(buf, "  <name>%s</name>\n", def->name);

    uuid = def->uuid;
    virUUIDFormat(uuid, uuidstr);
    virBufferAsprintf(buf, "  <uuid>%s</uuid>\n", uuidstr);

    if (def->description)
        virBufferEscapeString(buf, "  <description>%s</description>\n",
                              def->description);

    virBufferAsprintf(buf, "  <memory>%lu</memory>\n", def->mem.max_balloon);
    virBufferAsprintf(buf, "  <currentMemory>%lu</currentMemory>\n",
                      def->mem.cur_balloon);

    /* add blkiotune only if there are any */
    if (def->blkio.weight) {
        virBufferAsprintf(buf, "  <blkiotune>\n");
        virBufferAsprintf(buf, "    <weight>%u</weight>\n",
                          def->blkio.weight);
        virBufferAsprintf(buf, "  </blkiotune>\n");
    }

    /* add memtune only if there are any */
    if (def->mem.hard_limit || def->mem.soft_limit || def->mem.min_guarantee ||
        def->mem.swap_hard_limit)
        virBufferAsprintf(buf, "  <memtune>\n");
    if (def->mem.hard_limit) {
        virBufferAsprintf(buf, "    <hard_limit>%lu</hard_limit>\n",
                          def->mem.hard_limit);
    }
    if (def->mem.soft_limit) {
        virBufferAsprintf(buf, "    <soft_limit>%lu</soft_limit>\n",
                          def->mem.soft_limit);
    }
    if (def->mem.min_guarantee) {
        virBufferAsprintf(buf, "    <min_guarantee>%lu</min_guarantee>\n",
                          def->mem.min_guarantee);
    }
    if (def->mem.swap_hard_limit) {
        virBufferAsprintf(buf, "    <swap_hard_limit>%lu</swap_hard_limit>\n",
                          def->mem.swap_hard_limit);
    }
    if (def->mem.hard_limit || def->mem.soft_limit || def->mem.min_guarantee ||
        def->mem.swap_hard_limit)
        virBufferAsprintf(buf, "  </memtune>\n");

    if (def->mem.hugepage_backed || def->mem.ksm_disabled)
        virBufferAddLit(buf, "  <memoryBacking>\n");

    if (def->mem.hugepage_backed)
        virBufferAddLit(buf, "    <hugepages/>\n");

    if (def->mem.ksm_disabled)
        virBufferAddLit(buf, "    <nosharepages/>\n");

    if (def->mem.hugepage_backed || def->mem.ksm_disabled)
        virBufferAddLit(buf, "  </memoryBacking>\n");

    for (n = 0 ; n < def->cpumasklen ; n++)
        if (def->cpumask[n] != 1)
            allones = 0;

    virBufferAddLit(buf, "  <vcpu");
    if (!allones) {
        char *cpumask = NULL;
        if ((cpumask =
             virDomainCpuSetFormat(def->cpumask, def->cpumasklen)) == NULL)
            goto cleanup;
        virBufferAsprintf(buf, " cpuset='%s'", cpumask);
        VIR_FREE(cpumask);
    }
    if (def->vcpus != def->maxvcpus)
        virBufferAsprintf(buf, " current='%u'", def->vcpus);
    virBufferAsprintf(buf, ">%u</vcpu>\n", def->maxvcpus);

    if (def->cputune.shares || def->cputune.vcpupin ||
        def->cputune.period || def->cputune.quota)
        virBufferAddLit(buf, "  <cputune>\n");

    if (def->cputune.shares)
        virBufferAsprintf(buf, "    <shares>%lu</shares>\n",
                          def->cputune.shares);
    if (def->cputune.period)
        virBufferAsprintf(buf, "    <period>%llu</period>\n",
                          def->cputune.period);
    if (def->cputune.quota)
        virBufferAsprintf(buf, "    <quota>%lld</quota>\n",
                          def->cputune.quota);
    if (def->cputune.vcpupin) {
        int i;
        for (i = 0; i < def->cputune.nvcpupin; i++) {
            virBufferAsprintf(buf, "    <vcpupin vcpu='%u' ",
                              def->cputune.vcpupin[i]->vcpuid);

            char *cpumask = NULL;
            cpumask = virDomainCpuSetFormat(def->cputune.vcpupin[i]->cpumask,
                                            VIR_DOMAIN_CPUMASK_LEN);

            if (cpumask == NULL) {
                virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                     "%s", _("failed to format cpuset for vcpupin"));
                goto cleanup;
            }

            virBufferAsprintf(buf, "cpuset='%s'/>\n", cpumask);
            VIR_FREE(cpumask);
        }
    }

    if (def->cputune.shares || def->cputune.vcpupin ||
        def->cputune.period || def->cputune.quota)
        virBufferAddLit(buf, "  </cputune>\n");

    if (def->numatune.memory.nodemask)
        virBufferAddLit(buf, "  <numatune>\n");

    if (def->numatune.memory.nodemask) {
        char *nodemask = NULL;
        nodemask = virDomainCpuSetFormat(def->numatune.memory.nodemask,
                                         VIR_DOMAIN_CPUMASK_LEN);
        if (nodemask == NULL) {
            virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                 "%s", _("failed to format nodeset for NUMA memory tuning"));
            goto cleanup;
        }

        virBufferAsprintf(buf, "    <memory mode='%s' nodeset='%s'/>\n",
                          virDomainNumatuneMemModeTypeToString(def->numatune.memory.mode),
                          nodemask);
        VIR_FREE(nodemask);
    }

    if (def->numatune.memory.nodemask)
        virBufferAddLit(buf, "  </numatune>\n");

    if (def->sysinfo)
        virDomainSysinfoDefFormat(buf, def->sysinfo);

    if (def->os.bootloader) {
        virBufferEscapeString(buf, "  <bootloader>%s</bootloader>\n",
                              def->os.bootloader);
        if (def->os.bootloaderArgs)
            virBufferEscapeString(buf, "  <bootloader_args>%s</bootloader_args>\n",
                                  def->os.bootloaderArgs);
    }
    virBufferAddLit(buf, "  <os>\n");

    virBufferAddLit(buf, "    <type");
    if (def->os.arch)
        virBufferAsprintf(buf, " arch='%s'", def->os.arch);
    if (def->os.machine)
        virBufferAsprintf(buf, " machine='%s'", def->os.machine);
    /*
     * HACK: For xen driver we previously used bogus 'linux' as the
     * os type for paravirt, whereas capabilities declare it to
     * be 'xen'. So we convert to the former for backcompat
     */
    if (def->virtType == VIR_DOMAIN_VIRT_XEN &&
        STREQ(def->os.type, "xen"))
        virBufferAsprintf(buf, ">%s</type>\n", "linux");
    else
        virBufferAsprintf(buf, ">%s</type>\n", def->os.type);

    if (def->os.init)
        virBufferEscapeString(buf, "    <init>%s</init>\n",
                              def->os.init);
    if (def->os.loader)
        virBufferEscapeString(buf, "    <loader>%s</loader>\n",
                              def->os.loader);
    if (def->os.kernel)
        virBufferEscapeString(buf, "    <kernel>%s</kernel>\n",
                              def->os.kernel);
    if (def->os.initrd)
        virBufferEscapeString(buf, "    <initrd>%s</initrd>\n",
                              def->os.initrd);
    if (def->os.cmdline)
        virBufferEscapeString(buf, "    <cmdline>%s</cmdline>\n",
                              def->os.cmdline);
    if (def->os.root)
        virBufferEscapeString(buf, "    <root>%s</root>\n",
                              def->os.root);

    if (!def->os.bootloader) {
        for (n = 0 ; n < def->os.nBootDevs ; n++) {
            const char *boottype =
                virDomainBootTypeToString(def->os.bootDevs[n]);
            if (!boottype) {
                virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                     _("unexpected boot device type %d"),
                                     def->os.bootDevs[n]);
                goto cleanup;
            }
            virBufferAsprintf(buf, "    <boot dev='%s'/>\n", boottype);
        }

        if (def->os.bootmenu != VIR_DOMAIN_BOOT_MENU_DEFAULT) {
            const char *enabled = (def->os.bootmenu ==
                                   VIR_DOMAIN_BOOT_MENU_ENABLED ? "yes"
                                                                : "no");
            virBufferAsprintf(buf, "    <bootmenu enable='%s'/>\n", enabled);
        }

        if (def->os.bios.useserial) {
            const char *useserial = (def->os.bios.useserial ==
                                     VIR_DOMAIN_BIOS_USESERIAL_YES ? "yes"
                                                                   : "no");
            virBufferAsprintf(buf, "    <bios useserial='%s'/>\n", useserial);
        }
    }

    if (def->os.smbios_mode) {
        const char *mode;

        mode = virDomainSmbiosModeTypeToString(def->os.smbios_mode);
        if (mode == NULL) {
            virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                         _("unexpected smbios mode %d"), def->os.smbios_mode);
            goto cleanup;
        }
        virBufferAsprintf(buf, "    <smbios mode='%s'/>\n", mode);
    }

    virBufferAddLit(buf, "  </os>\n");

    if (def->features) {
        int i;
        virBufferAddLit(buf, "  <features>\n");
        for (i = 0 ; i < VIR_DOMAIN_FEATURE_LAST ; i++) {
            if (def->features & (1 << i)) {
                const char *name = virDomainFeatureTypeToString(i);
                if (!name) {
                    virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                         _("unexpected feature %d"), i);
                    goto cleanup;
                }
                virBufferAsprintf(buf, "    <%s/>\n", name);
            }
        }
        virBufferAddLit(buf, "  </features>\n");
    }

    if (virCPUDefFormatBuf(buf, def->cpu, "  ", 0) < 0)
        goto cleanup;

    virBufferAsprintf(buf, "  <clock offset='%s'",
                      virDomainClockOffsetTypeToString(def->clock.offset));
    switch (def->clock.offset) {
    case VIR_DOMAIN_CLOCK_OFFSET_VARIABLE:
        virBufferAsprintf(buf, " adjustment='%lld'", def->clock.data.adjustment);
        break;
    case VIR_DOMAIN_CLOCK_OFFSET_TIMEZONE:
        virBufferEscapeString(buf, " timezone='%s'", def->clock.data.timezone);
        break;
    }
    if (def->clock.ntimers == 0) {
        virBufferAddLit(buf, "/>\n");
    } else {
        virBufferAddLit(buf, ">\n");
        for (n = 0; n < def->clock.ntimers; n++) {
            if (virDomainTimerDefFormat(buf, def->clock.timers[n]) < 0)
                goto cleanup;
        }
        virBufferAddLit(buf, "  </clock>\n");
    }

    if (virDomainLifecycleDefFormat(buf, def->onPoweroff,
                                    "on_poweroff",
                                    virDomainLifecycleTypeToString) < 0)
        goto cleanup;
    if (virDomainLifecycleDefFormat(buf, def->onReboot,
                                    "on_reboot",
                                    virDomainLifecycleTypeToString) < 0)
        goto cleanup;
    if (virDomainLifecycleDefFormat(buf, def->onCrash,
                                    "on_crash",
                                    virDomainLifecycleCrashTypeToString) < 0)
        goto cleanup;

    virBufferAddLit(buf, "  <devices>\n");

    if (def->emulator)
        virBufferEscapeString(buf, "    <emulator>%s</emulator>\n",
                              def->emulator);

    for (n = 0 ; n < def->ndisks ; n++)
        if (virDomainDiskDefFormat(buf, def->disks[n], flags) < 0)
            goto cleanup;

    for (n = 0 ; n < def->ncontrollers ; n++)
        if (virDomainControllerDefFormat(buf, def->controllers[n], flags) < 0)
            goto cleanup;

    for (n = 0 ; n < def->nleases ; n++)
        if (virDomainLeaseDefFormat(buf, def->leases[n]) < 0)
            goto cleanup;

    for (n = 0 ; n < def->nfss ; n++)
        if (virDomainFSDefFormat(buf, def->fss[n], flags) < 0)
            goto cleanup;


    for (n = 0 ; n < def->nnets ; n++)
        if (virDomainNetDefFormat(buf, def->nets[n], flags) < 0)
            goto cleanup;

    for (n = 0 ; n < def->nsmartcards ; n++)
        if (virDomainSmartcardDefFormat(buf, def->smartcards[n], flags) < 0)
            goto cleanup;

    for (n = 0 ; n < def->nserials ; n++)
        if (virDomainChrDefFormat(buf, def->serials[n], flags) < 0)
            goto cleanup;

    for (n = 0 ; n < def->nparallels ; n++)
        if (virDomainChrDefFormat(buf, def->parallels[n], flags) < 0)
            goto cleanup;

    /* If there's a PV console that's preferred.. */
    if (def->console) {
        if (virDomainChrDefFormat(buf, def->console, flags) < 0)
            goto cleanup;
    } else if (def->nserials != 0) {
        /* ..else for legacy compat duplicate the first serial device as a
         * console */
        virDomainChrDef console;
        memcpy(&console, def->serials[0], sizeof(console));
        console.deviceType = VIR_DOMAIN_CHR_DEVICE_TYPE_CONSOLE;
        if (virDomainChrDefFormat(buf, &console, flags) < 0)
            goto cleanup;
    }

    for (n = 0 ; n < def->nchannels ; n++)
        if (virDomainChrDefFormat(buf, def->channels[n], flags) < 0)
            goto cleanup;

    for (n = 0 ; n < def->ninputs ; n++)
        if (def->inputs[n]->bus == VIR_DOMAIN_INPUT_BUS_USB &&
            virDomainInputDefFormat(buf, def->inputs[n], flags) < 0)
            goto cleanup;

    if (def->ngraphics > 0) {
        /* If graphics is enabled, add the implicit mouse */
        virDomainInputDef autoInput = {
            VIR_DOMAIN_INPUT_TYPE_MOUSE,
            STREQ(def->os.type, "hvm") ?
            VIR_DOMAIN_INPUT_BUS_PS2 : VIR_DOMAIN_INPUT_BUS_XEN,
            { .alias = NULL },
        };

        if (virDomainInputDefFormat(buf, &autoInput, flags) < 0)
            goto cleanup;

        for (n = 0 ; n < def->ngraphics ; n++)
            if (virDomainGraphicsDefFormat(buf, def->graphics[n], flags) < 0)
                goto cleanup;
    }

    for (n = 0 ; n < def->nsounds ; n++)
        if (virDomainSoundDefFormat(buf, def->sounds[n], flags) < 0)
            goto cleanup;

    for (n = 0 ; n < def->nvideos ; n++)
        if (virDomainVideoDefFormat(buf, def->videos[n], flags) < 0)
            goto cleanup;

    for (n = 0 ; n < def->nhostdevs ; n++) {
        if ((flags & VIR_DOMAIN_XML_NO_EPHEMERAL_DEVICES) != 0 &&
            def->hostdevs[n]->ephemeral)
            continue;
        if (virDomainHostdevDefFormat(buf, def->hostdevs[n], flags) < 0)
            goto cleanup;
    }

    if (def->watchdog)
        virDomainWatchdogDefFormat (buf, def->watchdog, flags);

    if (def->memballoon)
        virDomainMemballoonDefFormat (buf, def->memballoon, flags);

    virBufferAddLit(buf, "  </devices>\n");

    if (def->seclabel.model) {
        const char *sectype = virDomainSeclabelTypeToString(def->seclabel.type);
        if (!sectype)
            goto cleanup;

        if (def->seclabel.type == VIR_DOMAIN_SECLABEL_DYNAMIC &&
            !def->seclabel.baselabel &&
            (flags & VIR_DOMAIN_XML_INACTIVE)) {
            /* This is the default for inactive xml, so nothing to output.  */
        } else {
            virBufferAsprintf(buf, "  <seclabel type='%s' model='%s' relabel='%s'>\n",
                              sectype, def->seclabel.model,
                              def->seclabel.norelabel ? "no" : "yes");
            if (def->seclabel.label)
                virBufferEscapeString(buf, "    <label>%s</label>\n",
                                      def->seclabel.label);
            if (!def->seclabel.norelabel && def->seclabel.imagelabel)
                virBufferEscapeString(buf, "    <imagelabel>%s</imagelabel>\n",
                                      def->seclabel.imagelabel);
            if (def->seclabel.baselabel &&
                (def->seclabel.type == VIR_DOMAIN_SECLABEL_DYNAMIC))
                virBufferEscapeString(buf, "    <baselabel>%s</baselabel>\n",
                                      def->seclabel.baselabel);
            virBufferAddLit(buf, "  </seclabel>\n");
        }
    }

    if (def->namespaceData && def->ns.format) {
        if ((def->ns.format)(buf, def->namespaceData) < 0)
            goto cleanup;
    }

    virBufferAddLit(buf, "</domain>\n");

    if (virBufferError(buf))
        goto no_memory;

    return 0;

 no_memory:
    virReportOOMError();
 cleanup:
    virBufferFreeAndReset(buf);
    return -1;
}

char *
virDomainDefFormat(virDomainDefPtr def, unsigned int flags)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;

    virCheckFlags((DUMPXML_FLAGS |  VIR_DOMAIN_XML_NO_EPHEMERAL_DEVICES), 
                  NULL);
    
    if (!(flags & VIR_DOMAIN_XML_NO_EPHEMERAL_DEVICES))
        flags |= VIR_DOMAIN_XML_INTERNAL_ACTUAL_NET;
    if (virDomainDefFormatInternal(def, flags, &buf) < 0)
        return NULL;

    return virBufferContentAndReset(&buf);
}


static char *virDomainObjFormat(virCapsPtr caps,
                                virDomainObjPtr obj,
                                unsigned int flags)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    int state;
    int reason;
    int i;

    state = virDomainObjGetState(obj, &reason);
    virBufferAsprintf(&buf, "<domstatus state='%s' reason='%s' pid='%d'>\n",
                      virDomainStateTypeToString(state),
                      virDomainStateReasonToString(state, reason),
                      obj->pid);

    for (i = 0 ; i < VIR_DOMAIN_TAINT_LAST ; i++) {
        if (obj->taint & (1 << i))
            virBufferAsprintf(&buf, "  <taint flag='%s'/>\n",
                              virDomainTaintTypeToString(i));
    }

    if (caps->privateDataXMLFormat &&
        ((caps->privateDataXMLFormat)(&buf, obj->privateData)) < 0)
        goto error;

    if (virDomainDefFormatInternal(obj->def, flags, &buf) < 0)
        goto error;

    virBufferAddLit(&buf, "</domstatus>\n");

    if (virBufferError(&buf))
        goto no_memory;

    return virBufferContentAndReset(&buf);

no_memory:
    virReportOOMError();
error:
    virBufferFreeAndReset(&buf);
    return NULL;
}

int virDomainSaveXML(const char *configDir,
                     virDomainDefPtr def,
                     const char *xml)
{
    char *configFile = NULL;
    int fd = -1, ret = -1;
    size_t towrite;

    if ((configFile = virDomainConfigFile(configDir, def->name)) == NULL)
        goto cleanup;

    if (virFileMakePath(configDir) < 0) {
        virReportSystemError(errno,
                             _("cannot create config directory '%s'"),
                             configDir);
        goto cleanup;
    }

    if ((fd = open(configFile,
                   O_WRONLY | O_CREAT | O_TRUNC,
                   S_IRUSR | S_IWUSR )) < 0) {
        virReportSystemError(errno,
                             _("cannot create config file '%s'"),
                             configFile);
        goto cleanup;
    }

    virEmitXMLWarning(fd, def->name, "edit");

    towrite = strlen(xml);
    if (safewrite(fd, xml, towrite) < 0) {
        virReportSystemError(errno,
                             _("cannot write config file '%s'"),
                             configFile);
        goto cleanup;
    }

    if (VIR_CLOSE(fd) < 0) {
        virReportSystemError(errno,
                             _("cannot save config file '%s'"),
                             configFile);
        goto cleanup;
    }

    ret = 0;
 cleanup:
    VIR_FORCE_CLOSE(fd);

    VIR_FREE(configFile);
    return ret;
}

int virDomainSaveConfig(const char *configDir,
                        virDomainDefPtr def)
{
    int ret = -1;
    char *xml;

    if (!(xml = virDomainDefFormat(def,
                                   VIR_DOMAIN_XML_WRITE_FLAGS)))
        goto cleanup;

    if (virDomainSaveXML(configDir, def, xml))
        goto cleanup;

    ret = 0;
cleanup:
    VIR_FREE(xml);
    return ret;
}

int virDomainSaveStatus(virCapsPtr caps,
                        const char *statusDir,
                        virDomainObjPtr obj)
{
    unsigned int flags = (VIR_DOMAIN_XML_SECURE |
                          VIR_DOMAIN_XML_INTERNAL_STATUS |
                          VIR_DOMAIN_XML_INTERNAL_ACTUAL_NET);

    int ret = -1;
    char *xml;

    if (!(xml = virDomainObjFormat(caps, obj, flags)))
        goto cleanup;

    if (virDomainSaveXML(statusDir, obj->def, xml))
        goto cleanup;

    ret = 0;
cleanup:
    VIR_FREE(xml);
    return ret;
}


static virDomainObjPtr virDomainLoadConfig(virCapsPtr caps,
                                           virDomainObjListPtr doms,
                                           const char *configDir,
                                           const char *autostartDir,
                                           const char *name,
                                           unsigned int expectedVirtTypes,
                                           virDomainLoadConfigNotify notify,
                                           void *opaque)
{
    char *configFile = NULL, *autostartLink = NULL;
    virDomainDefPtr def = NULL;
    virDomainObjPtr dom;
    int autostart;
    int newVM = 1;

    if ((configFile = virDomainConfigFile(configDir, name)) == NULL)
        goto error;
    if (!(def = virDomainDefParseFile(caps, configFile, expectedVirtTypes,
                                      VIR_DOMAIN_XML_INACTIVE)))
        goto error;

    if ((autostartLink = virDomainConfigFile(autostartDir, name)) == NULL)
        goto error;

    if ((autostart = virFileLinkPointsTo(autostartLink, configFile)) < 0)
        goto error;

    /* if the domain is already in our hashtable, we only need to
     * update the autostart flag
     */
    if ((dom = virDomainFindByUUID(doms, def->uuid))) {
        dom->autostart = autostart;

        if (virDomainObjIsActive(dom) &&
            !dom->newDef) {
            virDomainObjAssignDef(dom, def, false);
        } else {
            virDomainDefFree(def);
        }

        VIR_FREE(configFile);
        VIR_FREE(autostartLink);
        return dom;
    }

    if (!(dom = virDomainAssignDef(caps, doms, def, false)))
        goto error;

    dom->autostart = autostart;

    if (notify)
        (*notify)(dom, newVM, opaque);

    VIR_FREE(configFile);
    VIR_FREE(autostartLink);
    return dom;

error:
    VIR_FREE(configFile);
    VIR_FREE(autostartLink);
    virDomainDefFree(def);
    return NULL;
}

static virDomainObjPtr virDomainLoadStatus(virCapsPtr caps,
                                           virDomainObjListPtr doms,
                                           const char *statusDir,
                                           const char *name,
                                           unsigned int expectedVirtTypes,
                                           virDomainLoadConfigNotify notify,
                                           void *opaque)
{
    char *statusFile = NULL;
    virDomainObjPtr obj = NULL;
    char uuidstr[VIR_UUID_STRING_BUFLEN];

    if ((statusFile = virDomainConfigFile(statusDir, name)) == NULL)
        goto error;

    if (!(obj = virDomainObjParseFile(caps, statusFile, expectedVirtTypes,
                                      VIR_DOMAIN_XML_INTERNAL_STATUS |
                                      VIR_DOMAIN_XML_INTERNAL_ACTUAL_NET)))
        goto error;

    virUUIDFormat(obj->def->uuid, uuidstr);

    if (virHashLookup(doms->objs, uuidstr) != NULL) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                             _("unexpected domain %s already exists"),
                             obj->def->name);
        goto error;
    }

    if (virHashAddEntry(doms->objs, uuidstr, obj) < 0)
        goto error;

    if (notify)
        (*notify)(obj, 1, opaque);

    VIR_FREE(statusFile);
    return obj;

error:
    /* obj was never shared, so unref should return 0 */
    if (obj)
        ignore_value(virDomainObjUnref(obj));
    VIR_FREE(statusFile);
    return NULL;
}

int virDomainLoadAllConfigs(virCapsPtr caps,
                            virDomainObjListPtr doms,
                            const char *configDir,
                            const char *autostartDir,
                            int liveStatus,
                            unsigned int expectedVirtTypes,
                            virDomainLoadConfigNotify notify,
                            void *opaque)
{
    DIR *dir;
    struct dirent *entry;

    VIR_INFO("Scanning for configs in %s", configDir);

    if (!(dir = opendir(configDir))) {
        if (errno == ENOENT)
            return 0;
        virReportSystemError(errno,
                             _("Failed to open dir '%s'"),
                             configDir);
        return -1;
    }

    while ((entry = readdir(dir))) {
        virDomainObjPtr dom;

        if (entry->d_name[0] == '.')
            continue;

        if (!virFileStripSuffix(entry->d_name, ".xml"))
            continue;

        /* NB: ignoring errors, so one malformed config doesn't
           kill the whole process */
        VIR_INFO("Loading config file '%s.xml'", entry->d_name);
        if (liveStatus)
            dom = virDomainLoadStatus(caps,
                                      doms,
                                      configDir,
                                      entry->d_name,
                                      expectedVirtTypes,
                                      notify,
                                      opaque);
        else
            dom = virDomainLoadConfig(caps,
                                      doms,
                                      configDir,
                                      autostartDir,
                                      entry->d_name,
                                      expectedVirtTypes,
                                      notify,
                                      opaque);
        if (dom) {
            virDomainObjUnlock(dom);
            if (!liveStatus)
                dom->persistent = 1;
        }
    }

    closedir(dir);

    return 0;
}

int virDomainDeleteConfig(const char *configDir,
                          const char *autostartDir,
                          virDomainObjPtr dom)
{
    char *configFile = NULL, *autostartLink = NULL;
    int ret = -1;

    if ((configFile = virDomainConfigFile(configDir, dom->def->name)) == NULL)
        goto cleanup;
    if ((autostartLink = virDomainConfigFile(autostartDir, dom->def->name)) == NULL)
        goto cleanup;

    /* Not fatal if this doesn't work */
    unlink(autostartLink);

    if (unlink(configFile) < 0 &&
        errno != ENOENT) {
        virReportSystemError(errno,
                             _("cannot remove config %s"),
                             configFile);
        goto cleanup;
    }

    ret = 0;

cleanup:
    VIR_FREE(configFile);
    VIR_FREE(autostartLink);
    return ret;
}

char *virDomainConfigFile(const char *dir,
                          const char *name)
{
    char *ret = NULL;

    if (virAsprintf(&ret, "%s/%s.xml", dir, name) < 0) {
        virReportOOMError();
        return NULL;
    }

    return ret;
}

/* Translates a device name of the form (regex) "[fhv]d[a-z]+" into
 * the corresponding bus,index combination (e.g. sda => (0,0), sdi (1,1),
 *                                               hdd => (1,1), vdaa => (0,26))
 * @param disk The disk device
 * @param busIdx parsed bus number
 * @param devIdx parsed device number
 * @return 0 on success, -1 on failure
 */
int virDiskNameToBusDeviceIndex(const virDomainDiskDefPtr disk,
                                int *busIdx,
                                int *devIdx) {

    int idx = virDiskNameToIndex(disk->dst);
    if (idx < 0)
        return -1;

    switch (disk->bus) {
        case VIR_DOMAIN_DISK_BUS_IDE:
            *busIdx = idx / 2;
            *devIdx = idx % 2;
            break;
        case VIR_DOMAIN_DISK_BUS_SCSI:
            *busIdx = idx / 7;
            *devIdx = idx % 7;
            break;
        case VIR_DOMAIN_DISK_BUS_FDC:
        case VIR_DOMAIN_DISK_BUS_USB:
        case VIR_DOMAIN_DISK_BUS_VIRTIO:
        case VIR_DOMAIN_DISK_BUS_XEN:
        default:
            *busIdx = 0;
            *devIdx = idx;
            break;
    }

    return 0;
}

virDomainFSDefPtr virDomainGetRootFilesystem(virDomainDefPtr def)
{
    int i;

    for (i = 0 ; i < def->nfss ; i++) {
        if (def->fss[i]->type != VIR_DOMAIN_FS_TYPE_MOUNT)
            continue;

        if (STREQ(def->fss[i]->dst, "/"))
            return def->fss[i];
    }

    return NULL;
}

/*
 * virDomainObjIsDuplicate:
 * @doms : virDomainObjListPtr to search
 * @def  : virDomainDefPtr definition of domain to lookup
 * @check_active: If true, ensure that domain is not active
 *
 * Returns: -1 on error
 *          0 if domain is new
 *          1 if domain is a duplicate
 */
int
virDomainObjIsDuplicate(virDomainObjListPtr doms,
                        virDomainDefPtr def,
                        unsigned int check_active)
{
    int ret = -1;
    int dupVM = 0;
    virDomainObjPtr vm = NULL;

    /* See if a VM with matching UUID already exists */
    vm = virDomainFindByUUID(doms, def->uuid);
    if (vm) {
        /* UUID matches, but if names don't match, refuse it */
        if (STRNEQ(vm->def->name, def->name)) {
            char uuidstr[VIR_UUID_STRING_BUFLEN];
            virUUIDFormat(vm->def->uuid, uuidstr);
            virDomainReportError(VIR_ERR_OPERATION_FAILED,
                                 _("domain '%s' is already defined with uuid %s"),
                                 vm->def->name, uuidstr);
            goto cleanup;
        }

        if (check_active) {
            /* UUID & name match, but if VM is already active, refuse it */
            if (virDomainObjIsActive(vm)) {
                virDomainReportError(VIR_ERR_OPERATION_INVALID,
                                     _("domain is already active as '%s'"),
                                     vm->def->name);
                goto cleanup;
            }
        }

        dupVM = 1;
    } else {
        /* UUID does not match, but if a name matches, refuse it */
        vm = virDomainFindByName(doms, def->name);
        if (vm) {
            char uuidstr[VIR_UUID_STRING_BUFLEN];
            virUUIDFormat(vm->def->uuid, uuidstr);
            virDomainReportError(VIR_ERR_OPERATION_FAILED,
                                 _("domain '%s' already exists with uuid %s"),
                                 def->name, uuidstr);
            goto cleanup;
        }
    }

    ret = dupVM;
cleanup:
    if (vm)
        virDomainObjUnlock(vm);
    return ret;
}


void virDomainObjLock(virDomainObjPtr obj)
{
    virMutexLock(&obj->lock);
}

void virDomainObjUnlock(virDomainObjPtr obj)
{
    virMutexUnlock(&obj->lock);
}


static void virDomainObjListCountActive(void *payload, const void *name ATTRIBUTE_UNUSED, void *data)
{
    virDomainObjPtr obj = payload;
    int *count = data;
    virDomainObjLock(obj);
    if (virDomainObjIsActive(obj))
        (*count)++;
    virDomainObjUnlock(obj);
}

static void virDomainObjListCountInactive(void *payload, const void *name ATTRIBUTE_UNUSED, void *data)
{
    virDomainObjPtr obj = payload;
    int *count = data;
    virDomainObjLock(obj);
    if (!virDomainObjIsActive(obj))
        (*count)++;
    virDomainObjUnlock(obj);
}

int virDomainObjListNumOfDomains(virDomainObjListPtr doms, int active)
{
    int count = 0;
    if (active)
        virHashForEach(doms->objs, virDomainObjListCountActive, &count);
    else
        virHashForEach(doms->objs, virDomainObjListCountInactive, &count);
    return count;
}

struct virDomainIDData {
    int numids;
    int maxids;
    int *ids;
};

static void virDomainObjListCopyActiveIDs(void *payload, const void *name ATTRIBUTE_UNUSED, void *opaque)
{
    virDomainObjPtr obj = payload;
    struct virDomainIDData *data = opaque;
    virDomainObjLock(obj);
    if (virDomainObjIsActive(obj) && data->numids < data->maxids)
        data->ids[data->numids++] = obj->def->id;
    virDomainObjUnlock(obj);
}

int virDomainObjListGetActiveIDs(virDomainObjListPtr doms,
                                 int *ids,
                                 int maxids)
{
    struct virDomainIDData data = { 0, maxids, ids };
    virHashForEach(doms->objs, virDomainObjListCopyActiveIDs, &data);
    return data.numids;
}

struct virDomainNameData {
    int oom;
    int numnames;
    int maxnames;
    char **const names;
};

static void virDomainObjListCopyInactiveNames(void *payload, const void *name ATTRIBUTE_UNUSED, void *opaque)
{
    virDomainObjPtr obj = payload;
    struct virDomainNameData *data = opaque;

    if (data->oom)
        return;

    virDomainObjLock(obj);
    if (!virDomainObjIsActive(obj) && data->numnames < data->maxnames) {
        if (!(data->names[data->numnames] = strdup(obj->def->name)))
            data->oom = 1;
        else
            data->numnames++;
    }
    virDomainObjUnlock(obj);
}


int virDomainObjListGetInactiveNames(virDomainObjListPtr doms,
                                     char **const names,
                                     int maxnames)
{
    struct virDomainNameData data = { 0, 0, maxnames, names };
    int i;
    virHashForEach(doms->objs, virDomainObjListCopyInactiveNames, &data);
    if (data.oom) {
        virReportOOMError();
        goto cleanup;
    }

    return data.numnames;

cleanup:
    for (i = 0 ; i < data.numnames ; i++)
        VIR_FREE(data.names[i]);
    return -1;
}

/* Snapshot Def functions */
static void
virDomainSnapshotDiskDefClear(virDomainSnapshotDiskDefPtr disk)
{
    VIR_FREE(disk->name);
    VIR_FREE(disk->file);
    VIR_FREE(disk->driverType);
}

void virDomainSnapshotDefFree(virDomainSnapshotDefPtr def)
{
    int i;

    if (!def)
        return;

    VIR_FREE(def->name);
    VIR_FREE(def->description);
    VIR_FREE(def->parent);
    for (i = 0; i < def->ndisks; i++)
        virDomainSnapshotDiskDefClear(&def->disks[i]);
    VIR_FREE(def->disks);
    virDomainDefFree(def->dom);
    VIR_FREE(def);
}

static int
virDomainSnapshotDiskDefParseXML(xmlNodePtr node,
                                 virDomainSnapshotDiskDefPtr def)
{
    int ret = -1;
    char *snapshot = NULL;
    xmlNodePtr cur;

    def->name = virXMLPropString(node, "name");
    if (!def->name) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                             _("missing name from disk snapshot element"));
        goto cleanup;
    }

    snapshot = virXMLPropString(node, "snapshot");
    if (snapshot) {
        def->snapshot = virDomainDiskSnapshotTypeFromString(snapshot);
        if (def->snapshot <= 0) {
            virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                 _("unknown disk snapshot setting '%s'"),
                                 snapshot);
            goto cleanup;
        }
    }

    cur = node->children;
    while (cur) {
        if (cur->type == XML_ELEMENT_NODE) {
            if (!def->file &&
                xmlStrEqual(cur->name, BAD_CAST "source")) {
                def->file = virXMLPropString(cur, "file");
            } else if (!def->driverType &&
                       xmlStrEqual(cur->name, BAD_CAST "driver")) {
                def->driverType = virXMLPropString(cur, "type");
            }
        }
        cur = cur->next;
    }

    if (!def->snapshot && (def->file || def->driverType))
        def->snapshot = VIR_DOMAIN_DISK_SNAPSHOT_EXTERNAL;

    ret = 0;
cleanup:
    VIR_FREE(snapshot);
    if (ret < 0)
        virDomainSnapshotDiskDefClear(def);
    return ret;
}

/* flags is bitwise-or of virDomainSnapshotParseFlags.
 * If flags does not include VIR_DOMAIN_SNAPSHOT_PARSE_REDEFINE, then
 * caps and expectedVirtTypes are ignored.
 */
virDomainSnapshotDefPtr
virDomainSnapshotDefParseString(const char *xmlStr,
                                virCapsPtr caps,
                                unsigned int expectedVirtTypes,
                                unsigned int flags)
{
    xmlXPathContextPtr ctxt = NULL;
    xmlDocPtr xml = NULL;
    virDomainSnapshotDefPtr def = NULL;
    virDomainSnapshotDefPtr ret = NULL;
    xmlNodePtr *nodes = NULL;
    int i;
    char *creation = NULL, *state = NULL;
    struct timeval tv;
    int active;
    char *tmp;

    xml = virXMLParseCtxt(NULL, xmlStr, "domainsnapshot.xml", &ctxt);
    if (!xml) {
        return NULL;
    }

    if (VIR_ALLOC(def) < 0) {
        virReportOOMError();
        goto cleanup;
    }

    if (!xmlStrEqual(ctxt->node->name, BAD_CAST "domainsnapshot")) {
        virDomainReportError(VIR_ERR_XML_ERROR, "%s", _("domainsnapshot"));
        goto cleanup;
    }

    gettimeofday(&tv, NULL);

    def->name = virXPathString("string(./name)", ctxt);
    if (def->name == NULL) {
        if (flags & VIR_DOMAIN_SNAPSHOT_PARSE_REDEFINE) {
            virDomainReportError(VIR_ERR_XML_ERROR, "%s",
                                 _("a redefined snapshot must have a name"));
            goto cleanup;
        } else {
            ignore_value(virAsprintf(&def->name, "%lld",
                                     (long long)tv.tv_sec));
        }
    }

    if (def->name == NULL) {
        virReportOOMError();
        goto cleanup;
    }

    def->description = virXPathString("string(./description)", ctxt);

    if (flags & VIR_DOMAIN_SNAPSHOT_PARSE_REDEFINE) {
        if (virXPathLongLong("string(./creationTime)", ctxt,
                             &def->creationTime) < 0) {
            virDomainReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                                 _("missing creationTime from existing snapshot"));
            goto cleanup;
        }

        def->parent = virXPathString("string(./parent/name)", ctxt);

        state = virXPathString("string(./state)", ctxt);
        if (state == NULL) {
            /* there was no state in an existing snapshot; this
             * should never happen
             */
            virDomainReportError(VIR_ERR_XML_ERROR, "%s",
                                 _("missing state from existing snapshot"));
            goto cleanup;
        }
        def->state = virDomainSnapshotStateTypeFromString(state);
        if (def->state < 0) {
            virDomainReportError(VIR_ERR_XML_ERROR,
                                 _("Invalid state '%s' in domain snapshot XML"),
                                 state);
            goto cleanup;
        }

        /* Older snapshots were created with just <domain>/<uuid>, and
         * lack domain/@type.  In that case, leave dom NULL, and
         * clients will have to decide between best effort
         * initialization or outright failure.  */
        if ((tmp = virXPathString("string(./domain/@type)", ctxt))) {
            xmlNodePtr domainNode = virXPathNode("./domain", ctxt);

            VIR_FREE(tmp);
            if (!domainNode) {
                virDomainReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                                     _("missing domain in snapshot"));
                goto cleanup;
            }
            def->dom = virDomainDefParseNode(caps, xml, domainNode,
                                             expectedVirtTypes,
                                             (VIR_DOMAIN_XML_INACTIVE |
                                              VIR_DOMAIN_XML_SECURE));
            if (!def->dom)
                goto cleanup;
        } else {
            VIR_WARN("parsing older snapshot that lacks domain");
        }
    } else {
        def->creationTime = tv.tv_sec;
    }

    if ((i = virXPathNodeSet("./disks/*", ctxt, &nodes)) < 0)
        goto cleanup;
    if (flags & VIR_DOMAIN_SNAPSHOT_PARSE_DISKS ||
        (flags & VIR_DOMAIN_SNAPSHOT_PARSE_REDEFINE &&
         def->state == VIR_DOMAIN_DISK_SNAPSHOT)) {
        def->ndisks = i;
        if (def->ndisks && VIR_ALLOC_N(def->disks, def->ndisks) < 0) {
            virReportOOMError();
            goto cleanup;
        }
        for (i = 0; i < def->ndisks; i++) {
            if (virDomainSnapshotDiskDefParseXML(nodes[i], &def->disks[i]) < 0)
                goto cleanup;
        }
        VIR_FREE(nodes);
    } else if (i) {
        virDomainReportError(VIR_ERR_ARGUMENT_UNSUPPORTED, "%s",
                             _("unable to handle disk requests in snapshot"));
        goto cleanup;
    }

    if (flags & VIR_DOMAIN_SNAPSHOT_PARSE_INTERNAL) {
        if (virXPathInt("string(./active)", ctxt, &active) < 0) {
            virDomainReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                                 _("Could not find 'active' element"));
            goto cleanup;
        }
        def->current = active != 0;
    }

    ret = def;

cleanup:
    VIR_FREE(creation);
    VIR_FREE(state);
    VIR_FREE(nodes);
    xmlXPathFreeContext(ctxt);
    if (ret == NULL)
        virDomainSnapshotDefFree(def);
    xmlFreeDoc(xml);

    return ret;
}

static int
disksorter(const void *a, const void *b)
{
    const virDomainSnapshotDiskDef *diska = a;
    const virDomainSnapshotDiskDef *diskb = b;

    /* Integer overflow shouldn't be a problem here.  */
    return diska->index - diskb->index;
}

/* Align def->disks to def->domain.  Sort the list of def->disks,
 * filling in any missing disks or snapshot state defaults given by
 * the domain, with a fallback to a passed in default.  Convert paths
 * to disk targets for uniformity.  Issue an error and return -1 if
 * any def->disks[n]->name appears more than once or does not map to
 * dom->disks.  If require_match, also require that existing
 * def->disks snapshot states do not override explicit def->dom
 * settings.  */
int
virDomainSnapshotAlignDisks(virDomainSnapshotDefPtr def,
                            int default_snapshot,
                            bool require_match)
{
    int ret = -1;
    virBitmapPtr map = NULL;
    int i;
    int ndisks;
    bool inuse;

    if (!def->dom) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                             _("missing domain in snapshot"));
        goto cleanup;
    }

    if (def->ndisks > def->dom->ndisks) {
        virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                             _("too many disk snapshot requests for domain"));
        goto cleanup;
    }

    /* Unlikely to have a guest without disks but technically possible.  */
    if (!def->dom->ndisks) {
        ret = 0;
        goto cleanup;
    }

    if (!(map = virBitmapAlloc(def->dom->ndisks))) {
        virReportOOMError();
        goto cleanup;
    }

    /* Double check requested disks.  */
    for (i = 0; i < def->ndisks; i++) {
        virDomainSnapshotDiskDefPtr disk = &def->disks[i];
        int idx = virDomainDiskIndexByName(def->dom, disk->name, false);
        int disk_snapshot;

        if (idx < 0) {
            virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                                 _("no disk named '%s'"), disk->name);
            goto cleanup;
        }
        disk_snapshot = def->dom->disks[idx]->snapshot;

        if (virBitmapGetBit(map, idx, &inuse) < 0 || inuse) {
            virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                                 _("disk '%s' specified twice"),
                                 disk->name);
            goto cleanup;
        }
        ignore_value(virBitmapSetBit(map, idx));
        disk->index = idx;
        if (!disk_snapshot)
            disk_snapshot = default_snapshot;
        if (!disk->snapshot) {
            disk->snapshot = disk_snapshot;
        } else if (disk_snapshot && require_match &&
                   disk->snapshot != disk_snapshot) {
            const char *tmp = virDomainDiskSnapshotTypeToString(disk_snapshot);
            virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                                 _("disk '%s' must use snapshot mode '%s'"),
                                 disk->name, tmp);
            goto cleanup;
        }
        if (disk->file &&
            disk->snapshot != VIR_DOMAIN_DISK_SNAPSHOT_EXTERNAL) {
            virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                                 _("file '%s' for disk '%s' requires "
                                   "use of external snapshot mode"),
                                 disk->file, disk->name);
            goto cleanup;
        }
        if (STRNEQ(disk->name, def->dom->disks[idx]->dst)) {
            VIR_FREE(disk->name);
            if (!(disk->name = strdup(def->dom->disks[idx]->dst))) {
                virReportOOMError();
                goto cleanup;
            }
        }
    }

    /* Provide defaults for all remaining disks.  */
    ndisks = def->ndisks;
    if (VIR_EXPAND_N(def->disks, def->ndisks,
                     def->dom->ndisks - def->ndisks) < 0) {
        virReportOOMError();
        goto cleanup;
    }

    for (i = 0; i < def->dom->ndisks; i++) {
        virDomainSnapshotDiskDefPtr disk;

        ignore_value(virBitmapGetBit(map, i, &inuse));
        if (inuse)
            continue;
        disk = &def->disks[ndisks++];
        if (!(disk->name = strdup(def->dom->disks[i]->dst))) {
            virReportOOMError();
            goto cleanup;
        }
        disk->index = i;
        disk->snapshot = def->dom->disks[i]->snapshot;
        if (!disk->snapshot)
            disk->snapshot = default_snapshot;
    }

    qsort(&def->disks[0], def->ndisks, sizeof(def->disks[0]), disksorter);

    /* Generate any default external file names.  */
    for (i = 0; i < def->ndisks; i++) {
        virDomainSnapshotDiskDefPtr disk = &def->disks[i];

        if (disk->snapshot == VIR_DOMAIN_DISK_SNAPSHOT_EXTERNAL &&
            !disk->file) {
            const char *original = def->dom->disks[i]->src;
            const char *tmp;

            if (!original) {
                virDomainReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                                     _("cannot generate external backup name "
                                       "for disk '%s' without source"),
                                     disk->name);
                goto cleanup;
            }
            tmp = strrchr(original, '.');
            if (!tmp || strchr(tmp, '/')) {
                ignore_value(virAsprintf(&disk->file, "%s.%s",
                                         original, def->name));
            } else {
                if ((tmp - original) > INT_MAX) {
                    virDomainReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                                         _("integer overflow"));
                    goto cleanup;
                }
                ignore_value(virAsprintf(&disk->file, "%.*s.%s",
                                         (int) (tmp - original), original,
                                         def->name));
            }
            if (!disk->file) {
                virReportOOMError();
                goto cleanup;
            }
        }
    }

    ret = 0;

cleanup:
    virBitmapFree(map);
    return ret;
}

char *virDomainSnapshotDefFormat(char *domain_uuid,
                                 virDomainSnapshotDefPtr def,
                                 unsigned int flags,
                                 int internal)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    int i;

    virCheckFlags(VIR_DOMAIN_XML_SECURE, NULL);

    flags |= VIR_DOMAIN_XML_INACTIVE;

    virBufferAddLit(&buf, "<domainsnapshot>\n");
    virBufferEscapeString(&buf, "  <name>%s</name>\n", def->name);
    if (def->description)
        virBufferEscapeString(&buf, "  <description>%s</description>\n",
                              def->description);
    virBufferAsprintf(&buf, "  <state>%s</state>\n",
                      virDomainSnapshotStateTypeToString(def->state));
    if (def->parent) {
        virBufferAddLit(&buf, "  <parent>\n");
        virBufferEscapeString(&buf, "    <name>%s</name>\n", def->parent);
        virBufferAddLit(&buf, "  </parent>\n");
    }
    virBufferAsprintf(&buf, "  <creationTime>%lld</creationTime>\n",
                      def->creationTime);
    /* For now, only output <disks> on disk-snapshot */
    if (def->state == VIR_DOMAIN_DISK_SNAPSHOT) {
        virBufferAddLit(&buf, "  <disks>\n");
        for (i = 0; i < def->ndisks; i++) {
            virDomainSnapshotDiskDefPtr disk = &def->disks[i];

            if (!disk->name)
                continue;

            virBufferEscapeString(&buf, "    <disk name='%s'", disk->name);
            if (disk->snapshot)
                virBufferAsprintf(&buf, " snapshot='%s'",
                                  virDomainDiskSnapshotTypeToString(disk->snapshot));
            if (disk->file || disk->driverType) {
                virBufferAddLit(&buf, ">\n");
                if (disk->file)
                    virBufferEscapeString(&buf, "      <source file='%s'/>\n",
                                          disk->file);
                if (disk->driverType)
                    virBufferEscapeString(&buf, "      <driver type='%s'/>\n",
                                          disk->driverType);
                virBufferAddLit(&buf, "    </disk>\n");
            } else {
                virBufferAddLit(&buf, "/>\n");
            }
        }
        virBufferAddLit(&buf, "  </disks>\n");
    }
    if (def->dom) {
        virDomainDefFormatInternal(def->dom, flags, &buf);
    } else {
        virBufferAddLit(&buf, "  <domain>\n");
        virBufferAsprintf(&buf, "    <uuid>%s</uuid>\n", domain_uuid);
        virBufferAddLit(&buf, "  </domain>\n");
    }
    if (internal)
        virBufferAsprintf(&buf, "  <active>%d</active>\n", def->current);
    virBufferAddLit(&buf, "</domainsnapshot>\n");

    if (virBufferError(&buf)) {
        virBufferFreeAndReset(&buf);
        virReportOOMError();
        return NULL;
    }

    return virBufferContentAndReset(&buf);
}

/* Snapshot Obj functions */
static virDomainSnapshotObjPtr virDomainSnapshotObjNew(void)
{
    virDomainSnapshotObjPtr snapshot;

    if (VIR_ALLOC(snapshot) < 0) {
        virReportOOMError();
        return NULL;
    }

    VIR_DEBUG("obj=%p", snapshot);

    return snapshot;
}

static void virDomainSnapshotObjFree(virDomainSnapshotObjPtr snapshot)
{
    if (!snapshot)
        return;

    VIR_DEBUG("obj=%p", snapshot);

    virDomainSnapshotDefFree(snapshot->def);
    VIR_FREE(snapshot);
}

virDomainSnapshotObjPtr virDomainSnapshotAssignDef(virDomainSnapshotObjListPtr snapshots,
                                                   const virDomainSnapshotDefPtr def)
{
    virDomainSnapshotObjPtr snap;

    if (virHashLookup(snapshots->objs, def->name) != NULL) {
        virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                             _("unexpected domain snapshot %s already exists"),
                             def->name);
        return NULL;
    }

    if (!(snap = virDomainSnapshotObjNew()))
        return NULL;
    snap->def = def;

    if (virHashAddEntry(snapshots->objs, snap->def->name, snap) < 0) {
        VIR_FREE(snap);
        return NULL;
    }

    return snap;
}

/* Snapshot Obj List functions */
static void
virDomainSnapshotObjListDataFree(void *payload,
                                 const void *name ATTRIBUTE_UNUSED)
{
    virDomainSnapshotObjPtr obj = payload;

    virDomainSnapshotObjFree(obj);
}

int virDomainSnapshotObjListInit(virDomainSnapshotObjListPtr snapshots)
{
    snapshots->objs = virHashCreate(50, virDomainSnapshotObjListDataFree);
    if (!snapshots->objs)
        return -1;
    return 0;
}

static void
virDomainSnapshotObjListDeinit(virDomainSnapshotObjListPtr snapshots)
{
    virHashFree(snapshots->objs);
}

struct virDomainSnapshotNameData {
    int oom;
    int numnames;
    int maxnames;
    char **const names;
    unsigned int flags;
};

static void virDomainSnapshotObjListCopyNames(void *payload,
                                              const void *name ATTRIBUTE_UNUSED,
                                              void *opaque)
{
    virDomainSnapshotObjPtr obj = payload;
    struct virDomainSnapshotNameData *data = opaque;

    if (data->oom)
        return;
    if ((data->flags & VIR_DOMAIN_SNAPSHOT_LIST_ROOTS) && obj->def->parent)
        return;

    if (data->numnames < data->maxnames) {
        if (!(data->names[data->numnames] = strdup(obj->def->name)))
            data->oom = 1;
        else
            data->numnames++;
    }
}

int virDomainSnapshotObjListGetNames(virDomainSnapshotObjListPtr snapshots,
                                     char **const names, int maxnames,
                                     unsigned int flags)
{
    struct virDomainSnapshotNameData data = { 0, 0, maxnames, names, flags };
    int i;

    virHashForEach(snapshots->objs, virDomainSnapshotObjListCopyNames, &data);
    if (data.oom) {
        virReportOOMError();
        goto cleanup;
    }

    return data.numnames;

cleanup:
    for (i = 0; i < data.numnames; i++)
        VIR_FREE(data.names[i]);
    return -1;
}

struct virDomainSnapshotNumData {
    int count;
    unsigned int flags;
};

static void virDomainSnapshotObjListCount(void *payload,
                                          const void *name ATTRIBUTE_UNUSED,
                                          void *opaque)
{
    virDomainSnapshotObjPtr obj = payload;
    struct virDomainSnapshotNumData *data = opaque;

    if ((data->flags & VIR_DOMAIN_SNAPSHOT_LIST_ROOTS) && obj->def->parent)
        return;
    data->count++;
}

int virDomainSnapshotObjListNum(virDomainSnapshotObjListPtr snapshots,
                                unsigned int flags)
{
    struct virDomainSnapshotNumData data = { 0, flags };

    virHashForEach(snapshots->objs, virDomainSnapshotObjListCount, &data);

    return data.count;
}

virDomainSnapshotObjPtr
virDomainSnapshotFindByName(const virDomainSnapshotObjListPtr snapshots,
                            const char *name)
{
    return virHashLookup(snapshots->objs, name);
}

void virDomainSnapshotObjListRemove(virDomainSnapshotObjListPtr snapshots,
                                    virDomainSnapshotObjPtr snapshot)
{
    virHashRemoveEntry(snapshots->objs, snapshot->def->name);
}

struct snapshot_act_on_child {
    char *parent;
    int number;
    virHashIterator iter;
    void *data;
};

static void
virDomainSnapshotActOnChild(void *payload,
                            const void *name,
                            void *data)
{
    virDomainSnapshotObjPtr obj = payload;
    struct snapshot_act_on_child *curr = data;

    if (obj->def->parent && STREQ(curr->parent, obj->def->parent)) {
        curr->number++;
        if (curr->iter)
            (curr->iter)(payload, name, curr->data);
    }
}

/* Run iter(data) on all direct children of snapshot, while ignoring all
 * other entries in snapshots.  Return the number of children
 * visited.  No particular ordering is guaranteed.  */
int
virDomainSnapshotForEachChild(virDomainSnapshotObjListPtr snapshots,
                              virDomainSnapshotObjPtr snapshot,
                              virHashIterator iter,
                              void *data)
{
    struct snapshot_act_on_child act;

    act.parent = snapshot->def->name;
    act.number = 0;
    act.iter = iter;
    act.data = data;
    virHashForEach(snapshots->objs, virDomainSnapshotActOnChild, &act);

    return act.number;
}

int virDomainSnapshotHasChildren(virDomainSnapshotObjPtr snap,
                                 virDomainSnapshotObjListPtr snapshots)
{
    return virDomainSnapshotForEachChild(snapshots, snap, NULL, NULL);
}

typedef enum {
    MARK_NONE,       /* No relation determined yet */
    MARK_DESCENDANT, /* Descendant of target */
    MARK_OTHER,      /* Not a descendant of target */
} snapshot_mark;

struct snapshot_mark_descendant {
    const char *name; /* Parent's name on round 1, NULL on other rounds.  */
    virDomainSnapshotObjListPtr snapshots;
    bool marked; /* True if descendants were found in this round */
};

/* To be called in a loop until no more descendants are found.
 * Additionally marking known unrelated snapshots reduces the number
 * of total hash searches needed.  */
static void
virDomainSnapshotMarkDescendant(void *payload,
                                const void *name ATTRIBUTE_UNUSED,
                                void *data)
{
    virDomainSnapshotObjPtr obj = payload;
    struct snapshot_mark_descendant *curr = data;
    virDomainSnapshotObjPtr parent = NULL;

    /* Learned on a previous pass.  */
    if (obj->mark)
        return;

    if (curr->name) {
        /* First round can only find root nodes and direct children.  */
        if (!obj->def->parent) {
            obj->mark = MARK_OTHER;
        } else if (STREQ(obj->def->parent, curr->name)) {
            obj->mark = MARK_DESCENDANT;
            curr->marked = true;
        }
    } else {
        /* All remaining rounds propagate marks from parents to children.  */
        parent = virDomainSnapshotFindByName(curr->snapshots, obj->def->parent);
        if (!parent) {
            VIR_WARN("snapshot hash table is inconsistent!");
            obj->mark = MARK_OTHER;
            return;
        }
        if (parent->mark) {
            obj->mark = parent->mark;
            if (obj->mark == MARK_DESCENDANT)
                curr->marked = true;
        }
    }
}

struct snapshot_act_on_descendant {
    int number;
    virHashIterator iter;
    void *data;
};

static void
virDomainSnapshotActOnDescendant(void *payload,
                                 const void *name,
                                 void *data)
{
    virDomainSnapshotObjPtr obj = payload;
    struct snapshot_act_on_descendant *curr = data;

    if (obj->mark == MARK_DESCENDANT) {
        curr->number++;
        (curr->iter)(payload, name, curr->data);
    }
    obj->mark = MARK_NONE;
}

/* Run iter(data) on all descendants of snapshot, while ignoring all
 * other entries in snapshots.  Return the number of descendants
 * visited.  No particular ordering is guaranteed.  */
int
virDomainSnapshotForEachDescendant(virDomainSnapshotObjListPtr snapshots,
                                   virDomainSnapshotObjPtr snapshot,
                                   virHashIterator iter,
                                   void *data)
{
    struct snapshot_mark_descendant mark;
    struct snapshot_act_on_descendant act;

    /* virHashForEach does not support nested traversal, so we must
     * instead iterate until no more snapshots get marked.  We
     * guarantee that on exit, all marks have been cleared again.  */
    mark.name = snapshot->def->name;
    mark.snapshots = snapshots;
    mark.marked = true;
    while (mark.marked) {
        mark.marked = false;
        virHashForEach(snapshots->objs, virDomainSnapshotMarkDescendant, &mark);
        mark.name = NULL;
    }
    act.number = 0;
    act.iter = iter;
    act.data = data;
    virHashForEach(snapshots->objs, virDomainSnapshotActOnDescendant, &act);

    return act.number;
}

int virDomainChrDefForeach(virDomainDefPtr def,
                           bool abortOnError,
                           virDomainChrDefIterator iter,
                           void *opaque)
{
    int i;
    int rc = 0;

    for (i = 0 ; i < def->nserials ; i++) {
        if ((iter)(def,
                   def->serials[i],
                   opaque) < 0)
            rc = -1;

        if (abortOnError && rc != 0)
            goto done;
    }

    for (i = 0 ; i < def->nparallels ; i++) {
        if ((iter)(def,
                   def->parallels[i],
                   opaque) < 0)
            rc = -1;

        if (abortOnError && rc != 0)
            goto done;
    }

    for (i = 0 ; i < def->nchannels ; i++) {
        if ((iter)(def,
                   def->channels[i],
                   opaque) < 0)
            rc = -1;

        if (abortOnError && rc != 0)
            goto done;
    }
    if (def->console) {
        if ((iter)(def,
                   def->console,
                   opaque) < 0)
            rc = -1;

        if (abortOnError && rc != 0)
            goto done;
    }

done:
    return rc;
}


int virDomainSmartcardDefForeach(virDomainDefPtr def,
                                 bool abortOnError,
                                 virDomainSmartcardDefIterator iter,
                                 void *opaque)
{
    int i;
    int rc = 0;

    for (i = 0 ; i < def->nsmartcards ; i++) {
        if ((iter)(def,
                   def->smartcards[i],
                   opaque) < 0)
            rc = -1;

        if (abortOnError && rc != 0)
            goto done;
    }

done:
    return rc;
}


int virDomainDiskDefForeachPath(virDomainDiskDefPtr disk,
                                bool allowProbing,
                                bool ignoreOpenFailure,
                                virDomainDiskDefPathIterator iter,
                                void *opaque)
{
    virHashTablePtr paths = NULL;
    int format;
    int ret = -1;
    size_t depth = 0;
    char *nextpath = NULL;
    virStorageFileMetadata *meta;

    if (!disk->src || disk->type == VIR_DOMAIN_DISK_TYPE_NETWORK)
        return 0;

    if (VIR_ALLOC(meta) < 0) {
        virReportOOMError();
        return ret;
    }

    if (disk->driverType) {
        const char *formatStr = disk->driverType;
        if (STREQ(formatStr, "aio"))
            formatStr = "raw"; /* Xen compat */

        if ((format = virStorageFileFormatTypeFromString(formatStr)) < 0) {
            virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                 _("unknown disk format '%s' for %s"),
                                 disk->driverType, disk->src);
            goto cleanup;
        }
    } else {
        if (allowProbing) {
            format = VIR_STORAGE_FILE_AUTO;
        } else {
            virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                 _("no disk format for %s and probing is disabled"),
                                 disk->src);
            goto cleanup;
        }
    }

    paths = virHashCreate(5, NULL);

    do {
        const char *path = nextpath ? nextpath : disk->src;
        int fd;

        if (iter(disk, path, depth, opaque) < 0)
            goto cleanup;

        if (virHashLookup(paths, path)) {
            virDomainReportError(VIR_ERR_INTERNAL_ERROR,
                                 _("backing store for %s is self-referential"),
                                 disk->src);
            goto cleanup;
        }

        if ((fd = open(path, O_RDONLY)) < 0) {
            if (ignoreOpenFailure) {
                char ebuf[1024];
                VIR_WARN("Ignoring open failure on %s: %s", path,
                         virStrerror(errno, ebuf, sizeof(ebuf)));
                break;
            } else {
                virReportSystemError(errno,
                                     _("unable to open disk path %s"),
                                     path);
                goto cleanup;
            }
        }

        if (virStorageFileGetMetadataFromFD(path, fd, format, meta) < 0) {
            VIR_FORCE_CLOSE(fd);
            goto cleanup;
        }

        if (VIR_CLOSE(fd) < 0)
            virReportSystemError(errno,
                                 _("could not close file %s"),
                                 path);

        if (virHashAddEntry(paths, path, (void*)0x1) < 0)
            goto cleanup;

        depth++;
        VIR_FREE(nextpath);
        nextpath = meta->backingStore;
        meta->backingStore = NULL;

        /* Stop iterating if we reach a non-file backing store */
        if (nextpath && !meta->backingStoreIsFile) {
            VIR_DEBUG("Stopping iteration on non-file backing store: %s",
                      nextpath);
            break;
        }

        format = meta->backingStoreFormat;

        if (format == VIR_STORAGE_FILE_AUTO &&
            !allowProbing)
            format = VIR_STORAGE_FILE_RAW; /* Stops further recursion */

        /* Allow probing for image formats that are safe */
        if (format == VIR_STORAGE_FILE_AUTO_SAFE)
            format = VIR_STORAGE_FILE_AUTO;
    } while (nextpath);

    ret = 0;

cleanup:
    virHashFree(paths);
    VIR_FREE(nextpath);
    virStorageFileFreeMetadata(meta);

    return ret;
}


virDomainDefPtr
virDomainObjCopyPersistentDef(virCapsPtr caps, virDomainObjPtr dom)
{
    char *xml;
    virDomainDefPtr cur, ret;

    cur = virDomainObjGetPersistentDef(caps, dom);

    xml = virDomainDefFormat(cur, VIR_DOMAIN_XML_WRITE_FLAGS);
    if (!xml)
        return NULL;

    ret = virDomainDefParseString(caps, xml, -1, VIR_DOMAIN_XML_READ_FLAGS);

    VIR_FREE(xml);
    return ret;
}


virDomainState
virDomainObjGetState(virDomainObjPtr dom, int *reason)
{
    if (reason)
        *reason = dom->state.reason;

    return dom->state.state;
}


void
virDomainObjSetState(virDomainObjPtr dom, virDomainState state, int reason)
{
    int last = -1;

    switch (state) {
    case VIR_DOMAIN_NOSTATE:    last = VIR_DOMAIN_NOSTATE_LAST;     break;
    case VIR_DOMAIN_RUNNING:    last = VIR_DOMAIN_RUNNING_LAST;     break;
    case VIR_DOMAIN_BLOCKED:    last = VIR_DOMAIN_BLOCKED_LAST;     break;
    case VIR_DOMAIN_PAUSED:     last = VIR_DOMAIN_PAUSED_LAST;      break;
    case VIR_DOMAIN_SHUTDOWN:   last = VIR_DOMAIN_SHUTDOWN_LAST;    break;
    case VIR_DOMAIN_SHUTOFF:    last = VIR_DOMAIN_SHUTOFF_LAST;     break;
    case VIR_DOMAIN_CRASHED:    last = VIR_DOMAIN_CRASHED_LAST;     break;
    default: last = -1;
    }

    if (last < 0) {
        VIR_ERROR(_("invalid domain state: %d"), state);
        return;
    }

    dom->state.state = state;
    if (reason > 0 && reason < last)
        dom->state.reason = reason;
    else
        dom->state.reason = 0;
}


const char *
virDomainStateReasonToString(virDomainState state, int reason)
{
    switch (state) {
    case VIR_DOMAIN_NOSTATE:
        return virDomainNostateReasonTypeToString(reason);
    case VIR_DOMAIN_RUNNING:
        return virDomainRunningReasonTypeToString(reason);
    case VIR_DOMAIN_BLOCKED:
        return virDomainBlockedReasonTypeToString(reason);
    case VIR_DOMAIN_PAUSED:
        return virDomainPausedReasonTypeToString(reason);
    case VIR_DOMAIN_SHUTDOWN:
        return virDomainShutdownReasonTypeToString(reason);
    case VIR_DOMAIN_SHUTOFF:
        return virDomainShutoffReasonTypeToString(reason);
    case VIR_DOMAIN_CRASHED:
        return virDomainCrashedReasonTypeToString(reason);
    default:
        return NULL;
    }
}


int
virDomainStateReasonFromString(virDomainState state, const char *reason)
{
    switch (state) {
    case VIR_DOMAIN_NOSTATE:
        return virDomainNostateReasonTypeFromString(reason);
    case VIR_DOMAIN_RUNNING:
        return virDomainRunningReasonTypeFromString(reason);
    case VIR_DOMAIN_BLOCKED:
        return virDomainBlockedReasonTypeFromString(reason);
    case VIR_DOMAIN_PAUSED:
        return virDomainPausedReasonTypeFromString(reason);
    case VIR_DOMAIN_SHUTDOWN:
        return virDomainShutdownReasonTypeFromString(reason);
    case VIR_DOMAIN_SHUTOFF:
        return virDomainShutoffReasonTypeFromString(reason);
    case VIR_DOMAIN_CRASHED:
        return virDomainCrashedReasonTypeFromString(reason);
    default:
        return -1;
    }
}


/* Some access functions to gloss over the difference between NetDef
 * (<interface>) and ActualNetDef (<actual>). If the NetDef has an
 * ActualNetDef, return the requested value from the ActualNetDef,
 * otherwise return the value from the NetDef.
 */

int
virDomainNetGetActualType(virDomainNetDefPtr iface)
{
    if (iface->type != VIR_DOMAIN_NET_TYPE_NETWORK)
        return iface->type;
    if (!iface->data.network.actual)
        return iface->type;
    return iface->data.network.actual->type;
}

char *
virDomainNetGetActualVfPCIAddr(virDomainNetDefPtr iface)
{
    if (iface->type == VIR_DOMAIN_NET_TYPE_DIRECT)
        return iface->data.direct.vf_pci_addr;
    if (iface->type != VIR_DOMAIN_NET_TYPE_NETWORK)
        return NULL;
    if (!iface->data.network.actual)
        return NULL;
    return iface->data.network.actual->data.direct.vf_pci_addr;
}

int
virDomainNetGetActualVlan(virDomainNetDefPtr iface)
{
    if (iface->type == VIR_DOMAIN_NET_TYPE_DIRECT)
        return iface->data.direct.vlan;
    if (iface->type != VIR_DOMAIN_NET_TYPE_NETWORK)
        return 0;
    if (!iface->data.network.actual)
        return 0;
    return iface->data.network.actual->data.direct.vlan;    
}

char *
virDomainNetGetActualBridgeName(virDomainNetDefPtr iface)
{
    if (iface->type == VIR_DOMAIN_NET_TYPE_BRIDGE)
        return iface->data.bridge.brname;
    if (iface->type != VIR_DOMAIN_NET_TYPE_NETWORK)
        return NULL;
    if (!iface->data.network.actual)
        return NULL;
    return iface->data.network.actual->data.bridge.brname;
}

char *
virDomainNetGetActualDirectDev(virDomainNetDefPtr iface)
{
    if (iface->type == VIR_DOMAIN_NET_TYPE_DIRECT)
        return iface->data.direct.linkdev;
    if (iface->type != VIR_DOMAIN_NET_TYPE_NETWORK)
        return NULL;
    if (!iface->data.network.actual)
        return NULL;
    return iface->data.network.actual->data.direct.linkdev;
}

int
virDomainNetGetActualDirectMode(virDomainNetDefPtr iface)
{
    if (iface->type == VIR_DOMAIN_NET_TYPE_DIRECT)
        return iface->data.direct.mode;
    if (iface->type != VIR_DOMAIN_NET_TYPE_NETWORK)
        return 0;
    if (!iface->data.network.actual)
        return 0;
    return iface->data.network.actual->data.direct.mode;
}

virVirtualPortProfileParamsPtr
virDomainNetGetActualDirectVirtPortProfile(virDomainNetDefPtr iface)
{
    if (iface->type == VIR_DOMAIN_NET_TYPE_DIRECT)
        return iface->data.direct.virtPortProfile;
    if (iface->type != VIR_DOMAIN_NET_TYPE_NETWORK)
        return NULL;
    if (!iface->data.network.actual)
        return NULL;
    return iface->data.network.actual->data.direct.virtPortProfile;
}

virBandwidthPtr
virDomainNetGetActualBandwidth(virDomainNetDefPtr iface)
{
    if ((iface->type == VIR_DOMAIN_NET_TYPE_NETWORK) &&
        iface->data.network.actual && iface->data.network.actual->bandwidth) {
        return iface->data.network.actual->bandwidth;
    }
    return iface->bandwidth;
}


/* Return listens[ii] from the appropriate union for the graphics
 * type, or NULL if this is an unsuitable type, or the index is out of
 * bounds. If force0 is TRUE, ii == 0, and there is no listen array,
 * allocate one with a single item. */
static virDomainGraphicsListenDefPtr
virDomainGraphicsGetListen(virDomainGraphicsDefPtr def, size_t ii, bool force0)
{
    if ((def->type == VIR_DOMAIN_GRAPHICS_TYPE_VNC) ||
        (def->type == VIR_DOMAIN_GRAPHICS_TYPE_RDP) ||
        (def->type == VIR_DOMAIN_GRAPHICS_TYPE_SPICE)) {

        if (!def->listens && (ii == 0) && force0) {
            if (VIR_ALLOC(def->listens) < 0)
                virReportOOMError();
            else
                def->nListens = 1;
        }

        if (!def->listens || (def->nListens <= ii))
            return NULL;

        return &def->listens[ii];
    }

    /* it's a type that has no listens array */
    return NULL;
}


/* Access functions for the fields in a virDomainGraphicsDef's
 * "listens" array.
 *
 * NB: For simple backward compatibility with existing code, any of
 * the "Set" functions will auto-create listens[0] to store the new
 * setting, when necessary. Auto-creation beyond the first item is not
 * supported.
 *
 * Return values: All "Get" functions return the requested item, or
 * 0/NULL. (in the case of returned const char *, the caller should
 * make a copy if they want to keep it around). All "Set" functions
 * return 0 on success, -1 on failure. */

int
virDomainGraphicsListenGetType(virDomainGraphicsDefPtr def, size_t ii)
{
    virDomainGraphicsListenDefPtr listenInfo
        = virDomainGraphicsGetListen(def, ii, false);

    if (!listenInfo)
        return VIR_DOMAIN_GRAPHICS_LISTEN_TYPE_NONE;
    return listenInfo->type;
}


/* NB: This function assumes type has not previously been set. It
 * *will not* free any existing address or network based on a change
 * in value of type. */
int
virDomainGraphicsListenSetType(virDomainGraphicsDefPtr def, size_t ii, int val)
{
    virDomainGraphicsListenDefPtr listenInfo
        = virDomainGraphicsGetListen(def, ii, true);

    if (!listenInfo)
        return -1;
    listenInfo->type = val;
    return 0;
}


const char *
virDomainGraphicsListenGetAddress(virDomainGraphicsDefPtr def, size_t ii)
{
    virDomainGraphicsListenDefPtr listenInfo
        = virDomainGraphicsGetListen(def, ii, false);

    if (!listenInfo ||
        (listenInfo->type != VIR_DOMAIN_GRAPHICS_LISTEN_TYPE_ADDRESS))
        return NULL;
    return listenInfo->address;
}


/* Make a copy of up to len characters of address, and store it in
 * listens[ii].address. If setType is true, set the listen's type
 * to 'address', otherwise leave type alone. */
int
virDomainGraphicsListenSetAddress(virDomainGraphicsDefPtr def,
                                  size_t ii, const char *address,
                                  int len, bool setType)
{
    virDomainGraphicsListenDefPtr listenInfo
        = virDomainGraphicsGetListen(def, ii, true);

    if (!listenInfo)
        return -1;

    if (setType)
        listenInfo->type = VIR_DOMAIN_GRAPHICS_LISTEN_TYPE_ADDRESS;

    if (!address) {
        listenInfo->address = NULL;
        return 0;
    }

    listenInfo->address = (len == -1) ? strdup(address) : strndup(address, len);
    if (!listenInfo->address) {
        virReportOOMError();
        return -1;
    }

    return 0;
}


const char *
virDomainGraphicsListenGetNetwork(virDomainGraphicsDefPtr def, size_t ii)
{
    virDomainGraphicsListenDefPtr listenInfo
        = virDomainGraphicsGetListen(def, ii, false);

    if (!listenInfo ||
        (listenInfo->type != VIR_DOMAIN_GRAPHICS_LISTEN_TYPE_NETWORK))
        return NULL;
    return listenInfo->network;
}


/* Make a copy of up to len characters of address, and store it in
 * listens[ii].network */
int
virDomainGraphicsListenSetNetwork(virDomainGraphicsDefPtr def,
                                  size_t ii, const char *network, int len)
{
    virDomainGraphicsListenDefPtr listenInfo
        = virDomainGraphicsGetListen(def, ii, true);

    if (!listenInfo)
        return -1;

    listenInfo->type = VIR_DOMAIN_GRAPHICS_LISTEN_TYPE_NETWORK;

    if (!network) {
        listenInfo->network = NULL;
        return 0;
    }

    listenInfo->network = (len == -1) ? strdup(network) : strndup(network, len);
    if (!listenInfo->network) {
        virReportOOMError();
        return -1;
    }

    return 0;
}
