/*
 * ch_hotplug.c: CH device hotplug handling
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
 * License along with this library.  If not, see
 * <http://www.gnu.org/licenses/>.
 */

#include <config.h>

#include "ch_hotplug.h"
#include "ch_alias.h"
#include "ch_domain.h"
#include "ch_pci_addr.h"
#include "ch_process.h"
#include "domain_event.h"
#include "domain_interface.h"
#include "domain_postparse.h"
#include "domain_validate.h"
#include "virlog.h"

#define VIR_FROM_THIS VIR_FROM_CH

VIR_LOG_INIT("ch.ch_hotplug");

static int
chDomainAddDisk(virCHMonitor *mon,
                virDomainObj *vm,
                virDomainDiskDef *disk)
{
    if (chAssignDeviceDiskAlias(disk) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                    _("Assigning disk alias failed"));
        return -1;
    }

    if (chEnsurePciAddress(vm, &disk->info) < 0)
        return -1;

    if (virCHMonitorAddDisk(mon, disk) < 0) {
        chDomainReleaseDeviceAddress(vm, &disk->info);
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                    _("Adding disk to domain failed"));
        return -1;
    }

    virDomainDiskInsert(vm->def, disk);

    return 0;
}


static int
chDomainAddNet(virCHDriver *driver,
               virCHMonitor *mon,
               virDomainObj *vm,
               virDomainNetDef *net)
{
    chAssignDeviceNetAlias(vm->def, net);

    if (chEnsurePciAddress(vm, &net->info) < 0)
        return -1;

    if (chProcessAddNetworkDevice(driver, mon, vm->def, net, NULL, NULL) < 0) {
        chDomainReleaseDeviceAddress(vm, &net->info);
        return -1;
    }

    virDomainNetInsert(vm->def, net);

    return 0;
}


static int
chDomainAttachDeviceLive(virCHDriver *driver,
                         virDomainObj *vm,
                         virDomainDeviceDef *dev)
{
    int ret = -1;
    virCHDomainObjPrivate *priv = vm->privateData;
    virCHMonitor *mon = priv->monitor;
    const char *alias = NULL;

    switch (dev->type) {
    case VIR_DOMAIN_DEVICE_DISK:
        if (chDomainAddDisk(mon, vm, dev->data.disk) < 0) {
            break;
        }

        alias = dev->data.disk->info.alias;
        dev->data.disk = NULL;
        ret = 0;
        break;

    case VIR_DOMAIN_DEVICE_NET:
        if (chDomainAddNet(driver, mon, vm, dev->data.net) < 0) {
            break;
        }

        alias = dev->data.net->info.alias;
        dev->data.net = NULL;
        ret = 0;
        break;

    case VIR_DOMAIN_DEVICE_LEASE:
    case VIR_DOMAIN_DEVICE_FS:
    case VIR_DOMAIN_DEVICE_INPUT:
    case VIR_DOMAIN_DEVICE_HOSTDEV:
    case VIR_DOMAIN_DEVICE_WATCHDOG:
    case VIR_DOMAIN_DEVICE_CONTROLLER:
    case VIR_DOMAIN_DEVICE_REDIRDEV:
    case VIR_DOMAIN_DEVICE_CHR:
    case VIR_DOMAIN_DEVICE_RNG:
    case VIR_DOMAIN_DEVICE_SHMEM:
    case VIR_DOMAIN_DEVICE_MEMORY:
    case VIR_DOMAIN_DEVICE_VSOCK:
    case VIR_DOMAIN_DEVICE_NONE:
    case VIR_DOMAIN_DEVICE_SOUND:
    case VIR_DOMAIN_DEVICE_VIDEO:
    case VIR_DOMAIN_DEVICE_GRAPHICS:
    case VIR_DOMAIN_DEVICE_HUB:
    case VIR_DOMAIN_DEVICE_SMARTCARD:
    case VIR_DOMAIN_DEVICE_MEMBALLOON:
    case VIR_DOMAIN_DEVICE_NVRAM:
    case VIR_DOMAIN_DEVICE_TPM:
    case VIR_DOMAIN_DEVICE_PANIC:
    case VIR_DOMAIN_DEVICE_IOMMU:
    case VIR_DOMAIN_DEVICE_AUDIO:
    case VIR_DOMAIN_DEVICE_CRYPTO:
    case VIR_DOMAIN_DEVICE_PSTORE:
    case VIR_DOMAIN_DEVICE_LAST:
    default:
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED,
                       _("live attach of device '%1$s' is not supported"),
                       virDomainDeviceTypeToString(dev->type));
        break;
    }

    if (alias) {
        virObjectEvent *event;

        event = virDomainEventDeviceAddedNewFromObj(vm, alias);
        virObjectEventStateQueue(driver->domainEventState, event);
    }

    return ret;
}

