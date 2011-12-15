/*
 * xen_sxpr.c: Xen SEXPR parsing functions
 *
 * Copyright (C) 2011 Univention GmbH
 * Copyright (C) 2010-2011 Red Hat, Inc.
 * Copyright (C) 2005 Anthony Liguori <aliguori@us.ibm.com>
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
 * Author: Anthony Liguori <aliguori@us.ibm.com>
 * Author: Daniel Veillard <veillard@redhat.com>
 * Author: Markus Groß <gross@univention.de>
 */

#include <config.h>

#include "internal.h"
#include "virterror_internal.h"
#include "conf.h"
#include "memory.h"
#include "verify.h"
#include "uuid.h"
#include "logging.h"
#include "count-one-bits.h"
#include "xenxs_private.h"
#include "xen_sxpr.h"

/* Get a domain id from a sexpr string */
int xenGetDomIdFromSxprString(const char *sexpr, int xendConfigVersion)
{
    struct sexpr *root = string2sexpr(sexpr);

    if (!root)
        return -1;

    int id = xenGetDomIdFromSxpr(root, xendConfigVersion);
    sexpr_free(root);
    return id;
}

/* Get a domain id from a sexpr */
int xenGetDomIdFromSxpr(const struct sexpr *root, int xendConfigVersion)
{
    int id = -1;
    const char * tmp = sexpr_node(root, "domain/domid");
    if (tmp == NULL && xendConfigVersion < 3) { /* Old XenD, domid was mandatory */
        XENXS_ERROR(VIR_ERR_INTERNAL_ERROR,
                     "%s", _("domain information incomplete, missing id"));
    } else {
      id = tmp ? sexpr_int(root, "domain/domid") : -1;
    }
    return id;
}

/*****************************************************************
 ******
 ****** Parsing of SEXPR into virDomainDef objects
 ******
 *****************************************************************/

/**
 * xenParseSxprOS
 * @node: the root of the parsed S-Expression
 * @def: the domain config
 * @hvm: true or 1 if no contains HVM S-Expression
 * @bootloader: true or 1 if a bootloader is defined
 *
 * Parse the xend sexp for description of os and append it to buf.
 *
 * Returns 0 in case of success and -1 in case of error
 */
static int
xenParseSxprOS(const struct sexpr *node,
               virDomainDefPtr def,
               int hvm)
{
    if (hvm) {
        if (sexpr_node_copy(node, "domain/image/hvm/loader", &def->os.loader) < 0)
            goto no_memory;
        if (def->os.loader == NULL) {
            if (sexpr_node_copy(node, "domain/image/hvm/kernel", &def->os.loader) < 0)
                goto no_memory;

            if (def->os.loader == NULL) {
                XENXS_ERROR(VIR_ERR_INTERNAL_ERROR,
                             "%s", _("domain information incomplete, missing HVM loader"));
                return(-1);
            }
        } else {
            if (sexpr_node_copy(node, "domain/image/hvm/kernel", &def->os.kernel) < 0)
                goto no_memory;
            if (sexpr_node_copy(node, "domain/image/hvm/ramdisk", &def->os.initrd) < 0)
                goto no_memory;
            if (sexpr_node_copy(node, "domain/image/hvm/args", &def->os.cmdline) < 0)
                goto no_memory;
            if (sexpr_node_copy(node, "domain/image/hvm/root", &def->os.root) < 0)
                goto no_memory;
        }
    } else {
        if (sexpr_node_copy(node, "domain/image/linux/kernel", &def->os.kernel) < 0)
            goto no_memory;
        if (sexpr_node_copy(node, "domain/image/linux/ramdisk", &def->os.initrd) < 0)
            goto no_memory;
        if (sexpr_node_copy(node, "domain/image/linux/args", &def->os.cmdline) < 0)
            goto no_memory;
        if (sexpr_node_copy(node, "domain/image/linux/root", &def->os.root) < 0)
            goto no_memory;
    }

    /* If HVM kenrel == loader, then old xend, so kill off kernel */
    if (hvm &&
        def->os.kernel &&
        STREQ(def->os.kernel, def->os.loader)) {
        VIR_FREE(def->os.kernel);
    }

    if (!def->os.kernel &&
        hvm) {
        const char *boot = sexpr_node(node, "domain/image/hvm/boot");
        if ((boot != NULL) && (boot[0] != 0)) {
            while (*boot &&
                   def->os.nBootDevs < VIR_DOMAIN_BOOT_LAST) {
                if (*boot == 'a')
                    def->os.bootDevs[def->os.nBootDevs++] = VIR_DOMAIN_BOOT_FLOPPY;
                else if (*boot == 'c')
                    def->os.bootDevs[def->os.nBootDevs++] = VIR_DOMAIN_BOOT_DISK;
                else if (*boot == 'd')
                    def->os.bootDevs[def->os.nBootDevs++] = VIR_DOMAIN_BOOT_CDROM;
                else if (*boot == 'n')
                    def->os.bootDevs[def->os.nBootDevs++] = VIR_DOMAIN_BOOT_NET;
                boot++;
            }
        }
    }

    if (!hvm &&
        !def->os.kernel &&
        !def->os.bootloader) {
        XENXS_ERROR(VIR_ERR_INTERNAL_ERROR,
                     "%s", _("domain information incomplete, missing kernel & bootloader"));
        return -1;
    }

    return 0;

no_memory:
    virReportOOMError();
    return -1;
}

virDomainChrDefPtr
xenParseSxprChar(const char *value,
                 const char *tty)
{
    const char *prefix;
    char *tmp;
    virDomainChrDefPtr def;

    if (!(def = virDomainChrDefNew()))
        return NULL;

    prefix = value;

    if (value[0] == '/') {
        def->source.type = VIR_DOMAIN_CHR_TYPE_DEV;
        def->source.data.file.path = strdup(value);
        if (!def->source.data.file.path)
            goto no_memory;
    } else {
        if ((tmp = strchr(value, ':')) != NULL) {
            *tmp = '\0';
            value = tmp + 1;
        }

        if (STRPREFIX(prefix, "telnet")) {
            def->source.type = VIR_DOMAIN_CHR_TYPE_TCP;
            def->source.data.tcp.protocol = VIR_DOMAIN_CHR_TCP_PROTOCOL_TELNET;
        } else {
            if ((def->source.type = virDomainChrTypeFromString(prefix)) < 0) {
                XENXS_ERROR(VIR_ERR_INTERNAL_ERROR,
                             _("unknown chr device type '%s'"), prefix);
                goto error;
            }
        }
    }

    /* Compat with legacy  <console tty='/dev/pts/5'/> syntax */
    switch (def->source.type) {
    case VIR_DOMAIN_CHR_TYPE_PTY:
        if (tty != NULL &&
            !(def->source.data.file.path = strdup(tty)))
            goto no_memory;
        break;

    case VIR_DOMAIN_CHR_TYPE_FILE:
    case VIR_DOMAIN_CHR_TYPE_PIPE:
        if (!(def->source.data.file.path = strdup(value)))
            goto no_memory;
        break;

    case VIR_DOMAIN_CHR_TYPE_TCP:
    {
        const char *offset = strchr(value, ':');
        const char *offset2;

        if (offset == NULL) {
            XENXS_ERROR(VIR_ERR_INTERNAL_ERROR,
                         "%s", _("malformed char device string"));
            goto error;
        }

        if (offset != value &&
            (def->source.data.tcp.host = strndup(value,
                                                 offset - value)) == NULL)
            goto no_memory;

        offset2 = strchr(offset, ',');
        if (offset2 == NULL)
            def->source.data.tcp.service = strdup(offset+1);
        else
            def->source.data.tcp.service = strndup(offset+1,
                                                   offset2-(offset+1));
        if (def->source.data.tcp.service == NULL)
            goto no_memory;

        if (offset2 && strstr(offset2, ",server"))
            def->source.data.tcp.listen = true;
    }
    break;

    case VIR_DOMAIN_CHR_TYPE_UDP:
    {
        const char *offset = strchr(value, ':');
        const char *offset2, *offset3;

        if (offset == NULL) {
            XENXS_ERROR(VIR_ERR_INTERNAL_ERROR,
                         "%s", _("malformed char device string"));
            goto error;
        }

        if (offset != value &&
            (def->source.data.udp.connectHost
             = strndup(value, offset - value)) == NULL)
            goto no_memory;

        offset2 = strchr(offset, '@');
        if (offset2 != NULL) {
            if ((def->source.data.udp.connectService
                 = strndup(offset + 1, offset2-(offset+1))) == NULL)
                goto no_memory;

            offset3 = strchr(offset2, ':');
            if (offset3 == NULL) {
                XENXS_ERROR(VIR_ERR_INTERNAL_ERROR,
                             "%s", _("malformed char device string"));
                goto error;
            }

            if (offset3 > (offset2 + 1) &&
                (def->source.data.udp.bindHost
                 = strndup(offset2 + 1, offset3 - (offset2+1))) == NULL)
                goto no_memory;

            if ((def->source.data.udp.bindService
                 = strdup(offset3 + 1)) == NULL)
                goto no_memory;
        } else {
            if ((def->source.data.udp.connectService
                 = strdup(offset + 1)) == NULL)
                goto no_memory;
        }
    }
    break;

    case VIR_DOMAIN_CHR_TYPE_UNIX:
    {
        const char *offset = strchr(value, ',');
        if (offset)
            def->source.data.nix.path = strndup(value, (offset - value));
        else
            def->source.data.nix.path = strdup(value);
        if (def->source.data.nix.path == NULL)
            goto no_memory;

        if (offset != NULL &&
            strstr(offset, ",server") != NULL)
            def->source.data.nix.listen = true;
    }
    break;
    }

    return def;

no_memory:
    virReportOOMError();
error:
    virDomainChrDefFree(def);
    return NULL;
}

/**
 * xend_parse_sexp_desc_disks
 * @conn: connection
 * @root: root sexpr
 * @xendConfigVersion: version of xend
 *
 * This parses out block devices from the domain sexpr
 *
 * Returns 0 if successful or -1 if failed.
 */
