/*
 * qemu_hostdev.c: QEMU hostdev management
 *
 * Copyright (C) 2006-2007, 2009-2011 Red Hat, Inc.
 * Copyright (C) 2006 Daniel P. Berrange
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

#include "qemu_hostdev.h"
#include "logging.h"
#include "virterror_internal.h"
#include "memory.h"
#include "pci.h"
#include "hostusb.h"

static pciDeviceList *
qemuGetPciHostDeviceList(virDomainHostdevDefPtr *hostdevs, int nhostdevs)
{
    pciDeviceList *list;
    int i;

    if (!(list = pciDeviceListNew()))
        return NULL;

    for (i = 0 ; i < nhostdevs ; i++) {
        virDomainHostdevDefPtr hostdev = hostdevs[i];
        pciDevice *dev;

        if (hostdev->mode != VIR_DOMAIN_HOSTDEV_MODE_SUBSYS)
            continue;
        if (hostdev->source.subsys.type != VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_PCI)
            continue;

        dev = pciGetDevice(hostdev->source.subsys.u.pci.domain,
                           hostdev->source.subsys.u.pci.bus,
                           hostdev->source.subsys.u.pci.slot,
                           hostdev->source.subsys.u.pci.function);
        if (!dev) {
            pciDeviceListFree(list);
            return NULL;
        }

        if (pciDeviceListAdd(list, dev) < 0) {
            pciFreeDevice(dev);
            pciDeviceListFree(list);
            return NULL;
        }

        pciDeviceSetManaged(dev, hostdev->managed);
    }

    return list;
}


int qemuUpdateActivePciHostdevs(struct qemud_driver *driver,
                                virDomainDefPtr def)
{
    pciDeviceList *pcidevs;
    int ret = -1;

    if (!def->nhostdevs)
        return 0;

    if (!(pcidevs = qemuGetPciHostDeviceList(def->hostdevs, def->nhostdevs)))
        return -1;

    while (pciDeviceListCount(pcidevs) > 0) {
        pciDevice *dev = pciDeviceListGet(pcidevs, 0);
        pciDeviceListSteal(pcidevs, dev);
        if (pciDeviceListAdd(driver->activePciHostdevs, dev) < 0) {
            pciFreeDevice(dev);
            goto cleanup;
        }
    }

    ret = 0;

cleanup:
    pciDeviceListFree(pcidevs);
    return ret;
}



int qemuPrepareHostdevPCIDevices(struct qemud_driver *driver,
                                 const char *name,
                                 virDomainHostdevDefPtr *hostdevs,
                                 int nhostdevs)
{
    pciDeviceList *pcidevs;
    int i;
    int ret = -1;

    if (!(pcidevs = qemuGetPciHostDeviceList(hostdevs, nhostdevs)))
        return -1;

    /* We have to use 6 loops here. *All* devices must
     * be detached before we reset any of them, because
     * in some cases you have to reset the whole PCI,
     * which impacts all devices on it. Also, all devices
     * must be reset before being marked as active.
     */

    /* Loop 1: validate that non-managed device isn't in use, eg
     * by checking that device is either un-bound, or bound
     * to pci-stub.ko
     */

    for (i = 0; i < pciDeviceListCount(pcidevs); i++) {
        pciDevice *dev = pciDeviceListGet(pcidevs, i);
        pciDevice *other;

        if (!pciDeviceIsAssignable(dev, !driver->relaxedACS)) {
            qemuReportError(VIR_ERR_OPERATION_INVALID,
                            _("PCI device %s is not assignable"),
                            pciDeviceGetName(dev));
            goto cleanup;
        }
        /* The device is in use by other active domain if
         * the dev is in list driver->activePciHostdevs.
         */
        if ((other = pciDeviceListFind(driver->activePciHostdevs, dev))) {
            const char *other_name = pciDeviceGetUsedBy(other);

            if (other_name)
                qemuReportError(VIR_ERR_OPERATION_INVALID,
                                _("PCI device %s is in use by domain %s"),
                                pciDeviceGetName(dev), other_name);
            else
                qemuReportError(VIR_ERR_OPERATION_INVALID,
                                _("PCI device %s is already in use"),
                                pciDeviceGetName(dev));
            goto cleanup;
        }
    }

    /* Loop 2: detach managed devices */
    for (i = 0; i < pciDeviceListCount(pcidevs); i++) {
        pciDevice *dev = pciDeviceListGet(pcidevs, i);
        if (pciDeviceGetManaged(dev) &&
            pciDettachDevice(dev, driver->activePciHostdevs) < 0)
            goto reattachdevs;
    }

    /* Loop 3: Now that all the PCI hostdevs have been detached, we
     * can safely reset them */
    for (i = 0; i < pciDeviceListCount(pcidevs); i++) {
        pciDevice *dev = pciDeviceListGet(pcidevs, i);
        if (pciResetDevice(dev, driver->activePciHostdevs, pcidevs) < 0)
            goto reattachdevs;
    }

    /* Loop 4: Now mark all the devices as active */
    for (i = 0; i < pciDeviceListCount(pcidevs); i++) {
        pciDevice *dev = pciDeviceListGet(pcidevs, i);
        if (pciDeviceListAdd(driver->activePciHostdevs, dev) < 0) {
            pciFreeDevice(dev);
            goto inactivedevs;
        }
    }

    /* Loop 5: Now set the used_by_domain of the device in
     * driver->activePciHostdevs as domain name.
     */
    for (i = 0; i < pciDeviceListCount(pcidevs); i++) {
        pciDevice *dev, *activeDev;

        dev = pciDeviceListGet(pcidevs, i);
        activeDev = pciDeviceListFind(driver->activePciHostdevs, dev);

        pciDeviceSetUsedBy(activeDev, name);
    }

    /* Loop 6: Now steal all the devices from pcidevs */
    while (pciDeviceListCount(pcidevs) > 0) {
        pciDevice *dev = pciDeviceListGet(pcidevs, 0);
        pciDeviceListSteal(pcidevs, dev);
    }

    ret = 0;
    goto cleanup;