static int
chDomainAttachDeviceConfig(virDomainDef *vmdef,
                           virDomainDeviceDef *dev,
                           unsigned int parse_flags,
                           virDomainXMLOption *xmlopt)
{
    virDomainDiskDef *disk;

    switch (dev->type) {
    case VIR_DOMAIN_DEVICE_DISK:
        disk = dev->data.disk;
        if (virDomainDiskIndexByName(vmdef, disk->dst, true) >= 0) {
            virReportError(VIR_ERR_OPERATION_INVALID,
                           _("target %1$s already exists"), disk->dst);
            return -1;
        }

        if (virDomainDiskTranslateSourcePool(disk) < 0)
            return -1;

        virDomainDiskInsert(vmdef, disk);
        dev->data.disk = NULL;
        break;

    case VIR_DOMAIN_DEVICE_NET:
        virDomainNetInsert(vmdef, g_steal_pointer(&dev->data.net));
        break;

    case VIR_DOMAIN_DEVICE_SOUND:
    case VIR_DOMAIN_DEVICE_HOSTDEV:
    case VIR_DOMAIN_DEVICE_LEASE:
    case VIR_DOMAIN_DEVICE_CONTROLLER:
    case VIR_DOMAIN_DEVICE_CHR:
    case VIR_DOMAIN_DEVICE_FS:
    case VIR_DOMAIN_DEVICE_RNG:
    case VIR_DOMAIN_DEVICE_MEMORY:
    case VIR_DOMAIN_DEVICE_REDIRDEV:
    case VIR_DOMAIN_DEVICE_SHMEM:
    case VIR_DOMAIN_DEVICE_WATCHDOG:
    case VIR_DOMAIN_DEVICE_INPUT:
    case VIR_DOMAIN_DEVICE_VSOCK:
    case VIR_DOMAIN_DEVICE_IOMMU:
    case VIR_DOMAIN_DEVICE_VIDEO:
    case VIR_DOMAIN_DEVICE_GRAPHICS:
    case VIR_DOMAIN_DEVICE_HUB:
    case VIR_DOMAIN_DEVICE_SMARTCARD:
    case VIR_DOMAIN_DEVICE_MEMBALLOON:
    case VIR_DOMAIN_DEVICE_NVRAM:
    case VIR_DOMAIN_DEVICE_NONE:
    case VIR_DOMAIN_DEVICE_TPM:
    case VIR_DOMAIN_DEVICE_PANIC:
    case VIR_DOMAIN_DEVICE_AUDIO:
    case VIR_DOMAIN_DEVICE_CRYPTO:
    case VIR_DOMAIN_DEVICE_PSTORE:
    case VIR_DOMAIN_DEVICE_LAST:
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED,
                       _("persistent attach of device '%1$s' is not supported"),
                       virDomainDeviceTypeToString(dev->type));
        return -1;
    }

    if (virDomainDefPostParse(vmdef, parse_flags, xmlopt, NULL) < 0)
        return -1;

    if (virDomainDefValidate(vmdef, parse_flags, xmlopt, NULL) < 0)
        return -1;

    return 0;
}

static void
chDomainAttachDeviceLiveAndConfigHomogenize(const virDomainDeviceDef *devConf,
                                            virDomainDeviceDef *devLive)
{
    if (devConf->type == VIR_DOMAIN_DEVICE_NET)
        virMacAddrSet(&devLive->data.net->mac, &devConf->data.net->mac);
}