static int
xenParseSxprDisks(virDomainDefPtr def,
                  const struct sexpr *root,
                  int hvm,
                  int xendConfigVersion)
{
    const struct sexpr *cur, *node;
    virDomainDiskDefPtr disk = NULL;

    for (cur = root; cur->kind == SEXPR_CONS; cur = cur->u.s.cdr) {
        node = cur->u.s.car;
        /* Normally disks are in a (device (vbd ...)) block
           but blktap disks ended up in a differently named
           (device (tap ....)) block.... */
        if (sexpr_lookup(node, "device/vbd") ||
            sexpr_lookup(node, "device/tap") ||
            sexpr_lookup(node, "device/tap2")) {
            char *offset;
            const char *src = NULL;
            const char *dst = NULL;
            const char *mode = NULL;

            /* Again dealing with (vbd...) vs (tap ...) differences */
            if (sexpr_lookup(node, "device/vbd")) {
                src = sexpr_node(node, "device/vbd/uname");
                dst = sexpr_node(node, "device/vbd/dev");
                mode = sexpr_node(node, "device/vbd/mode");
            } else if (sexpr_lookup(node, "device/tap2")) {
                src = sexpr_node(node, "device/tap2/uname");
                dst = sexpr_node(node, "device/tap2/dev");
                mode = sexpr_node(node, "device/tap2/mode");
            } else {
                src = sexpr_node(node, "device/tap/uname");
                dst = sexpr_node(node, "device/tap/dev");
                mode = sexpr_node(node, "device/tap/mode");
            }

            if (VIR_ALLOC(disk) < 0)
                goto no_memory;

            if (dst == NULL) {
                XENXS_ERROR(VIR_ERR_INTERNAL_ERROR,
                             "%s", _("domain information incomplete, vbd has no dev"));
                goto error;
            }

            if (src == NULL) {
                /* There is a case without the uname to the CD-ROM device */
                offset = strchr(dst, ':');
                if (!offset ||
                    !hvm ||
                    STRNEQ(offset, ":cdrom")) {
                    XENXS_ERROR(VIR_ERR_INTERNAL_ERROR,
                                 "%s", _("domain information incomplete, vbd has no src"));
                    goto error;
                }
            }

            if (src != NULL) {
                offset = strchr(src, ':');
                if (!offset) {
                    XENXS_ERROR(VIR_ERR_INTERNAL_ERROR,
                                 "%s", _("cannot parse vbd filename, missing driver name"));
                    goto error;
                }

                if (VIR_ALLOC_N(disk->driverName, (offset-src)+1) < 0)
                    goto no_memory;
                if (virStrncpy(disk->driverName, src, offset-src,
                              (offset-src)+1) == NULL) {
                    XENXS_ERROR(VIR_ERR_INTERNAL_ERROR,
                                 _("Driver name %s too big for destination"),
                                 src);
                    goto error;
                }

                src = offset + 1;

                if (STREQ (disk->driverName, "tap") ||
                    STREQ (disk->driverName, "tap2")) {
                    offset = strchr(src, ':');
                    if (!offset) {
                        XENXS_ERROR(VIR_ERR_INTERNAL_ERROR,
                                     "%s", _("cannot parse vbd filename, missing driver type"));
                        goto error;
                    }

                    if (VIR_ALLOC_N(disk->driverType, (offset-src)+1)< 0)
                        goto no_memory;
                    if (virStrncpy(disk->driverType, src, offset-src,
                                   (offset-src)+1) == NULL) {
                        XENXS_ERROR(VIR_ERR_INTERNAL_ERROR,
                                     _("Driver type %s too big for destination"),
                                     src);
                        goto error;
                    }

                    src = offset + 1;
                    /* Its possible to use blktap driver for block devs
                       too, but kinda pointless because blkback is better,
                       so we assume common case here. If blktap becomes
                       omnipotent, we can revisit this, perhaps stat()'ing
                       the src file in question */
                    disk->type = VIR_DOMAIN_DISK_TYPE_FILE;
                } else if (STREQ(disk->driverName, "phy")) {
                    disk->type = VIR_DOMAIN_DISK_TYPE_BLOCK;
                } else if (STREQ(disk->driverName, "file")) {
                    disk->type = VIR_DOMAIN_DISK_TYPE_FILE;
                }
            } else {
                /* No CDROM media so can't really tell. We'll just
                   call if a FILE for now and update when media
                   is inserted later */
                disk->type = VIR_DOMAIN_DISK_TYPE_FILE;
            }

            if (STREQLEN (dst, "ioemu:", 6))
                dst += 6;

            disk->device = VIR_DOMAIN_DISK_DEVICE_DISK;
            /* New style disk config from Xen >= 3.0.3 */
            if (xendConfigVersion > 1) {
                offset = strrchr(dst, ':');
                if (offset) {
                    if (STREQ (offset, ":cdrom")) {
                        disk->device = VIR_DOMAIN_DISK_DEVICE_CDROM;
                    } else if (STREQ (offset, ":disk")) {
                        /* The default anyway */
                    } else {
                        /* Unknown, lets pretend its a disk too */
                    }
                    offset[0] = '\0';
                }
            }

            if (!(disk->dst = strdup(dst)))
                goto no_memory;
            if (src &&
                !(disk->src = strdup(src)))
                goto no_memory;

            if (STRPREFIX(disk->dst, "xvd"))
                disk->bus = VIR_DOMAIN_DISK_BUS_XEN;
            else if (STRPREFIX(disk->dst, "hd"))
                disk->bus = VIR_DOMAIN_DISK_BUS_IDE;
            else if (STRPREFIX(disk->dst, "sd"))
                disk->bus = VIR_DOMAIN_DISK_BUS_SCSI;
            else
                disk->bus = VIR_DOMAIN_DISK_BUS_IDE;

            if (mode &&
                strchr(mode, 'r'))
                disk->readonly = 1;
            if (mode &&
                strchr(mode, '!'))
                disk->shared = 1;

            if (VIR_REALLOC_N(def->disks, def->ndisks+1) < 0)
                goto no_memory;

            def->disks[def->ndisks++] = disk;
            disk = NULL;
        }
    }

    return 0;

no_memory:
    virReportOOMError();

error:
    virDomainDiskDefFree(disk);
    return -1;
}


static int
xenParseSxprNets(virDomainDefPtr def,
                 const struct sexpr *root)
{
    virDomainNetDefPtr net = NULL;
    const struct sexpr *cur, *node;
    const char *tmp;
    int vif_index = 0;

    for (cur = root; cur->kind == SEXPR_CONS; cur = cur->u.s.cdr) {
        node = cur->u.s.car;
        if (sexpr_lookup(node, "device/vif")) {
            const char *tmp2, *model, *type;
            tmp2 = sexpr_node(node, "device/vif/script");
            tmp = sexpr_node(node, "device/vif/bridge");
            model = sexpr_node(node, "device/vif/model");
            type = sexpr_node(node, "device/vif/type");

            if (VIR_ALLOC(net) < 0)
                goto no_memory;

            if (tmp != NULL ||
                (tmp2 != NULL && STREQ(tmp2, DEFAULT_VIF_SCRIPT))) {
                net->type = VIR_DOMAIN_NET_TYPE_BRIDGE;
                /* XXX virtual network reverse resolve */

                if (tmp &&
                    !(net->data.bridge.brname = strdup(tmp)))
                    goto no_memory;
                if (tmp2 &&
                    net->type == VIR_DOMAIN_NET_TYPE_BRIDGE &&
                    !(net->data.bridge.script = strdup(tmp2)))
                    goto no_memory;
                tmp = sexpr_node(node, "device/vif/ip");
                if (tmp &&
                    !(net->data.bridge.ipaddr = strdup(tmp)))
                    goto no_memory;
            } else {
                net->type = VIR_DOMAIN_NET_TYPE_ETHERNET;
                if (tmp2 &&
                    !(net->data.ethernet.script = strdup(tmp2)))
                    goto no_memory;
                tmp = sexpr_node(node, "device/vif/ip");
                if (tmp &&
                    !(net->data.ethernet.ipaddr = strdup(tmp)))
                    goto no_memory;
            }

            tmp = sexpr_node(node, "device/vif/vifname");
            /* If vifname is specified in xend config, include it in net
             * definition regardless of domain state.  If vifname is not
             * specified, only generate one if domain is active (id != -1). */
            if (tmp) {
                if (!(net->ifname = strdup(tmp)))
                    goto no_memory;
            } else if (def->id != -1) {
                if (virAsprintf(&net->ifname, "vif%d.%d", def->id, vif_index) < 0)
                    goto no_memory;
            }

            tmp = sexpr_node(node, "device/vif/mac");
            if (tmp) {
                if (virParseMacAddr(tmp, net->mac) < 0) {
                    XENXS_ERROR(VIR_ERR_INTERNAL_ERROR,
                                 _("malformed mac address '%s'"), tmp);
                    goto cleanup;
                }
            }

            if (model &&
                !(net->model = strdup(model)))
                goto no_memory;

            if (!model && type &&
                STREQ(type, "netfront") &&
                !(net->model = strdup("netfront")))
                goto no_memory;

            if (VIR_REALLOC_N(def->nets, def->nnets + 1) < 0)
                goto no_memory;

            def->nets[def->nnets++] = net;
            vif_index++;
        }
    }

    return 0;

no_memory:
    virReportOOMError();
cleanup:
    virDomainNetDefFree(net);
    return -1;
}


int
xenParseSxprSound(virDomainDefPtr def,
                  const char *str)
{
    if (STREQ(str, "all")) {
        int i;

        /*
         * Special compatability code for Xen with a bogus
         * sound=all in config.
         *
         * NB deliberately, don't include all possible
         * sound models anymore, just the 2 that were
         * historically present in Xen's QEMU.
         *
         * ie just es1370 + sb16.
         *
         * Hence use of MODEL_ES1370 + 1, instead of MODEL_LAST
         */

        if (VIR_ALLOC_N(def->sounds,
                        VIR_DOMAIN_SOUND_MODEL_ES1370 + 1) < 0)
            goto no_memory;


        for (i = 0 ; i < (VIR_DOMAIN_SOUND_MODEL_ES1370 + 1) ; i++) {
            virDomainSoundDefPtr sound;
            if (VIR_ALLOC(sound) < 0)
                goto no_memory;
            sound->model = i;
            def->sounds[def->nsounds++] = sound;
        }
    } else {
        char model[10];
        const char *offset = str, *offset2;

        do {
            int len;
            virDomainSoundDefPtr sound;
            offset2 = strchr(offset, ',');
            if (offset2)
                len = (offset2 - offset);
            else
                len = strlen(offset);
            if (virStrncpy(model, offset, len, sizeof(model)) == NULL) {
                XENXS_ERROR(VIR_ERR_INTERNAL_ERROR,
                                 _("Sound model %s too big for destination"),
                             offset);
                goto error;
            }

            if (VIR_ALLOC(sound) < 0)
                goto no_memory;

            if ((sound->model = virDomainSoundModelTypeFromString(model)) < 0) {
                VIR_FREE(sound);
                goto error;
            }

            if (VIR_REALLOC_N(def->sounds, def->nsounds+1) < 0) {
                virDomainSoundDefFree(sound);
                goto no_memory;
            }

            def->sounds[def->nsounds++] = sound;
            offset = offset2 ? offset2 + 1 : NULL;
        } while (offset);
    }

    return 0;

no_memory:
    virReportOOMError();
error:
    return -1;
}