inactivedevs:
    /* Only steal all the devices from driver->activePciHostdevs. We will
     * free them in pciDeviceListFree().
     */
    while (pciDeviceListCount(pcidevs) > 0) {
        pciDevice *dev = pciDeviceListGet(pcidevs, 0);
        pciDeviceListSteal(driver->activePciHostdevs, dev);
    }

reattachdevs:
    for (i = 0; i < pciDeviceListCount(pcidevs); i++) {
        pciDevice *dev = pciDeviceListGet(pcidevs, i);
        pciReAttachDevice(dev, driver->activePciHostdevs);
    }

cleanup:
    pciDeviceListFree(pcidevs);
    return ret;
}

static int
qemuPrepareHostPCIDevices(struct qemud_driver *driver,
                          virDomainDefPtr def)
{
    return qemuPrepareHostdevPCIDevices(driver, def->name, def->hostdevs, def->nhostdevs);
}


static int
qemuPrepareHostUSBDevices(struct qemud_driver *driver ATTRIBUTE_UNUSED,
                          virDomainDefPtr def)
{
    int i;
    for (i = 0 ; i < def->nhostdevs ; i++) {
        virDomainHostdevDefPtr hostdev = def->hostdevs[i];

        if (hostdev->mode != VIR_DOMAIN_HOSTDEV_MODE_SUBSYS)
            continue;
        if (hostdev->source.subsys.type != VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_USB)
            continue;

        /* Resolve a vendor/product to bus/device */
        if (hostdev->source.subsys.u.usb.vendor) {
            usbDevice *usb
                = usbFindDevice(hostdev->source.subsys.u.usb.vendor,
                                hostdev->source.subsys.u.usb.product);

            if (!usb)
                return -1;

            hostdev->source.subsys.u.usb.bus = usbDeviceGetBus(usb);
            hostdev->source.subsys.u.usb.device = usbDeviceGetDevno(usb);

            usbFreeDevice(usb);
        }
    }

    return 0;
}


int qemuPrepareHostDevices(struct qemud_driver *driver,
                           virDomainDefPtr def)
{
    if (!def->nhostdevs)
        return 0;

    if (qemuPrepareHostPCIDevices(driver, def) < 0)
        return -1;

    if (qemuPrepareHostUSBDevices(driver, def) < 0)
        return -1;

    return 0;
}


void qemuReattachPciDevice(pciDevice *dev, struct qemud_driver *driver)
{
    int retries = 100;

    while (pciWaitForDeviceCleanup(dev, "kvm_assigned_device")
           && retries) {
        usleep(100*1000);
        retries--;
    }

    if (pciDeviceGetManaged(dev)) {
        if (pciReAttachDevice(dev, driver->activePciHostdevs) < 0) {
            virErrorPtr err = virGetLastError();
            VIR_ERROR(_("Failed to re-attach PCI device: %s"),
                      err ? err->message : _("unknown error"));
            virResetError(err);
        }
    }
}


void qemuDomainReAttachHostdevDevices(struct qemud_driver *driver,
                                      const char *name,
                                      virDomainHostdevDefPtr *hostdevs,
                                      int nhostdevs)
{
    pciDeviceList *pcidevs;
    int i;

    if (!(pcidevs = qemuGetPciHostDeviceList(hostdevs, nhostdevs))) {
        virErrorPtr err = virGetLastError();
        VIR_ERROR(_("Failed to allocate pciDeviceList: %s"),
                  err ? err->message : _("unknown error"));
        virResetError(err);
        return;
    }

    /* Again 3 loops; mark all devices as inactive before reset
     * them and reset all the devices before re-attach */

    for (i = 0; i < pciDeviceListCount(pcidevs); i++) {
        pciDevice *dev = pciDeviceListGet(pcidevs, i);
        pciDevice *activeDev = NULL;

        /* Never delete the dev from list driver->activePciHostdevs
         * if it's used by other domain.
         */
        activeDev = pciDeviceListFind(driver->activePciHostdevs, dev);
        if (activeDev &&
            STRNEQ_NULLABLE(name, pciDeviceGetUsedBy(activeDev)))
            continue;

        pciDeviceListDel(driver->activePciHostdevs, dev);
    }

    for (i = 0; i < pciDeviceListCount(pcidevs); i++) {
        pciDevice *dev = pciDeviceListGet(pcidevs, i);
        if (pciResetDevice(dev, driver->activePciHostdevs, pcidevs) < 0) {
            virErrorPtr err = virGetLastError();
            VIR_ERROR(_("Failed to reset PCI device: %s"),
                      err ? err->message : _("unknown error"));
            virResetError(err);
        }
    }

    for (i = 0; i < pciDeviceListCount(pcidevs); i++) {
        pciDevice *dev = pciDeviceListGet(pcidevs, i);
        qemuReattachPciDevice(dev, driver);
    }

    pciDeviceListFree(pcidevs);
}


void qemuDomainReAttachHostDevices(struct qemud_driver *driver,
                                   virDomainDefPtr def)
{
    if (!def->nhostdevs)
        return;

    qemuDomainReAttachHostdevDevices(driver, def->name, def->hostdevs, def->nhostdevs);
}