static int
chSyncDiskAddressesBetweenConfigs(virDomainDef *dest,
                                  virDomainDef *source,
                                  virDomainDiskDef *disk)
{
    virDomainDiskDef *diskLive = NULL;
    virDomainDiskDef *diskPers = NULL;

    if (!(diskLive = virDomainDiskByName(source, disk->dst, true))) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       _("target %1$s does not exists in live configuration"),
                       disk->dst);
        return -1;
    }

    if (!(diskPers = virDomainDiskByName(dest, disk->dst, true))) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       _("target %1$s does not exists in persistent configuration"),
                       disk->dst);
        return -1;
    }

    if (diskLive->info.type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI) {
        diskPers->info.type = diskLive->info.type;
        diskPers->info.addr = diskLive->info.addr;
    }

    return 0;
}

static int
chSyncNetAddressesBetweenConfigs(virDomainDef *dest,
                                 virDomainDef *source,
                                 virDomainNetDef *net)
{
    virDomainNetDef *netLive = NULL;
    virDomainNetDef *netPers = NULL;

    if (!net->ifname)
        return 0;

    if (!(netLive = virDomainNetFindByName(source, net->ifname)))
        return 0;

    if (!(netPers = virDomainNetFindByName(dest, net->ifname)))
        return 0;

    if (netLive->info.type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI) {
        netPers->info.type = netLive->info.type;
        netPers->info.addr = netLive->info.addr;
    }

    return 0;
}

static int
chSyncDeviceAddressLiveAndPersistent(virDomainDef *dest,
                                     virDomainDef *source,
                                     virDomainDeviceDef *dev)
{
    switch (dev->type) {
    case VIR_DOMAIN_DEVICE_DISK:
        return chSyncDiskAddressesBetweenConfigs(dest, source, dev->data.disk);

    case VIR_DOMAIN_DEVICE_NET:
        return chSyncNetAddressesBetweenConfigs(dest, source, dev->data.net);

    case VIR_DOMAIN_DEVICE_SOUND:
    case VIR_DOMAIN_DEVICE_HOSTDEV:
    case VIR_DOMAIN_DEVICE_LEASE:
    case VIR_DOMAIN_DEVICE_CONTROLLER:
    case VIR_DOMAIN_DEVICE_CHR:
    case VIR_DOMAIN_DEVICE_FS:
    case VIR_DOMAIN_DEVICE_RNG:
    case VIR_DOMAIN_DEVICE_MEMORY:
    case VIR_DOMAIN_DEVICE_REDIRDEV:
    case VIR_DOMAIN_DEVICE_SHMEM:
    case VIR_DOMAIN_DEVICE_WATCHDOG:
    case VIR_DOMAIN_DEVICE_INPUT:
    case VIR_DOMAIN_DEVICE_VSOCK:
    case VIR_DOMAIN_DEVICE_IOMMU:
    case VIR_DOMAIN_DEVICE_VIDEO:
    case VIR_DOMAIN_DEVICE_GRAPHICS:
    case VIR_DOMAIN_DEVICE_HUB:
    case VIR_DOMAIN_DEVICE_SMARTCARD:
    case VIR_DOMAIN_DEVICE_MEMBALLOON:
    case VIR_DOMAIN_DEVICE_NVRAM:
    case VIR_DOMAIN_DEVICE_NONE:
    case VIR_DOMAIN_DEVICE_TPM:
    case VIR_DOMAIN_DEVICE_PANIC:
    case VIR_DOMAIN_DEVICE_AUDIO:
    case VIR_DOMAIN_DEVICE_CRYPTO:
    case VIR_DOMAIN_DEVICE_PSTORE:
    case VIR_DOMAIN_DEVICE_LAST:
        break;
    }

    return 0;
}