static int
xenParseSxprUSB(virDomainDefPtr def,
                const struct sexpr *root)
{
    struct sexpr *cur, *node;
    const char *tmp;

    for (cur = sexpr_lookup(root, "domain/image/hvm"); cur && cur->kind == SEXPR_CONS; cur = cur->u.s.cdr) {
        node = cur->u.s.car;
        if (sexpr_lookup(node, "usbdevice")) {
            tmp = sexpr_node(node, "usbdevice");
            if (tmp && *tmp) {
                if (STREQ(tmp, "tablet") ||
                    STREQ(tmp, "mouse")) {
                    virDomainInputDefPtr input;
                    if (VIR_ALLOC(input) < 0)
                        goto no_memory;
                    input->bus = VIR_DOMAIN_INPUT_BUS_USB;
                    if (STREQ(tmp, "tablet"))
                        input->type = VIR_DOMAIN_INPUT_TYPE_TABLET;
                    else
                        input->type = VIR_DOMAIN_INPUT_TYPE_MOUSE;

                    if (VIR_REALLOC_N(def->inputs, def->ninputs+1) < 0) {
                        VIR_FREE(input);
                        goto no_memory;
                    }
                    def->inputs[def->ninputs++] = input;
                } else {
                    /* XXX Handle other non-input USB devices later */
                }
            }
        }
    }
    return 0;

no_memory:
    virReportOOMError();
    return -1;
}

static int
xenParseSxprGraphicsOld(virDomainDefPtr def,
                        const struct sexpr *root,
                        int hvm,
                        int xendConfigVersion, int vncport)
{
    const char *tmp;
    virDomainGraphicsDefPtr graphics = NULL;

    if ((tmp = sexpr_fmt_node(root, "domain/image/%s/vnc", hvm ? "hvm" : "linux")) &&
        tmp[0] == '1') {
        /* Graphics device (HVM, or old (pre-3.0.4) style PV VNC config) */
        int port;
        const char *listenAddr = sexpr_fmt_node(root, "domain/image/%s/vnclisten", hvm ? "hvm" : "linux");
        const char *vncPasswd = sexpr_fmt_node(root, "domain/image/%s/vncpasswd", hvm ? "hvm" : "linux");
        const char *keymap = sexpr_fmt_node(root, "domain/image/%s/keymap", hvm ? "hvm" : "linux");
        const char *unused = sexpr_fmt_node(root, "domain/image/%s/vncunused", hvm ? "hvm" : "linux");

        port = vncport;

        if (VIR_ALLOC(graphics) < 0)
            goto no_memory;

        graphics->type = VIR_DOMAIN_GRAPHICS_TYPE_VNC;
        /* For Xen >= 3.0.3, don't generate a fixed port mapping
         * because it will almost certainly be wrong ! Just leave
         * it as -1 which lets caller see that the VNC server isn't
         * present yet. Subsquent dumps of the XML will eventually
         * find the port in XenStore once VNC server has started
         */
        if (port == -1 && xendConfigVersion < 2)
            port = 5900 + def->id;

        if ((unused && STREQ(unused, "1")) || port == -1)
            graphics->data.vnc.autoport = 1;
        graphics->data.vnc.port = port;

        if (listenAddr &&
            virDomainGraphicsListenSetAddress(graphics, 0, listenAddr, -1, true))
            goto error;

        if (vncPasswd &&
            !(graphics->data.vnc.auth.passwd = strdup(vncPasswd)))
            goto no_memory;

        if (keymap &&
            !(graphics->data.vnc.keymap = strdup(keymap)))
            goto no_memory;

        if (VIR_ALLOC_N(def->graphics, 1) < 0)
            goto no_memory;
        def->graphics[0] = graphics;
        def->ngraphics = 1;
        graphics = NULL;
    } else if ((tmp = sexpr_fmt_node(root, "domain/image/%s/sdl", hvm ? "hvm" : "linux")) &&
               tmp[0] == '1') {
        /* Graphics device (HVM, or old (pre-3.0.4) style PV sdl config) */
        const char *display = sexpr_fmt_node(root, "domain/image/%s/display", hvm ? "hvm" : "linux");
        const char *xauth = sexpr_fmt_node(root, "domain/image/%s/xauthority", hvm ? "hvm" : "linux");

        if (VIR_ALLOC(graphics) < 0)
            goto no_memory;

        graphics->type = VIR_DOMAIN_GRAPHICS_TYPE_SDL;
        if (display &&
            !(graphics->data.sdl.display = strdup(display)))
            goto no_memory;
        if (xauth &&
            !(graphics->data.sdl.xauth = strdup(xauth)))
            goto no_memory;

        if (VIR_ALLOC_N(def->graphics, 1) < 0)
            goto no_memory;
        def->graphics[0] = graphics;
        def->ngraphics = 1;
        graphics = NULL;
    }

    return 0;

no_memory:
    virReportOOMError();
error:
    virDomainGraphicsDefFree(graphics);
    return -1;
}


static int
xenParseSxprGraphicsNew(virDomainDefPtr def,
                        const struct sexpr *root, int vncport)
{
    virDomainGraphicsDefPtr graphics = NULL;
    const struct sexpr *cur, *node;
    const char *tmp;

    /* append network devices and framebuffer */
    for (cur = root; cur->kind == SEXPR_CONS; cur = cur->u.s.cdr) {
        node = cur->u.s.car;
        if (sexpr_lookup(node, "device/vfb")) {
            /* New style graphics config for PV guests in >= 3.0.4,
             * or for HVM guests in >= 3.0.5 */
            if (sexpr_node(node, "device/vfb/type")) {
                tmp = sexpr_node(node, "device/vfb/type");
            } else if (sexpr_node(node, "device/vfb/vnc")) {
                tmp = "vnc";
            } else if (sexpr_node(node, "device/vfb/sdl")) {
                tmp = "sdl";
            } else {
                tmp = "unknown";
            }

            if (VIR_ALLOC(graphics) < 0)
                goto no_memory;

            if ((graphics->type = virDomainGraphicsTypeFromString(tmp)) < 0) {
                XENXS_ERROR(VIR_ERR_INTERNAL_ERROR,
                             _("unknown graphics type '%s'"), tmp);
                goto error;
            }

            if (graphics->type == VIR_DOMAIN_GRAPHICS_TYPE_SDL) {
                const char *display = sexpr_node(node, "device/vfb/display");
                const char *xauth = sexpr_node(node, "device/vfb/xauthority");
                if (display &&
                    !(graphics->data.sdl.display = strdup(display)))
                    goto no_memory;
                if (xauth &&
                    !(graphics->data.sdl.xauth = strdup(xauth)))
                    goto no_memory;
            } else {
                int port;
                const char *listenAddr = sexpr_node(node, "device/vfb/vnclisten");
                const char *vncPasswd = sexpr_node(node, "device/vfb/vncpasswd");
                const char *keymap = sexpr_node(node, "device/vfb/keymap");
                const char *unused = sexpr_node(node, "device/vfb/vncunused");

                port = vncport;

                /* Didn't find port entry in xenstore */
                if (port == -1) {
                    const char *str = sexpr_node(node, "device/vfb/vncdisplay");
                    int val;
                    if (str != NULL && virStrToLong_i(str, NULL, 0, &val) == 0)
                        port = val;
                }

                if ((unused && STREQ(unused, "1")) || port == -1)
                    graphics->data.vnc.autoport = 1;

                if (port >= 0 && port < 5900)
                    port += 5900;
                graphics->data.vnc.port = port;

                if (listenAddr &&
                    virDomainGraphicsListenSetAddress(graphics, 0, listenAddr, -1, true))
                    goto error;

                if (vncPasswd &&
                    !(graphics->data.vnc.auth.passwd = strdup(vncPasswd)))
                    goto no_memory;

                if (keymap &&
                    !(graphics->data.vnc.keymap = strdup(keymap)))
                    goto no_memory;
            }

            if (VIR_ALLOC_N(def->graphics, 1) < 0)
                goto no_memory;
            def->graphics[0] = graphics;
            def->ngraphics = 1;
            graphics = NULL;
            break;
        }
    }

    return 0;

no_memory:
    virReportOOMError();
error:
    virDomainGraphicsDefFree(graphics);
    return -1;
}

/**
 * xenParseSxprPCI
 * @root: root sexpr
 *
 * This parses out block devices from the domain sexpr
 *
 * Returns 0 if successful or -1 if failed.
 */