int
chDomainAttachDeviceLiveAndUpdateConfig(virDomainObj *vm,
                                        virCHDriver *driver,
                                        const char *xml,
                                        unsigned int flags)
{
    unsigned int parse_flags = VIR_DOMAIN_DEF_PARSE_INACTIVE |
                               VIR_DOMAIN_DEF_PARSE_ABI_UPDATE;
    virObjectEvent *event = NULL;
    g_autoptr(virDomainDeviceDef) devLive = NULL;
    g_autoptr(virDomainDef) vmdef = NULL;
    g_autoptr(virCHDriverConfig) cfg = NULL;
    g_autoptr(virDomainDeviceDef) devConf = NULL;
    virDomainDeviceDef devConfSave = { 0 };

    virCheckFlags(VIR_DOMAIN_AFFECT_LIVE |
                  VIR_DOMAIN_AFFECT_CONFIG, -1);

    cfg = virCHDriverGetConfig(driver);

    if (flags & VIR_DOMAIN_AFFECT_CONFIG) {
        vmdef = virDomainObjCopyPersistentDef(vm, driver->xmlopt, NULL);
        if (!vmdef)
            return -1;

        if (!(devConf = virDomainDeviceDefParse(xml, vmdef,
                                                driver->xmlopt, NULL,
                                                parse_flags))) {
            return -1;
        }

        devConfSave = *devConf;

        if (virDomainDeviceValidateAliasForHotplug(vm, devConf,
                                                   VIR_DOMAIN_AFFECT_CONFIG) < 0)
            return -1;

        if (virDomainDefCompatibleDevice(vmdef, devConf, NULL,
                                         VIR_DOMAIN_DEVICE_ACTION_ATTACH,
                                         false) < 0)
            return -1;

        if (chDomainAttachDeviceConfig(vmdef, devConf, parse_flags,
                                       driver->xmlopt) < 0)
            return -1;
    }

    if (flags & VIR_DOMAIN_AFFECT_LIVE) {
        if (!(devLive = virDomainDeviceDefParse(xml, vm->def,
                                                driver->xmlopt, NULL,
                                                parse_flags))) {
            return -1;
        }

        if (flags & VIR_DOMAIN_AFFECT_CONFIG)
            chDomainAttachDeviceLiveAndConfigHomogenize(&devConfSave, devLive);

        if (virDomainDeviceValidateAliasForHotplug(vm, devLive,
                                                   VIR_DOMAIN_AFFECT_LIVE) < 0)
            return -1;

        if (virDomainDefCompatibleDevice(vm->def, devLive, NULL,
                                        VIR_DOMAIN_DEVICE_ACTION_ATTACH,
                                        true) < 0) {
            return -1;
        }

        devConfSave = *devLive;

        if (chDomainAttachDeviceLive(driver, vm, devLive) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                        _("Failed to add device"));
            return -1;
        }

        if ((flags & VIR_DOMAIN_AFFECT_CONFIG) &&
            chSyncDeviceAddressLiveAndPersistent(vmdef, vm->def, &devConfSave) < 0)
            return -1;

        if (virDomainObjIsActive(vm) &&
            virDomainObjSave(vm, driver->xmlopt, cfg->stateDir) < 0) {
            VIR_DEBUG("Failed to save status on vm %s", vm->def->name);
        }
    }

    if (flags & VIR_DOMAIN_AFFECT_CONFIG) {
        if (virDomainDefSave(vmdef, driver->xmlopt, cfg->configDir) < 0)
            return -1;

        virDomainObjAssignDef(vm, &vmdef, false, NULL);

        event = virDomainEventLifecycleNewFromObj(vm,
                                                  VIR_DOMAIN_EVENT_DEFINED,
                                                  VIR_DOMAIN_EVENT_DEFINED_UPDATED);
        virObjectEventStateQueue(driver->domainEventState, event);
    }

    return 0;
}

static int
chFindDiskId(virDomainDef *def, const char *dst)
{
    size_t i;

    for (i = 0; i < def->ndisks; i++) {
        if (STREQ(def->disks[i]->dst, dst))
            return i;
    }

    return -1;
}


/**
 * chDomainFindDisk
 *
 * Helper function to find a disk device definition of a domain.
 *
 * Searches through the disk devices of a domain by comparing to 'match' and
 * returns any match via the 'detach' out parameter.
 */
static int
chDomainFindDisk(virDomainObj *vm,
                 virDomainDiskDef *match,
                 virDomainDiskDef **detach)
{
    int idx;

    if ((idx = chFindDiskId(vm->def, match->dst)) < 0) {
        virReportError(VIR_ERR_DEVICE_MISSING,
                       _("disk %1$s not found"), match->dst);
        return -1;
    }
    *detach = vm->def->disks[idx];

    return 0;
}


static int
chFindNetID(virDomainDef *def, const char *dst)
{
    size_t i;

    for (i = 0; i < def->nnets; i++) {
        if (STREQ(def->nets[i]->ifname, dst))
            return i;
    }

    return -1;
}

/**
 * chDomainFindNet
 *
 * Helper function to find a network device definition of a domain.
 *
 * Searches through the network devices of a domain by comparing to 'match' and
 * returns any match via the 'detach' out parameter.
 */
static int
chDomainFindNet(virDomainObj *vm,
                virDomainNetDef *match,
                virDomainNetDef **detach)
{
    int idx;

    if (!match->ifname) {
        virReportError(VIR_ERR_DEVICE_MISSING, "%s",
                       _("no interface name specified"));
        return -1;
    }
    if ((idx = chFindNetID(vm->def, match->ifname)) < 0) {
        virReportError(VIR_ERR_DEVICE_MISSING,
                       _("net %1$s not found"), match->ifname);
        return -1;
    }
    *detach = vm->def->nets[idx];

    return 0;
}


static int
chDomainRemoveDevice(virDomainObj *vm,
                     virDomainDeviceDef *device)
{
    size_t i;

    VIR_DEBUG("Removing device %s from domain %p %s",
              virDomainDeviceTypeToString(device->type), vm, vm->def->name);

    switch (device->type) {
    case VIR_DOMAIN_DEVICE_DISK:
        for (i = 0; i < vm->def->ndisks; i++) {
            if (vm->def->disks[i] == device->data.disk) {
                virDomainDiskRemove(vm->def, i);
                g_clear_pointer(&device->data.disk, virDomainDiskDefFree);
                break;
            }
        }
        break;
    case VIR_DOMAIN_DEVICE_NET:
        virDomainInterfaceStopDevice(device->data.net);
        virDomainInterfaceDeleteDevice(vm->def, device->data.net, false, NULL);
        for (i = 0; i < vm->def->nnets; i++) {
            if (vm->def->nets[i] == device->data.net) {
                virDomainNetRemove(vm->def, i);
                g_clear_pointer(&device->data.net, virDomainNetDefFree);
                break;
            }
        }
        break;
    case VIR_DOMAIN_DEVICE_LEASE:
    case VIR_DOMAIN_DEVICE_FS:
    case VIR_DOMAIN_DEVICE_INPUT:
    case VIR_DOMAIN_DEVICE_SOUND:
    case VIR_DOMAIN_DEVICE_VIDEO:
    case VIR_DOMAIN_DEVICE_HOSTDEV:
    case VIR_DOMAIN_DEVICE_WATCHDOG:
    case VIR_DOMAIN_DEVICE_CONTROLLER:
    case VIR_DOMAIN_DEVICE_GRAPHICS:
    case VIR_DOMAIN_DEVICE_HUB:
    case VIR_DOMAIN_DEVICE_REDIRDEV:
    case VIR_DOMAIN_DEVICE_SMARTCARD:
    case VIR_DOMAIN_DEVICE_CHR:
    case VIR_DOMAIN_DEVICE_MEMBALLOON:
    case VIR_DOMAIN_DEVICE_NVRAM:
    case VIR_DOMAIN_DEVICE_RNG:
    case VIR_DOMAIN_DEVICE_SHMEM:
    case VIR_DOMAIN_DEVICE_TPM:
    case VIR_DOMAIN_DEVICE_PANIC:
    case VIR_DOMAIN_DEVICE_MEMORY:
    case VIR_DOMAIN_DEVICE_IOMMU:
    case VIR_DOMAIN_DEVICE_VSOCK:
    case VIR_DOMAIN_DEVICE_AUDIO:
    case VIR_DOMAIN_DEVICE_CRYPTO:
    case VIR_DOMAIN_DEVICE_PSTORE:
    case VIR_DOMAIN_DEVICE_LAST:
    case VIR_DOMAIN_DEVICE_NONE:
    default:
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED,
                       _("don't know how to remove a %1$s device"),
                       virDomainDeviceTypeToString(device->type));
        return -1;
    }

    return 0;
}