static int
xenParseSxprPCI(virDomainDefPtr def,
                const struct sexpr *root)
{
    const struct sexpr *cur, *tmp = NULL, *node;
    virDomainHostdevDefPtr dev = NULL;

    /*
     * With the (domain ...) block we have the following odd setup
     *
     * (device
     *    (pci
     *       (dev (domain 0x0000) (bus 0x00) (slot 0x1b) (func 0x0))
     *       (dev (domain 0x0000) (bus 0x00) (slot 0x13) (func 0x0))
     *    )
     * )
     *
     * Normally there is one (device ...) block per device, but in
     * weird world of Xen PCI, once (device ...) covers multiple
     * devices.
     */

    for (cur = root; cur->kind == SEXPR_CONS; cur = cur->u.s.cdr) {
        node = cur->u.s.car;
        if ((tmp = sexpr_lookup(node, "device/pci")) != NULL)
            break;
    }

    if (!tmp)
        return 0;

    for (cur = tmp; cur->kind == SEXPR_CONS; cur = cur->u.s.cdr) {
        const char *domain = NULL;
        const char *bus = NULL;
        const char *slot = NULL;
        const char *func = NULL;
        int domainID;
        int busID;
        int slotID;
        int funcID;

        node = cur->u.s.car;
        if (!sexpr_lookup(node, "dev"))
            continue;

        if (!(domain = sexpr_node(node, "dev/domain"))) {
            XENXS_ERROR(VIR_ERR_INTERNAL_ERROR,
                         "%s", _("missing PCI domain"));
            goto error;
        }
        if (!(bus = sexpr_node(node, "dev/bus"))) {
            XENXS_ERROR(VIR_ERR_INTERNAL_ERROR,
                         "%s", _("missing PCI bus"));
            goto error;
        }
        if (!(slot = sexpr_node(node, "dev/slot"))) {
            XENXS_ERROR(VIR_ERR_INTERNAL_ERROR,
                         "%s", _("missing PCI slot"));
            goto error;
        }
        if (!(func = sexpr_node(node, "dev/func"))) {
            XENXS_ERROR(VIR_ERR_INTERNAL_ERROR,
                         "%s", _("missing PCI func"));
            goto error;
        }

        if (virStrToLong_i(domain, NULL, 0, &domainID) < 0) {
            XENXS_ERROR(VIR_ERR_INTERNAL_ERROR,
                         _("cannot parse PCI domain '%s'"), domain);
            goto error;
        }
        if (virStrToLong_i(bus, NULL, 0, &busID) < 0) {
            XENXS_ERROR(VIR_ERR_INTERNAL_ERROR,
                         _("cannot parse PCI bus '%s'"), bus);
            goto error;
        }
        if (virStrToLong_i(slot, NULL, 0, &slotID) < 0) {
            XENXS_ERROR(VIR_ERR_INTERNAL_ERROR,
                         _("cannot parse PCI slot '%s'"), slot);
            goto error;
        }
        if (virStrToLong_i(func, NULL, 0, &funcID) < 0) {
            XENXS_ERROR(VIR_ERR_INTERNAL_ERROR,
                         _("cannot parse PCI func '%s'"), func);
            goto error;
        }

        if (VIR_ALLOC(dev) < 0)
            goto no_memory;

        dev->mode = VIR_DOMAIN_HOSTDEV_MODE_SUBSYS;
        dev->managed = 0;
        dev->source.subsys.type = VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_PCI;
        dev->source.subsys.u.pci.domain = domainID;
        dev->source.subsys.u.pci.bus = busID;
        dev->source.subsys.u.pci.slot = slotID;
        dev->source.subsys.u.pci.function = funcID;

        if (VIR_REALLOC_N(def->hostdevs, def->nhostdevs+1) < 0) {
            goto no_memory;
        }

        def->hostdevs[def->nhostdevs++] = dev;
    }

    return 0;

no_memory:
    virReportOOMError();

error:
    virDomainHostdevDefFree(dev);
    return -1;
}


/**
 * xenParseSxpr:
 * @conn: the connection associated with the XML
 * @root: the root of the parsed S-Expression
 * @xendConfigVersion: version of xend
 * @cpus: set of cpus the domain may be pinned to
 *
 * Parse the xend sexp description and turn it into the XML format similar
 * to the one unsed for creation.
 *
 * Returns the 0 terminated XML string or NULL in case of error.
 *         the caller must free() the returned value.
 */