static int
chDomainDetachDeviceLive(virCHDriver *driver,
                         virDomainObj *vm,
                         virDomainDeviceDef *match,
                         bool free_addr)
{
    virDomainDeviceDef detach = { .type = match->type };
    virDomainDeviceInfo addrinfo = { 0 };
    virDomainDeviceInfo *info = NULL;
    virCHDomainObjPrivate *priv = vm->privateData;
    virObjectEvent *event = NULL;
    g_autofree char *alias = NULL;

    switch (match->type) {
    case VIR_DOMAIN_DEVICE_DISK:
        if (chDomainFindDisk(vm, match->data.disk,
                             &detach.data.disk) < 0) {
            return -1;
        }
        break;
    case VIR_DOMAIN_DEVICE_NET:
        if (chDomainFindNet(vm, match->data.net,
                            &detach.data.net) < 0) {
            return -1;
        }
        break;
    case VIR_DOMAIN_DEVICE_LEASE:
    case VIR_DOMAIN_DEVICE_FS:
    case VIR_DOMAIN_DEVICE_INPUT:
    case VIR_DOMAIN_DEVICE_SOUND:
    case VIR_DOMAIN_DEVICE_VIDEO:
    case VIR_DOMAIN_DEVICE_HOSTDEV:
    case VIR_DOMAIN_DEVICE_WATCHDOG:
    case VIR_DOMAIN_DEVICE_CONTROLLER:
    case VIR_DOMAIN_DEVICE_GRAPHICS:
    case VIR_DOMAIN_DEVICE_HUB:
    case VIR_DOMAIN_DEVICE_REDIRDEV:
    case VIR_DOMAIN_DEVICE_SMARTCARD:
    case VIR_DOMAIN_DEVICE_CHR:
    case VIR_DOMAIN_DEVICE_MEMBALLOON:
    case VIR_DOMAIN_DEVICE_NVRAM:
    case VIR_DOMAIN_DEVICE_RNG:
    case VIR_DOMAIN_DEVICE_SHMEM:
    case VIR_DOMAIN_DEVICE_TPM:
    case VIR_DOMAIN_DEVICE_PANIC:
    case VIR_DOMAIN_DEVICE_MEMORY:
    case VIR_DOMAIN_DEVICE_IOMMU:
    case VIR_DOMAIN_DEVICE_VSOCK:
    case VIR_DOMAIN_DEVICE_AUDIO:
    case VIR_DOMAIN_DEVICE_CRYPTO:
    case VIR_DOMAIN_DEVICE_PSTORE:
    case VIR_DOMAIN_DEVICE_LAST:
    case VIR_DOMAIN_DEVICE_NONE:
    default:
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED,
                       _("live detach of device '%1$s' is not supported"),
                       virDomainDeviceTypeToString(match->type));
        return -1;
    }

    /* "detach" now points to the actual device we want to detach */

    if (!(info = virDomainDeviceGetInfo(&detach))) {
        /*
         * This should never happen, since all of the device types in
         * the switch cases that end with a "break" instead of a
         * return have a virDeviceInfo in them.
         */
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("device of type '%1$s' has no device info"),
                       virDomainDeviceTypeToString(detach.type));
        return -1;
    }

    /* Make generic validation checks common to all device types */

    if (!info->alias) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Cannot detach %1$s device with no alias"),
                       virDomainDeviceTypeToString(detach.type));
        return -1;
    }

    /* Save the alias to use when sending a DEVICE_REMOVED event after all
     * other tear down is complete.
     */
    alias = g_strdup(info->alias);
    addrinfo = *info;

    if (virCHMonitorRemoveDevice(priv->monitor, info->alias) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Invalid response from CH. Device removal failed for device %1$s."),
                       info->alias);
        return -1;
    }

    if (chDomainRemoveDevice(vm, &detach) < 0)
        return -1;

    if (free_addr)
        chDomainReleaseDeviceAddress(vm, &addrinfo);

    event = virDomainEventDeviceRemovedNewFromObj(vm, alias);
    virObjectEventStateQueue(driver->domainEventState, event);

    return 0;
}