virDomainDefPtr
xenParseSxpr(const struct sexpr *root,
             int xendConfigVersion,
             const char *cpus, char *tty, int vncport)
{
    const char *tmp;
    virDomainDefPtr def;
    int hvm = 0;

    if (VIR_ALLOC(def) < 0)
        goto no_memory;

    tmp = sexpr_node(root, "domain/domid");
    if (tmp == NULL && xendConfigVersion < 3) { /* Old XenD, domid was mandatory */
        XENXS_ERROR(VIR_ERR_INTERNAL_ERROR,
                     "%s", _("domain information incomplete, missing id"));
        goto error;
    }
    def->virtType = VIR_DOMAIN_VIRT_XEN;
    if (tmp)
        def->id = sexpr_int(root, "domain/domid");
    else
        def->id = -1;

    if (sexpr_node_copy(root, "domain/name", &def->name) < 0)
        goto no_memory;
    if (def->name == NULL) {
        XENXS_ERROR(VIR_ERR_INTERNAL_ERROR,
                     "%s", _("domain information incomplete, missing name"));
        goto error;
    }

    tmp = sexpr_node(root, "domain/uuid");
    if (tmp == NULL) {
        XENXS_ERROR(VIR_ERR_INTERNAL_ERROR,
                     "%s", _("domain information incomplete, missing name"));
        goto error;
    }
    if (virUUIDParse(tmp, def->uuid) < 0)
        goto error;

    if (sexpr_node_copy(root, "domain/description", &def->description) < 0)
        goto no_memory;

    hvm = sexpr_lookup(root, "domain/image/hvm") ? 1 : 0;
    if (!hvm) {
        if (sexpr_node_copy(root, "domain/bootloader",
                            &def->os.bootloader) < 0)
            goto no_memory;

        if (!def->os.bootloader &&
            sexpr_has(root, "domain/bootloader") &&
            (def->os.bootloader = strdup("")) == NULL)
            goto no_memory;

        if (def->os.bootloader &&
            sexpr_node_copy(root, "domain/bootloader_args",
                            &def->os.bootloaderArgs) < 0)
            goto no_memory;
    }

    if (!(def->os.type = strdup(hvm ? "hvm" : "linux")))
        goto no_memory;

    if (def->id != 0) {
        if (sexpr_lookup(root, "domain/image")) {
            if (xenParseSxprOS(root, def, hvm) < 0)
                goto error;
        }
    }

    def->mem.max_balloon = (unsigned long)
                           (sexpr_u64(root, "domain/maxmem") << 10);
    def->mem.cur_balloon = (unsigned long)
                           (sexpr_u64(root, "domain/memory") << 10);
    if (def->mem.cur_balloon > def->mem.max_balloon)
        def->mem.cur_balloon = def->mem.max_balloon;

    if (cpus != NULL) {
        def->cpumasklen = VIR_DOMAIN_CPUMASK_LEN;
        if (VIR_ALLOC_N(def->cpumask, def->cpumasklen) < 0) {
            virReportOOMError();
            goto error;
        }

        if (virDomainCpuSetParse(&cpus,
                                 0, def->cpumask,
                                 def->cpumasklen) < 0) {
            XENXS_ERROR(VIR_ERR_INTERNAL_ERROR,
                         _("invalid CPU mask %s"), cpus);
            goto error;
        }
    }

    def->maxvcpus = sexpr_int(root, "domain/vcpus");
    def->vcpus = count_one_bits_l(sexpr_u64(root, "domain/vcpu_avail"));
    if (!def->vcpus || def->maxvcpus < def->vcpus)
        def->vcpus = def->maxvcpus;

    tmp = sexpr_node(root, "domain/on_poweroff");
    if (tmp != NULL) {
        if ((def->onPoweroff = virDomainLifecycleTypeFromString(tmp)) < 0) {
            XENXS_ERROR(VIR_ERR_INTERNAL_ERROR,
                         _("unknown lifecycle type %s"), tmp);
            goto error;
        }
    } else
        def->onPoweroff = VIR_DOMAIN_LIFECYCLE_DESTROY;

    tmp = sexpr_node(root, "domain/on_reboot");
    if (tmp != NULL) {
        if ((def->onReboot = virDomainLifecycleTypeFromString(tmp)) < 0) {
            XENXS_ERROR(VIR_ERR_INTERNAL_ERROR,
                         _("unknown lifecycle type %s"), tmp);
            goto error;
        }
    } else
        def->onReboot = VIR_DOMAIN_LIFECYCLE_RESTART;

    tmp = sexpr_node(root, "domain/on_crash");
    if (tmp != NULL) {
        if ((def->onCrash = virDomainLifecycleCrashTypeFromString(tmp)) < 0) {
            XENXS_ERROR(VIR_ERR_INTERNAL_ERROR,
                         _("unknown lifecycle type %s"), tmp);
            goto error;
        }
    } else
        def->onCrash = VIR_DOMAIN_LIFECYCLE_DESTROY;

    def->clock.offset = VIR_DOMAIN_CLOCK_OFFSET_UTC;
    if (hvm) {
        if (sexpr_int(root, "domain/image/hvm/acpi"))
            def->features |= (1 << VIR_DOMAIN_FEATURE_ACPI);
        if (sexpr_int(root, "domain/image/hvm/apic"))
            def->features |= (1 << VIR_DOMAIN_FEATURE_APIC);
        if (sexpr_int(root, "domain/image/hvm/pae"))
            def->features |= (1 << VIR_DOMAIN_FEATURE_PAE);
        if (sexpr_int(root, "domain/image/hvm/hap"))
            def->features |= (1 << VIR_DOMAIN_FEATURE_HAP);
        if (sexpr_int(root, "domain/image/hvm/viridian"))
            def->features |= (1 << VIR_DOMAIN_FEATURE_VIRIDIAN);

        /* Old XenD only allows localtime here for HVM */
        if (sexpr_int(root, "domain/image/hvm/localtime"))
            def->clock.offset = VIR_DOMAIN_CLOCK_OFFSET_LOCALTIME;

        if (sexpr_lookup(root, "domain/image/hvm/hpet")) {
            virDomainTimerDefPtr timer;

            if (VIR_ALLOC_N(def->clock.timers, 1) < 0 ||
                VIR_ALLOC(timer) < 0) {
                virReportOOMError();
                goto error;
            }

            timer->name = VIR_DOMAIN_TIMER_NAME_HPET;
            timer->present = sexpr_int(root, "domain/image/hvm/hpet");
            timer->tickpolicy = -1;

            def->clock.ntimers = 1;
            def->clock.timers[0] = timer;
        }
    }

    /* Current XenD allows localtime here, for PV and HVM */
    if (sexpr_int(root, "domain/localtime"))
        def->clock.offset = VIR_DOMAIN_CLOCK_OFFSET_LOCALTIME;

    if (sexpr_node_copy(root, hvm ?
                        "domain/image/hvm/device_model" :
                        "domain/image/linux/device_model",
                        &def->emulator) < 0)
        goto no_memory;

    /* append block devices */
    if (xenParseSxprDisks(def, root, hvm, xendConfigVersion) < 0)
        goto error;

    if (xenParseSxprNets(def, root) < 0)
        goto error;

    if (xenParseSxprPCI(def, root) < 0)
        goto error;

    /* New style graphics device config */
    if (xenParseSxprGraphicsNew(def, root, vncport) < 0)
        goto error;

    /* Graphics device (HVM <= 3.0.4, or PV <= 3.0.3) vnc config */
    if ((def->ngraphics == 0) &&
        xenParseSxprGraphicsOld(def, root, hvm, xendConfigVersion,
                                      vncport) < 0)
        goto error;


    /* Old style cdrom config from Xen <= 3.0.2 */
    if (hvm &&
        xendConfigVersion == 1) {
        tmp = sexpr_node(root, "domain/image/hvm/cdrom");
        if ((tmp != NULL) && (tmp[0] != 0)) {
            virDomainDiskDefPtr disk;
            if (VIR_ALLOC(disk) < 0)
                goto no_memory;
            if (!(disk->src = strdup(tmp))) {
                virDomainDiskDefFree(disk);
                goto no_memory;
            }
            disk->type = VIR_DOMAIN_DISK_TYPE_FILE;
            disk->device = VIR_DOMAIN_DISK_DEVICE_CDROM;
            if (!(disk->dst = strdup("hdc"))) {
                virDomainDiskDefFree(disk);
                goto no_memory;
            }
            if (!(disk->driverName = strdup("file"))) {
                virDomainDiskDefFree(disk);
                goto no_memory;
            }
            disk->bus = VIR_DOMAIN_DISK_BUS_IDE;
            disk->readonly = 1;

            if (VIR_REALLOC_N(def->disks, def->ndisks+1) < 0) {
                virDomainDiskDefFree(disk);
                goto no_memory;
            }
            def->disks[def->ndisks++] = disk;
        }
    }


    /* Floppy disk config */
    if (hvm) {
        const char *const fds[] = { "fda", "fdb" };
        int i;
        for (i = 0 ; i < ARRAY_CARDINALITY(fds) ; i++) {
            tmp = sexpr_fmt_node(root, "domain/image/hvm/%s", fds[i]);
            if ((tmp != NULL) && (tmp[0] != 0)) {
                virDomainDiskDefPtr disk;
                if (VIR_ALLOC(disk) < 0)
                    goto no_memory;
                if (!(disk->src = strdup(tmp))) {
                    VIR_FREE(disk);
                    goto no_memory;
                }
                disk->type = VIR_DOMAIN_DISK_TYPE_FILE;
                disk->device = VIR_DOMAIN_DISK_DEVICE_FLOPPY;
                if (!(disk->dst = strdup(fds[i]))) {
                    virDomainDiskDefFree(disk);
                    goto no_memory;
                }
                if (!(disk->driverName = strdup("file"))) {
                    virDomainDiskDefFree(disk);
                    goto no_memory;
                }
                disk->bus = VIR_DOMAIN_DISK_BUS_FDC;

                if (VIR_REALLOC_N(def->disks, def->ndisks+1) < 0) {
                    virDomainDiskDefFree(disk);
                    goto no_memory;
                }
                def->disks[def->ndisks++] = disk;
            }
        }
    }

    /* in case of HVM we have USB device emulation */
    if (hvm &&
        xenParseSxprUSB(def, root) < 0)
        goto error;

    /* Character device config */
    if (hvm) {
        const struct sexpr *serial_root;
        bool have_multiple_serials = false;

        serial_root = sexpr_lookup(root, "domain/image/hvm/serial");
        if (serial_root) {
            const struct sexpr *cur, *node, *cur2;
            int ports_skipped = 0;

            for (cur = serial_root; cur->kind == SEXPR_CONS; cur = cur->u.s.cdr) {
                node = cur->u.s.car;

                for (cur2 = node; cur2->kind == SEXPR_CONS; cur2 = cur2->u.s.cdr) {
                    tmp = cur2->u.s.car->u.value;

                    if (tmp && STRNEQ(tmp, "none")) {
                        virDomainChrDefPtr chr;
                        if ((chr = xenParseSxprChar(tmp, tty)) == NULL)
                            goto error;
                        if (VIR_REALLOC_N(def->serials, def->nserials+1) < 0) {
                            virDomainChrDefFree(chr);
                            goto no_memory;
                        }
                        chr->deviceType = VIR_DOMAIN_CHR_DEVICE_TYPE_SERIAL;
                        chr->target.port = def->nserials + ports_skipped;
                        def->serials[def->nserials++] = chr;
                    }
                    else
                        ports_skipped++;

                    have_multiple_serials = true;
                }
            }
        }

        if (!have_multiple_serials) {
            tmp = sexpr_node(root, "domain/image/hvm/serial");
            if (tmp && STRNEQ(tmp, "none")) {
                virDomainChrDefPtr chr;
                if ((chr = xenParseSxprChar(tmp, tty)) == NULL)
                    goto error;
                if (VIR_REALLOC_N(def->serials, def->nserials+1) < 0) {
                    virDomainChrDefFree(chr);
                    goto no_memory;
                }
                chr->deviceType = VIR_DOMAIN_CHR_DEVICE_TYPE_SERIAL;
                chr->target.port = 0;
                def->serials[def->nserials++] = chr;
            }
        }

        tmp = sexpr_node(root, "domain/image/hvm/parallel");
        if (tmp && STRNEQ(tmp, "none")) {
            virDomainChrDefPtr chr;
            /* XXX does XenD stuff parallel port tty info into xenstore somewhere ? */
            if ((chr = xenParseSxprChar(tmp, NULL)) == NULL)
                goto error;
            if (VIR_REALLOC_N(def->parallels, def->nparallels+1) < 0) {
                virDomainChrDefFree(chr);
                goto no_memory;
            }
            chr->deviceType = VIR_DOMAIN_CHR_DEVICE_TYPE_PARALLEL;
            chr->target.port = 0;
            def->parallels[def->nparallels++] = chr;
        }
    } else {
        /* Fake a paravirt console, since that's not in the sexpr */
        if (!(def->console = xenParseSxprChar("pty", tty)))
            goto error;
        def->console->deviceType = VIR_DOMAIN_CHR_DEVICE_TYPE_CONSOLE;
        def->console->target.port = 0;
        def->console->targetType = VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_XEN;
    }
    VIR_FREE(tty);


    /* Sound device config */
    if (hvm &&
        (tmp = sexpr_node(root, "domain/image/hvm/soundhw")) != NULL &&
        *tmp) {
        if (xenParseSxprSound(def, tmp) < 0)
            goto error;
    }

    return def;

no_memory:
    virReportOOMError();
error:
    VIR_FREE(tty);
    virDomainDefFree(def);
    return NULL;
}

virDomainDefPtr
xenParseSxprString(const char *sexpr,
                         int xendConfigVersion, char *tty, int vncport)
{
    struct sexpr *root = string2sexpr(sexpr);
    virDomainDefPtr def;

    if (!root)
        return NULL;

    def = xenParseSxpr(root, xendConfigVersion, NULL, tty, vncport);

    sexpr_free(root);

    return def;
}

/************************************************************************
 *                                                                      *
 * Converter functions to go from the XML tree to an S-Expr for Xen     *
 *                                                                      *
 ************************************************************************/


/**
 * virtDomainParseXMLGraphicsDescVFB:
 * @conn: pointer to the hypervisor connection
 * @node: node containing graphics description
 * @buf: a buffer for the result S-Expr
 *
 * Parse the graphics part of the XML description and add it to the S-Expr
 * in buf.  This is a temporary interface as the S-Expr interface will be
 * replaced by XML-RPC in the future. However the XML format should stay
 * valid over time.
 *
 * Returns 0 in case of success, -1 in case of error
 */
static int
xenFormatSxprGraphicsNew(virDomainGraphicsDefPtr def,
                         virBufferPtr buf)
{
    const char *listenAddr;

    if (def->type != VIR_DOMAIN_GRAPHICS_TYPE_SDL &&
        def->type != VIR_DOMAIN_GRAPHICS_TYPE_VNC) {
        XENXS_ERROR(VIR_ERR_INTERNAL_ERROR,
                     _("unexpected graphics type %d"),
                     def->type);
        return -1;
    }

    virBufferAddLit(buf, "(device (vkbd))");
    virBufferAddLit(buf, "(device (vfb ");

    if (def->type == VIR_DOMAIN_GRAPHICS_TYPE_SDL) {
        virBufferAddLit(buf, "(type sdl)");
        if (def->data.sdl.display)
            virBufferAsprintf(buf, "(display '%s')", def->data.sdl.display);
        if (def->data.sdl.xauth)
            virBufferAsprintf(buf, "(xauthority '%s')", def->data.sdl.xauth);
    } else if (def->type == VIR_DOMAIN_GRAPHICS_TYPE_VNC) {
        virBufferAddLit(buf, "(type vnc)");
        if (def->data.vnc.autoport) {
            virBufferAddLit(buf, "(vncunused 1)");
        } else {
            virBufferAddLit(buf, "(vncunused 0)");
            virBufferAsprintf(buf, "(vncdisplay %d)", def->data.vnc.port-5900);
        }

        listenAddr = virDomainGraphicsListenGetAddress(def, 0);
        if (listenAddr)
            virBufferAsprintf(buf, "(vnclisten '%s')", listenAddr);
        if (def->data.vnc.auth.passwd)
            virBufferAsprintf(buf, "(vncpasswd '%s')", def->data.vnc.auth.passwd);
        if (def->data.vnc.keymap)
            virBufferAsprintf(buf, "(keymap '%s')", def->data.vnc.keymap);
    }

    virBufferAddLit(buf, "))");

    return 0;
}


static int
xenFormatSxprGraphicsOld(virDomainGraphicsDefPtr def,
                         virBufferPtr buf,
                         int xendConfigVersion)
{
    const char *listenAddr;

    if (def->type != VIR_DOMAIN_GRAPHICS_TYPE_SDL &&
        def->type != VIR_DOMAIN_GRAPHICS_TYPE_VNC) {
        XENXS_ERROR(VIR_ERR_INTERNAL_ERROR,
                     _("unexpected graphics type %d"),
                     def->type);
        return -1;
    }

    if (def->type == VIR_DOMAIN_GRAPHICS_TYPE_SDL) {
        virBufferAddLit(buf, "(sdl 1)");
        if (def->data.sdl.display)
            virBufferAsprintf(buf, "(display '%s')", def->data.sdl.display);
        if (def->data.sdl.xauth)
            virBufferAsprintf(buf, "(xauthority '%s')", def->data.sdl.xauth);
    } else if (def->type == VIR_DOMAIN_GRAPHICS_TYPE_VNC) {
        virBufferAddLit(buf, "(vnc 1)");
        if (xendConfigVersion >= 2) {
            if (def->data.vnc.autoport) {
                virBufferAddLit(buf, "(vncunused 1)");
            } else {
                virBufferAddLit(buf, "(vncunused 0)");
                virBufferAsprintf(buf, "(vncdisplay %d)", def->data.vnc.port-5900);
            }

            listenAddr = virDomainGraphicsListenGetAddress(def, 0);
            if (listenAddr)
                virBufferAsprintf(buf, "(vnclisten '%s')", listenAddr);
            if (def->data.vnc.auth.passwd)
                virBufferAsprintf(buf, "(vncpasswd '%s')", def->data.vnc.auth.passwd);
            if (def->data.vnc.keymap)
                virBufferAsprintf(buf, "(keymap '%s')", def->data.vnc.keymap);

        }
    }

    return 0;
}

int
xenFormatSxprChr(virDomainChrDefPtr def,
                 virBufferPtr buf)
{
    const char *type = virDomainChrTypeToString(def->source.type);

    if (!type) {
        XENXS_ERROR(VIR_ERR_INTERNAL_ERROR,
                     "%s", _("unexpected chr device type"));
        return -1;
    }

    switch (def->source.type) {
    case VIR_DOMAIN_CHR_TYPE_NULL:
    case VIR_DOMAIN_CHR_TYPE_STDIO:
    case VIR_DOMAIN_CHR_TYPE_VC:
    case VIR_DOMAIN_CHR_TYPE_PTY:
        virBufferAdd(buf, type, -1);
        break;

    case VIR_DOMAIN_CHR_TYPE_FILE:
    case VIR_DOMAIN_CHR_TYPE_PIPE:
        virBufferAsprintf(buf, "%s:", type);
        virBufferEscapeSexpr(buf, "%s", def->source.data.file.path);
        break;

    case VIR_DOMAIN_CHR_TYPE_DEV:
        virBufferEscapeSexpr(buf, "%s", def->source.data.file.path);
        break;

    case VIR_DOMAIN_CHR_TYPE_TCP:
        virBufferAsprintf(buf, "%s:%s:%s%s",
                          (def->source.data.tcp.protocol
                           == VIR_DOMAIN_CHR_TCP_PROTOCOL_RAW ?
                           "tcp" : "telnet"),
                          (def->source.data.tcp.host ?
                           def->source.data.tcp.host : ""),
                          (def->source.data.tcp.service ?
                           def->source.data.tcp.service : ""),
                          (def->source.data.tcp.listen ?
                           ",server,nowait" : ""));
        break;

    case VIR_DOMAIN_CHR_TYPE_UDP:
        virBufferAsprintf(buf, "%s:%s:%s@%s:%s", type,
                          (def->source.data.udp.connectHost ?
                           def->source.data.udp.connectHost : ""),
                          (def->source.data.udp.connectService ?
                           def->source.data.udp.connectService : ""),
                          (def->source.data.udp.bindHost ?
                           def->source.data.udp.bindHost : ""),
                          (def->source.data.udp.bindService ?
                           def->source.data.udp.bindService : ""));
        break;

    case VIR_DOMAIN_CHR_TYPE_UNIX:
        virBufferAsprintf(buf, "%s:", type);
        virBufferEscapeSexpr(buf, "%s", def->source.data.nix.path);
        if (def->source.data.nix.listen)
            virBufferAddLit(buf, ",server,nowait");
        break;
    }

    if (virBufferError(buf)) {
        virReportOOMError();
        return -1;
    }

    return 0;
}


/**
 * virDomainParseXMLDiskDesc:
 * @node: node containing disk description
 * @buf: a buffer for the result S-Expr
 * @xendConfigVersion: xend configuration file format
 *
 * Parse the one disk in the XML description and add it to the S-Expr in buf
 * This is a temporary interface as the S-Expr interface
 * will be replaced by XML-RPC in the future. However the XML format should
 * stay valid over time.
 *
 * Returns 0 in case of success, -1 in case of error.
 */
int
xenFormatSxprDisk(virDomainDiskDefPtr def,
                  virBufferPtr buf,
                  int hvm,
                  int xendConfigVersion,
                  int isAttach)
{
    /* Xend (all versions) put the floppy device config
     * under the hvm (image (os)) block
     */
    if (hvm &&
        def->device == VIR_DOMAIN_DISK_DEVICE_FLOPPY) {
        if (isAttach) {
            XENXS_ERROR(VIR_ERR_INVALID_ARG,
                     _("Cannot directly attach floppy %s"), def->src);
            return -1;
        }
        return 0;
    }

    /* Xend <= 3.0.2 doesn't include cdrom config here */
    if (hvm &&
        def->device == VIR_DOMAIN_DISK_DEVICE_CDROM &&
        xendConfigVersion == 1) {
        if (isAttach) {
            XENXS_ERROR(VIR_ERR_INVALID_ARG,
                     _("Cannot directly attach CDROM %s"), def->src);
            return -1;
        }
        return 0;
    }

    if (!isAttach)
        virBufferAddLit(buf, "(device ");

    /* Normally disks are in a (device (vbd ...)) block
     * but blktap disks ended up in a differently named
     * (device (tap ....)) block.... */
    if (def->driverName && STREQ(def->driverName, "tap")) {
        virBufferAddLit(buf, "(tap ");
    } else if (def->driverName && STREQ(def->driverName, "tap2")) {
        virBufferAddLit(buf, "(tap2 ");
    } else {
        virBufferAddLit(buf, "(vbd ");
    }

    if (hvm) {
        /* Xend <= 3.0.2 wants a ioemu: prefix on devices for HVM */
        if (xendConfigVersion == 1) {
            virBufferEscapeSexpr(buf, "(dev 'ioemu:%s')", def->dst);
        } else {
            /* But newer does not */
            virBufferEscapeSexpr(buf, "(dev '%s:", def->dst);
            virBufferAsprintf(buf, "%s')",
                              def->device == VIR_DOMAIN_DISK_DEVICE_CDROM ?
                              "cdrom" : "disk");
        }
    } else if (def->device == VIR_DOMAIN_DISK_DEVICE_CDROM) {
        virBufferEscapeSexpr(buf, "(dev '%s:cdrom')", def->dst);
    } else {
        virBufferEscapeSexpr(buf, "(dev '%s')", def->dst);
    }

    if (def->src) {
        if (def->driverName) {
            if (STREQ(def->driverName, "tap") ||
                STREQ(def->driverName, "tap2")) {
                virBufferEscapeSexpr(buf, "(uname '%s:", def->driverName);
                virBufferEscapeSexpr(buf, "%s:",
                                     def->driverType ? def->driverType : "aio");
                virBufferEscapeSexpr(buf, "%s')", def->src);
            } else {
                virBufferEscapeSexpr(buf, "(uname '%s:", def->driverName);
                virBufferEscapeSexpr(buf, "%s')", def->src);
            }
        } else {
            if (def->type == VIR_DOMAIN_DISK_TYPE_FILE) {
                virBufferEscapeSexpr(buf, "(uname 'file:%s')", def->src);
            } else if (def->type == VIR_DOMAIN_DISK_TYPE_BLOCK) {
                if (def->src[0] == '/')
                    virBufferEscapeSexpr(buf, "(uname 'phy:%s')", def->src);
                else
                    virBufferEscapeSexpr(buf, "(uname 'phy:/dev/%s')",
                                         def->src);
            } else {
                XENXS_ERROR(VIR_ERR_CONFIG_UNSUPPORTED,
                             _("unsupported disk type %s"),
                             virDomainDiskTypeToString(def->type));
                return -1;
            }
        }
    }

    if (def->readonly)
        virBufferAddLit(buf, "(mode 'r')");
    else if (def->shared)
        virBufferAddLit(buf, "(mode 'w!')");
    else
        virBufferAddLit(buf, "(mode 'w')");
    if (def->transient) {
        XENXS_ERROR(VIR_ERR_CONFIG_UNSUPPORTED,
                    _("transient disks not supported yet"));
        return -1;
    }

    if (!isAttach)
        virBufferAddLit(buf, ")");

    virBufferAddLit(buf, ")");

    return 0;
}

/**
 * xenFormatSxprNet
 * @node: node containing the interface description
 * @buf: a buffer for the result S-Expr
 * @xendConfigVersion: xend configuration file format
 *
 * Parse the one interface the XML description and add it to the S-Expr in buf
 * This is a temporary interface as the S-Expr interface
 * will be replaced by XML-RPC in the future. However the XML format should
 * stay valid over time.
 *
 * Returns 0 in case of success, -1 in case of error.
 */