static int
chDomainDetachDeviceConfig(virDomainDef *vmdef,
                           virDomainDeviceDef *dev,
                           unsigned int parse_flags,
                           virDomainXMLOption *xmlopt)
{
    virDomainDiskDef *disk;
    virDomainDiskDef *detDisk;
    virDomainNetDef *net;
    int idx;

    switch (dev->type) {
    case VIR_DOMAIN_DEVICE_DISK:
        disk = dev->data.disk;
        if (!(detDisk = virDomainDiskRemoveByName(vmdef, disk->dst))) {
            virReportError(VIR_ERR_DEVICE_MISSING,
                           _("no target device %1$s"), disk->dst);
            return -1;
        }
        virDomainDiskDefFree(detDisk);
        break;

    case VIR_DOMAIN_DEVICE_NET:
        net = dev->data.net;
        if ((idx = virDomainNetFindIdx(vmdef, net)) < 0)
            return -1;

        virDomainNetDefFree(virDomainNetRemove(vmdef, idx));
        break;

    case VIR_DOMAIN_DEVICE_SOUND:
    case VIR_DOMAIN_DEVICE_HOSTDEV:
    case VIR_DOMAIN_DEVICE_LEASE:
    case VIR_DOMAIN_DEVICE_CONTROLLER:
    case VIR_DOMAIN_DEVICE_CHR:
    case VIR_DOMAIN_DEVICE_FS:
    case VIR_DOMAIN_DEVICE_RNG:
    case VIR_DOMAIN_DEVICE_MEMORY:
    case VIR_DOMAIN_DEVICE_REDIRDEV:
    case VIR_DOMAIN_DEVICE_SHMEM:
    case VIR_DOMAIN_DEVICE_WATCHDOG:
    case VIR_DOMAIN_DEVICE_INPUT:
    case VIR_DOMAIN_DEVICE_VSOCK:
    case VIR_DOMAIN_DEVICE_IOMMU:
    case VIR_DOMAIN_DEVICE_VIDEO:
    case VIR_DOMAIN_DEVICE_GRAPHICS:
    case VIR_DOMAIN_DEVICE_HUB:
    case VIR_DOMAIN_DEVICE_SMARTCARD:
    case VIR_DOMAIN_DEVICE_MEMBALLOON:
    case VIR_DOMAIN_DEVICE_NVRAM:
    case VIR_DOMAIN_DEVICE_NONE:
    case VIR_DOMAIN_DEVICE_TPM:
    case VIR_DOMAIN_DEVICE_PANIC:
    case VIR_DOMAIN_DEVICE_AUDIO:
    case VIR_DOMAIN_DEVICE_CRYPTO:
    case VIR_DOMAIN_DEVICE_PSTORE:
    case VIR_DOMAIN_DEVICE_LAST:
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED,
                       _("persistent detach of device '%1$s' is not supported"),
                       virDomainDeviceTypeToString(dev->type));
        return -1;
    }

    if (virDomainDefPostParse(vmdef, parse_flags, xmlopt, NULL) < 0)
        return -1;

    if (virDomainDefValidate(vmdef, parse_flags, xmlopt, NULL) < 0)
        return -1;

    return 0;
}

static bool
chDomainContainsDevice(virDomainDef *domDef,
                       virDomainDeviceDef *devDef)
{
    switch (devDef->type) {
    case VIR_DOMAIN_DEVICE_DISK:
        return virDomainDiskByName(domDef, devDef->data.disk->dst, false) != NULL;

    case VIR_DOMAIN_DEVICE_NET:
        return virDomainNetFindByName(domDef, devDef->data.net->ifname) != NULL;

    case VIR_DOMAIN_DEVICE_SOUND:
    case VIR_DOMAIN_DEVICE_HOSTDEV:
    case VIR_DOMAIN_DEVICE_LEASE:
    case VIR_DOMAIN_DEVICE_CONTROLLER:
    case VIR_DOMAIN_DEVICE_CHR:
    case VIR_DOMAIN_DEVICE_FS:
    case VIR_DOMAIN_DEVICE_RNG:
    case VIR_DOMAIN_DEVICE_MEMORY:
    case VIR_DOMAIN_DEVICE_REDIRDEV:
    case VIR_DOMAIN_DEVICE_SHMEM:
    case VIR_DOMAIN_DEVICE_WATCHDOG:
    case VIR_DOMAIN_DEVICE_INPUT:
    case VIR_DOMAIN_DEVICE_VSOCK:
    case VIR_DOMAIN_DEVICE_IOMMU:
    case VIR_DOMAIN_DEVICE_VIDEO:
    case VIR_DOMAIN_DEVICE_GRAPHICS:
    case VIR_DOMAIN_DEVICE_HUB:
    case VIR_DOMAIN_DEVICE_SMARTCARD:
    case VIR_DOMAIN_DEVICE_MEMBALLOON:
    case VIR_DOMAIN_DEVICE_NVRAM:
    case VIR_DOMAIN_DEVICE_NONE:
    case VIR_DOMAIN_DEVICE_TPM:
    case VIR_DOMAIN_DEVICE_PANIC:
    case VIR_DOMAIN_DEVICE_AUDIO:
    case VIR_DOMAIN_DEVICE_CRYPTO:
    case VIR_DOMAIN_DEVICE_PSTORE:
    case VIR_DOMAIN_DEVICE_LAST:
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED,
                       _("Currently no support for check existence for devices of type `%1$s`"),
                       virDomainDeviceTypeToString(devDef->type));
        break;
    }

    return false;
}