int
xenFormatSxprNet(virConnectPtr conn,
                 virDomainNetDefPtr def,
                 virBufferPtr buf,
                 int hvm,
                 int xendConfigVersion,
                 int isAttach)
{
    const char *script = DEFAULT_VIF_SCRIPT;

    if (def->type != VIR_DOMAIN_NET_TYPE_BRIDGE &&
        def->type != VIR_DOMAIN_NET_TYPE_NETWORK &&
        def->type != VIR_DOMAIN_NET_TYPE_ETHERNET) {
        XENXS_ERROR(VIR_ERR_INTERNAL_ERROR,
                     _("unsupported network type %d"), def->type);
        return -1;
    }

    if (!isAttach)
        virBufferAddLit(buf, "(device ");

    virBufferAddLit(buf, "(vif ");

    virBufferAsprintf(buf,
                      "(mac '%02x:%02x:%02x:%02x:%02x:%02x')",
                      def->mac[0], def->mac[1], def->mac[2],
                      def->mac[3], def->mac[4], def->mac[5]);

    switch (def->type) {
    case VIR_DOMAIN_NET_TYPE_BRIDGE:
        virBufferEscapeSexpr(buf, "(bridge '%s')", def->data.bridge.brname);
        if (def->data.bridge.script)
            script = def->data.bridge.script;

        virBufferEscapeSexpr(buf, "(script '%s')", script);
        if (def->data.bridge.ipaddr != NULL)
            virBufferEscapeSexpr(buf, "(ip '%s')", def->data.bridge.ipaddr);
        break;

    case VIR_DOMAIN_NET_TYPE_NETWORK:
    {
        virNetworkPtr network =
            virNetworkLookupByName(conn, def->data.network.name);
        char *bridge;

        if (!network) {
            XENXS_ERROR(VIR_ERR_NO_NETWORK, "%s",
                         def->data.network.name);
            return -1;
        }

        bridge = virNetworkGetBridgeName(network);
        virNetworkFree(network);
        if (!bridge) {
            XENXS_ERROR(VIR_ERR_INTERNAL_ERROR,
                         _("network %s is not active"),
                         def->data.network.name);
            return -1;
        }
        virBufferEscapeSexpr(buf, "(bridge '%s')", bridge);
        virBufferEscapeSexpr(buf, "(script '%s')", script);
        VIR_FREE(bridge);
    }
    break;

    case VIR_DOMAIN_NET_TYPE_ETHERNET:
        if (def->data.ethernet.script)
            virBufferEscapeSexpr(buf, "(script '%s')",
                                 def->data.ethernet.script);
        if (def->data.ethernet.ipaddr != NULL)
            virBufferEscapeSexpr(buf, "(ip '%s')", def->data.ethernet.ipaddr);
        break;

    case VIR_DOMAIN_NET_TYPE_USER:
    case VIR_DOMAIN_NET_TYPE_SERVER:
    case VIR_DOMAIN_NET_TYPE_CLIENT:
    case VIR_DOMAIN_NET_TYPE_MCAST:
    case VIR_DOMAIN_NET_TYPE_INTERNAL:
    case VIR_DOMAIN_NET_TYPE_DIRECT:
    case VIR_DOMAIN_NET_TYPE_LAST:
        break;
    }

    if (def->ifname != NULL &&
        !STRPREFIX(def->ifname, "vif"))
        virBufferEscapeSexpr(buf, "(vifname '%s')", def->ifname);

    if (!hvm) {
        if (def->model != NULL)
            virBufferEscapeSexpr(buf, "(model '%s')", def->model);
    }
    else if (def->model == NULL) {
        /*
         * apparently (type ioemu) breaks paravirt drivers on HVM so skip
         * this from XEND_CONFIG_MAX_VERS_NET_TYPE_IOEMU
         */
        if (xendConfigVersion <= XEND_CONFIG_MAX_VERS_NET_TYPE_IOEMU)
            virBufferAddLit(buf, "(type ioemu)");
    }
    else if (STREQ(def->model, "netfront")) {
        virBufferAddLit(buf, "(type netfront)");
    }
    else {
        virBufferEscapeSexpr(buf, "(model '%s')", def->model);
        virBufferAddLit(buf, "(type ioemu)");
    }

    if (!isAttach)
        virBufferAddLit(buf, ")");

    virBufferAddLit(buf, ")");

    return 0;
}


static void
xenFormatSxprPCI(virDomainHostdevDefPtr def,
                 virBufferPtr buf)
{
    virBufferAsprintf(buf, "(dev (domain 0x%04x)(bus 0x%02x)(slot 0x%02x)(func 0x%x))",
                      def->source.subsys.u.pci.domain,
                      def->source.subsys.u.pci.bus,
                      def->source.subsys.u.pci.slot,
                      def->source.subsys.u.pci.function);
}

int
xenFormatSxprOnePCI(virDomainHostdevDefPtr def,
                    virBufferPtr buf,
                    int detach)
{
    if (def->managed) {
        XENXS_ERROR(VIR_ERR_NO_SUPPORT, "%s",
                     _("managed PCI devices not supported with XenD"));
        return -1;
    }

    virBufferAddLit(buf, "(pci ");
    xenFormatSxprPCI(def, buf);
    if (detach)
        virBufferAddLit(buf, "(state 'Closing')");
    else
        virBufferAddLit(buf, "(state 'Initialising')");
    virBufferAddLit(buf, ")");

    return 0;
}

static int
xenFormatSxprAllPCI(virDomainDefPtr def,
                    virBufferPtr buf)
{
    int hasPCI = 0;
    int i;

    for (i = 0 ; i < def->nhostdevs ; i++)
        if (def->hostdevs[i]->mode == VIR_DOMAIN_HOSTDEV_MODE_SUBSYS &&
            def->hostdevs[i]->source.subsys.type == VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_PCI)
            hasPCI = 1;

    if (!hasPCI)
        return 0;

    /*
     * With the (domain ...) block we have the following odd setup
     *
     * (device
     *    (pci
     *       (dev (domain 0x0000) (bus 0x00) (slot 0x1b) (func 0x0))
     *       (dev (domain 0x0000) (bus 0x00) (slot 0x13) (func 0x0))
     *    )
     * )
     *
     * Normally there is one (device ...) block per device, but in the
     * weird world of Xen PCI, one (device ...) covers multiple devices.
     */

    virBufferAddLit(buf, "(device (pci ");
    for (i = 0 ; i < def->nhostdevs ; i++) {
        if (def->hostdevs[i]->mode == VIR_DOMAIN_HOSTDEV_MODE_SUBSYS &&
            def->hostdevs[i]->source.subsys.type == VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_PCI) {
            if (def->hostdevs[i]->managed) {
                XENXS_ERROR(VIR_ERR_NO_SUPPORT, "%s",
                             _("managed PCI devices not supported with XenD"));
                return -1;
            }

            xenFormatSxprPCI(def->hostdevs[i], buf);
        }
    }
    virBufferAddLit(buf, "))");

    return 0;
}

int
xenFormatSxprSound(virDomainDefPtr def,
                   virBufferPtr buf)
{
    const char *str;
    int i;

    for (i = 0 ; i < def->nsounds ; i++) {
        if (!(str = virDomainSoundModelTypeToString(def->sounds[i]->model))) {
            XENXS_ERROR(VIR_ERR_INTERNAL_ERROR,
                         _("unexpected sound model %d"),
                         def->sounds[i]->model);
            return -1;
        }
        if (i)
            virBufferAddChar(buf, ',');
        virBufferEscapeSexpr(buf, "%s", str);
    }

    if (virBufferError(buf)) {
        virReportOOMError();
        return -1;
    }

    return 0;
}


static int
xenFormatSxprInput(virDomainInputDefPtr input,
                   virBufferPtr buf)
{
    if (input->bus != VIR_DOMAIN_INPUT_BUS_USB)
        return 0;

    if (input->type != VIR_DOMAIN_INPUT_TYPE_MOUSE &&
        input->type != VIR_DOMAIN_INPUT_TYPE_TABLET) {
        XENXS_ERROR(VIR_ERR_INTERNAL_ERROR,
                     _("unexpected input type %d"), input->type);
        return -1;
    }

    virBufferAsprintf(buf, "(usbdevice %s)",
                      input->type == VIR_DOMAIN_INPUT_TYPE_MOUSE ?
                      "mouse" : "tablet");

    return 0;
}


/* Computing the vcpu_avail bitmask works because MAX_VIRT_CPUS is
   either 32, or 64 on a platform where long is big enough.  */
verify(MAX_VIRT_CPUS <= sizeof(1UL) * CHAR_BIT);

/**
 * xenFormatSxpr:
 * @conn: pointer to the hypervisor connection
 * @def: domain config definition
 * @xendConfigVersion: xend configuration file format
 *
 * Generate an SEXPR representing the domain configuration.
 *
 * Returns the 0 terminated S-Expr string or NULL in case of error.
 *         the caller must free() the returned value.
 */
char *
xenFormatSxpr(virConnectPtr conn,
              virDomainDefPtr def,
              int xendConfigVersion)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    char uuidstr[VIR_UUID_STRING_BUFLEN];
    const char *tmp;
    char *bufout;
    int hvm = 0, i;

    VIR_DEBUG("Formatting domain sexpr");

    virBufferAddLit(&buf, "(vm ");
    virBufferEscapeSexpr(&buf, "(name '%s')", def->name);
    virBufferAsprintf(&buf, "(memory %lu)(maxmem %lu)",
                      VIR_DIV_UP(def->mem.cur_balloon, 1024),
                      VIR_DIV_UP(def->mem.max_balloon, 1024));
    virBufferAsprintf(&buf, "(vcpus %u)", def->maxvcpus);
    /* Computing the vcpu_avail bitmask works because MAX_VIRT_CPUS is
       either 32, or 64 on a platform where long is big enough.  */
    if (def->vcpus < def->maxvcpus)
        virBufferAsprintf(&buf, "(vcpu_avail %lu)", (1UL << def->vcpus) - 1);

    if (def->cpumask) {
        char *ranges = virDomainCpuSetFormat(def->cpumask, def->cpumasklen);
        if (ranges == NULL)
            goto error;
        virBufferEscapeSexpr(&buf, "(cpus '%s')", ranges);
        VIR_FREE(ranges);
    }

    virUUIDFormat(def->uuid, uuidstr);
    virBufferAsprintf(&buf, "(uuid '%s')", uuidstr);

    if (def->description)
        virBufferEscapeSexpr(&buf, "(description '%s')", def->description);

    if (def->os.bootloader) {
        if (def->os.bootloader[0])
            virBufferEscapeSexpr(&buf, "(bootloader '%s')", def->os.bootloader);
        else
            virBufferAddLit(&buf, "(bootloader)");

        if (def->os.bootloaderArgs)
            virBufferEscapeSexpr(&buf, "(bootloader_args '%s')", def->os.bootloaderArgs);
    }

    if (!(tmp = virDomainLifecycleTypeToString(def->onPoweroff))) {
        XENXS_ERROR(VIR_ERR_INTERNAL_ERROR,
                     _("unexpected lifecycle value %d"), def->onPoweroff);
        goto error;
    }
    virBufferAsprintf(&buf, "(on_poweroff '%s')", tmp);

    if (!(tmp = virDomainLifecycleTypeToString(def->onReboot))) {
        XENXS_ERROR(VIR_ERR_INTERNAL_ERROR,
                     _("unexpected lifecycle value %d"), def->onReboot);
        goto error;
    }
    virBufferAsprintf(&buf, "(on_reboot '%s')", tmp);

    if (!(tmp = virDomainLifecycleCrashTypeToString(def->onCrash))) {
        XENXS_ERROR(VIR_ERR_INTERNAL_ERROR,
                     _("unexpected lifecycle value %d"), def->onCrash);
        goto error;
    }
    virBufferAsprintf(&buf, "(on_crash '%s')", tmp);

    /* Set localtime here for current XenD (both PV & HVM) */
    if (def->clock.offset == VIR_DOMAIN_CLOCK_OFFSET_LOCALTIME) {
        if (def->clock.data.timezone) {
            XENXS_ERROR(VIR_ERR_CONFIG_UNSUPPORTED,
                         "%s", _("configurable timezones are not supported"));
            goto error;
        }

        virBufferAddLit(&buf, "(localtime 1)");
    } else if (def->clock.offset != VIR_DOMAIN_CLOCK_OFFSET_UTC) {
        XENXS_ERROR(VIR_ERR_CONFIG_UNSUPPORTED,
                     _("unsupported clock offset '%s'"),
                     virDomainClockOffsetTypeToString(def->clock.offset));
        goto error;
    }

    if (!def->os.bootloader) {
        if (STREQ(def->os.type, "hvm"))
            hvm = 1;

        if (hvm)
            virBufferAddLit(&buf, "(image (hvm ");
        else
            virBufferAddLit(&buf, "(image (linux ");

        if (hvm &&
            def->os.loader == NULL) {
            XENXS_ERROR(VIR_ERR_INTERNAL_ERROR,
                         "%s",_("no HVM domain loader"));
            goto error;
        }

        if (def->os.kernel)
            virBufferEscapeSexpr(&buf, "(kernel '%s')", def->os.kernel);
        if (def->os.initrd)
            virBufferEscapeSexpr(&buf, "(ramdisk '%s')", def->os.initrd);
        if (def->os.root)
            virBufferEscapeSexpr(&buf, "(root '%s')", def->os.root);
        if (def->os.cmdline)
            virBufferEscapeSexpr(&buf, "(args '%s')", def->os.cmdline);

        if (hvm) {
            char bootorder[VIR_DOMAIN_BOOT_LAST+1];
            if (def->os.kernel)
                virBufferEscapeSexpr(&buf, "(loader '%s')", def->os.loader);
            else
                virBufferEscapeSexpr(&buf, "(kernel '%s')", def->os.loader);

            virBufferAsprintf(&buf, "(vcpus %u)", def->maxvcpus);
            if (def->vcpus < def->maxvcpus)
                virBufferAsprintf(&buf, "(vcpu_avail %lu)",
                                  (1UL << def->vcpus) - 1);

            for (i = 0 ; i < def->os.nBootDevs ; i++) {
                switch (def->os.bootDevs[i]) {
                case VIR_DOMAIN_BOOT_FLOPPY:
                    bootorder[i] = 'a';
                    break;
                default:
                case VIR_DOMAIN_BOOT_DISK:
                    bootorder[i] = 'c';
                    break;
                case VIR_DOMAIN_BOOT_CDROM:
                    bootorder[i] = 'd';
                    break;
                case VIR_DOMAIN_BOOT_NET:
                    bootorder[i] = 'n';
                    break;
                }
            }
            if (def->os.nBootDevs == 0) {
                bootorder[0] = 'c';
                bootorder[1] = '\0';
            } else {
                bootorder[def->os.nBootDevs] = '\0';
            }
            virBufferAsprintf(&buf, "(boot %s)", bootorder);

            /* some disk devices are defined here */
            for (i = 0 ; i < def->ndisks ; i++) {
                switch (def->disks[i]->device) {
                case VIR_DOMAIN_DISK_DEVICE_CDROM:
                    /* Only xend <= 3.0.2 wants cdrom config here */
                    if (xendConfigVersion != 1)
                        break;
                    if (!STREQ(def->disks[i]->dst, "hdc") ||
                        def->disks[i]->src == NULL)
                        break;

                    virBufferEscapeSexpr(&buf, "(cdrom '%s')",
                                         def->disks[i]->src);
                    break;

                case VIR_DOMAIN_DISK_DEVICE_FLOPPY:
                    /* all xend versions define floppies here */
                    virBufferEscapeSexpr(&buf, "(%s ", def->disks[i]->dst);
                    virBufferEscapeSexpr(&buf, "'%s')", def->disks[i]->src);
                    break;

                default:
                    break;
                }
            }

            if (def->features & (1 << VIR_DOMAIN_FEATURE_ACPI))
                virBufferAddLit(&buf, "(acpi 1)");
            if (def->features & (1 << VIR_DOMAIN_FEATURE_APIC))
                virBufferAddLit(&buf, "(apic 1)");
            if (def->features & (1 << VIR_DOMAIN_FEATURE_PAE))
                virBufferAddLit(&buf, "(pae 1)");
            if (def->features & (1 << VIR_DOMAIN_FEATURE_HAP))
                virBufferAddLit(&buf, "(hap 1)");
            if (def->features & (1 << VIR_DOMAIN_FEATURE_VIRIDIAN))
                virBufferAddLit(&buf, "(viridian 1)");

            virBufferAddLit(&buf, "(usb 1)");

            for (i = 0 ; i < def->ninputs ; i++)
                if (xenFormatSxprInput(def->inputs[i], &buf) < 0)
                    goto error;

            if (def->parallels) {
                virBufferAddLit(&buf, "(parallel ");
                if (xenFormatSxprChr(def->parallels[0], &buf) < 0)
                    goto error;
                virBufferAddLit(&buf, ")");
            } else {
                virBufferAddLit(&buf, "(parallel none)");
            }
            if (def->serials) {
                if ((def->nserials > 1) || (def->serials[0]->target.port != 0)) {
                    int maxport = -1;
                    int j = 0;

                    virBufferAddLit(&buf, "(serial (");
                    for (i = 0; i < def->nserials; i++)
                        if (def->serials[i]->target.port > maxport)
                            maxport = def->serials[i]->target.port;

                    for (i = 0; i <= maxport; i++) {
                        virDomainChrDefPtr chr = NULL;

                        if (i)
                            virBufferAddLit(&buf, " ");
                        for (j = 0; j < def->nserials; j++) {
                            if (def->serials[j]->target.port == i) {
                                chr = def->serials[j];
                                break;
                            }
                        }
                        if (chr) {
                            if (xenFormatSxprChr(chr, &buf) < 0)
                                goto error;
                        } else {
                            virBufferAddLit(&buf, "none");
                        }
                    }
                    virBufferAddLit(&buf, "))");
                }
                else {
                    virBufferAddLit(&buf, "(serial ");
                    if (xenFormatSxprChr(def->serials[0], &buf) < 0)
                        goto error;
                    virBufferAddLit(&buf, ")");
                }
            } else {
                virBufferAddLit(&buf, "(serial none)");
            }

            /* Set localtime here to keep old XenD happy for HVM */
            if (def->clock.offset == VIR_DOMAIN_CLOCK_OFFSET_LOCALTIME)
                virBufferAddLit(&buf, "(localtime 1)");

            if (def->sounds) {
                virBufferAddLit(&buf, "(soundhw '");
                if (xenFormatSxprSound(def, &buf) < 0)
                    goto error;
                virBufferAddLit(&buf, "')");
            }
        }

        /* get the device emulation model */
        if (def->emulator && (hvm || xendConfigVersion >= 3))
            virBufferEscapeSexpr(&buf, "(device_model '%s')", def->emulator);

        /* look for HPET in order to override the hypervisor/xend default */
        for (i = 0; i < def->clock.ntimers; i++) {
            if (def->clock.timers[i]->name == VIR_DOMAIN_TIMER_NAME_HPET &&
                def->clock.timers[i]->present != -1) {
                virBufferAsprintf(&buf, "(hpet %d)",
                                  def->clock.timers[i]->present);
                break;
            }
        }

        /* PV graphics for xen <= 3.0.4, or HVM graphics for xen <= 3.1.0 */
        if ((!hvm && xendConfigVersion < XEND_CONFIG_MIN_VERS_PVFB_NEWCONF) ||
            (hvm && xendConfigVersion < 4)) {
            if ((def->ngraphics == 1) &&
                xenFormatSxprGraphicsOld(def->graphics[0],
                                         &buf, xendConfigVersion) < 0)
                goto error;
        }

        virBufferAddLit(&buf, "))");
    } else {
        /* PV domains accept kernel cmdline args */
        if (def->os.cmdline) {
            virBufferEscapeSexpr(&buf, "(image (linux (args '%s')))",
                                 def->os.cmdline);
        }
    }

    for (i = 0 ; i < def->ndisks ; i++)
        if (xenFormatSxprDisk(def->disks[i],
                              &buf, hvm, xendConfigVersion, 0) < 0)
            goto error;

    for (i = 0 ; i < def->nnets ; i++)
        if (xenFormatSxprNet(conn, def->nets[i],
                             &buf, hvm, xendConfigVersion, 0) < 0)
            goto error;

    if (xenFormatSxprAllPCI(def, &buf) < 0)
        goto error;

    /* New style PV graphics config xen >= 3.0.4,
     * or HVM graphics config xen >= 3.0.5 */
    if ((xendConfigVersion >= XEND_CONFIG_MIN_VERS_PVFB_NEWCONF && !hvm) ||
        (xendConfigVersion >= 4 && hvm)) {
        if ((def->ngraphics == 1) &&
            xenFormatSxprGraphicsNew(def->graphics[0], &buf) < 0)
            goto error;
    }

    virBufferAddLit(&buf, ")"); /* closes (vm */

    if (virBufferError(&buf)) {
        virReportOOMError();
        goto error;
    }

    bufout = virBufferContentAndReset(&buf);
    VIR_DEBUG("Formatted sexpr: \n%s", bufout);
    return bufout;

error:
    virBufferFreeAndReset(&buf);
    return NULL;
}