int
chDomainDetachDeviceLiveAndUpdateConfig(virCHDriver *driver,
                                        virDomainObj *vm,
                                        const char *xml,
                                        unsigned int flags)
{
    virObjectEvent *event = NULL;
    g_autoptr(virCHDriverConfig) cfg = NULL;
    g_autoptr(virDomainDeviceDef) devConfig = NULL;
    g_autoptr(virDomainDeviceDef) devLive = NULL;
    unsigned int parse_flags = VIR_DOMAIN_DEF_PARSE_SKIP_VALIDATE;
    g_autoptr(virDomainDef) vmdef = NULL;

    virCheckFlags(VIR_DOMAIN_AFFECT_LIVE |
                  VIR_DOMAIN_AFFECT_CONFIG, -1);

    cfg = virCHDriverGetConfig(driver);

    if ((flags & VIR_DOMAIN_AFFECT_CONFIG) &&
        !(flags & VIR_DOMAIN_AFFECT_LIVE))
        parse_flags |= VIR_DOMAIN_DEF_PARSE_INACTIVE;

    vmdef = virDomainObjCopyPersistentDef(vm, driver->xmlopt, NULL);

    if (vmdef)
        devConfig = virDomainDeviceDefParse(xml, vmdef, driver->xmlopt,
                                            NULL, parse_flags);
    devLive = virDomainDeviceDefParse(xml, vm->def, driver->xmlopt,
                                      NULL, parse_flags);

    if (flags & VIR_DOMAIN_AFFECT_CONFIG) {
        if (!vmdef || !devConfig)
            return -1;

        if (chDomainDetachDeviceConfig(vmdef, devConfig, parse_flags,
                                       driver->xmlopt) < 0)
            return -1;
    }

    if (flags & VIR_DOMAIN_AFFECT_LIVE) {
        bool free_addr = false;

        if (!devLive)
            return -1;

        if (!vmdef ||
            !chDomainContainsDevice(vmdef, devLive) ||
            (flags & VIR_DOMAIN_AFFECT_CONFIG))
            free_addr = true;

        if (chDomainDetachDeviceLive(driver, vm, devLive, free_addr) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                        _("Could detach device"));
            return -1;
        }

        if (virDomainObjIsActive(vm) &&
            virDomainObjSave(vm, driver->xmlopt, cfg->stateDir) < 0) {
            VIR_DEBUG("Failed to save status on vm %s", vm->def->name);
        }
    }

    if (flags & VIR_DOMAIN_AFFECT_CONFIG) {
        if (virDomainDefSave(vmdef, driver->xmlopt, cfg->configDir) < 0)
            return -1;

        virDomainObjAssignDef(vm, &vmdef, false, NULL);

        event = virDomainEventLifecycleNewFromObj(vm,
                                                  VIR_DOMAIN_EVENT_DEFINED,
                                                  VIR_DOMAIN_EVENT_DEFINED_UPDATED);
        virObjectEventStateQueue(driver->domainEventState, event);
    }

    return 0;
}
