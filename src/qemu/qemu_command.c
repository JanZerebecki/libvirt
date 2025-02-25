/*
 * qemu_command.c: QEMU command generation
 *
 * Copyright (C) 2006-2016 Red Hat, Inc.
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
 * License along with this library.  If not, see
 * <http://www.gnu.org/licenses/>.
 */

#include <config.h>

#include "qemu_command.h"
#include "qemu_hostdev.h"
#include "qemu_capabilities.h"
#include "qemu_interface.h"
#include "qemu_alias.h"
#include "qemu_security.h"
#include "qemu_block.h"
#include "cpu/cpu.h"
#include "dirname.h"
#include "viralloc.h"
#include "virlog.h"
#include "virarch.h"
#include "virerror.h"
#include "virfile.h"
#include "virnetdev.h"
#include "virnetdevbridge.h"
#include "virqemu.h"
#include "virstring.h"
#include "virtime.h"
#include "viruuid.h"
#include "domain_nwfilter.h"
#include "domain_addr.h"
#include "domain_audit.h"
#include "domain_conf.h"
#include "netdev_bandwidth_conf.h"
#include "snapshot_conf.h"
#include "storage_conf.h"
#include "secret_conf.h"
#include "virnetdevtap.h"
#include "virnetdevopenvswitch.h"
#include "device_conf.h"
#include "virstoragefile.h"
#include "virtpm.h"
#include "virscsi.h"
#include "virnuma.h"
#include "virgic.h"
#include "virmdev.h"
#include "virdomainsnapshotobjlist.h"
#if defined(__linux__)
# include <linux/capability.h>
#endif
#include "logging/log_manager.h"

#include <sys/stat.h>
#include <fcntl.h>

#define VIR_FROM_THIS VIR_FROM_QEMU

VIR_LOG_INIT("qemu.qemu_command");

VIR_ENUM_DECL(virDomainDiskQEMUBus);
VIR_ENUM_IMPL(virDomainDiskQEMUBus,
              VIR_DOMAIN_DISK_BUS_LAST,
              "ide",
              "floppy",
              "scsi",
              "virtio",
              "xen",
              "usb",
              "uml",
              "sata",
              "sd",
);


VIR_ENUM_DECL(qemuDiskCacheV2);

VIR_ENUM_IMPL(qemuDiskCacheV2,
              VIR_DOMAIN_DISK_CACHE_LAST,
              "default",
              "none",
              "writethrough",
              "writeback",
              "directsync",
              "unsafe",
);

VIR_ENUM_IMPL(qemuVideo,
              VIR_DOMAIN_VIDEO_TYPE_LAST,
              "", /* default value, we shouldn't see this */
              "std",
              "cirrus",
              "vmware",
              "", /* don't support xen */
              "", /* don't support vbox */
              "qxl",
              "", /* don't support parallels */
              "", /* no need for virtio */
              "" /* don't support gop */,
              "" /* 'none' doesn't make sense here */,
);

VIR_ENUM_DECL(qemuDeviceVideo);

VIR_ENUM_IMPL(qemuDeviceVideo,
              VIR_DOMAIN_VIDEO_TYPE_LAST,
              "", /* default value, we shouldn't see this */
              "VGA",
              "cirrus-vga",
              "vmware-svga",
              "", /* don't support xen */
              "", /* don't support vbox */
              "qxl-vga",
              "", /* don't support parallels */
              "virtio-vga",
              "" /* don't support gop */,
              "" /* 'none' doesn't make sense here */,
);

VIR_ENUM_DECL(qemuDeviceVideoSecondary);

VIR_ENUM_IMPL(qemuDeviceVideoSecondary,
              VIR_DOMAIN_VIDEO_TYPE_LAST,
              "", /* default value, we shouldn't see this */
              "", /* no secondary device for VGA */
              "", /* no secondary device for cirrus-vga */
              "", /* no secondary device for vmware-svga */
              "", /* don't support xen */
              "", /* don't support vbox */
              "qxl",
              "", /* don't support parallels */
              "virtio-gpu",
              "" /* don't support gop */,
              "" /* 'none' doesn't make sense here */,
);

VIR_ENUM_DECL(qemuSoundCodec);

VIR_ENUM_IMPL(qemuSoundCodec,
              VIR_DOMAIN_SOUND_CODEC_TYPE_LAST,
              "hda-duplex",
              "hda-micro",
              "hda-output",
);

VIR_ENUM_DECL(qemuControllerModelUSB);

VIR_ENUM_IMPL(qemuControllerModelUSB,
              VIR_DOMAIN_CONTROLLER_MODEL_USB_LAST,
              "piix3-usb-uhci",
              "piix4-usb-uhci",
              "usb-ehci",
              "ich9-usb-ehci1",
              "ich9-usb-uhci1",
              "ich9-usb-uhci2",
              "ich9-usb-uhci3",
              "vt82c686b-usb-uhci",
              "pci-ohci",
              "nec-usb-xhci",
              "qusb1",
              "qusb2",
              "qemu-xhci",
              "none",
);

VIR_ENUM_DECL(qemuDomainFSDriver);
VIR_ENUM_IMPL(qemuDomainFSDriver,
              VIR_DOMAIN_FS_DRIVER_TYPE_LAST,
              "local",
              "local",
              "handle",
              NULL,
              NULL,
              NULL,
);

VIR_ENUM_DECL(qemuNumaPolicy);
VIR_ENUM_IMPL(qemuNumaPolicy,
              VIR_DOMAIN_NUMATUNE_MEM_LAST,
              "bind",
              "preferred",
              "interleave",
);


/**
 * qemuBuildMasterKeyCommandLine:
 * @cmd: the command to modify
 * @qemuCaps qemu capabilities object
 * @domainLibDir: location to find the master key

 * Formats the command line for a master key if available
 *
 * Returns 0 on success, -1 w/ error message on failure
 */
static int
qemuBuildMasterKeyCommandLine(virCommandPtr cmd,
                              qemuDomainObjPrivatePtr priv)
{
    int ret = -1;
    char *alias = NULL;
    char *path = NULL;
    virBuffer buf = VIR_BUFFER_INITIALIZER;

    /* If the -object secret does not exist, then just return. This just
     * means the domain won't be able to use a secret master key and is
     * not a failure.
     */
    if (!virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_OBJECT_SECRET)) {
        VIR_INFO("secret object is not supported by this QEMU binary");
        return 0;
    }

    if (!(alias = qemuDomainGetMasterKeyAlias()))
        return -1;

    /* Get the path. NB, the mocked test will not have the created
     * file so we cannot check for existence, which is no different
     * than other command line options which do not check for the
     * existence of socket files before using.
     */
    if (!(path = qemuDomainGetMasterKeyFilePath(priv->libDir)))
        goto cleanup;

    virCommandAddArg(cmd, "-object");
    virBufferAsprintf(&buf, "secret,id=%s,format=raw,file=", alias);
    virQEMUBuildBufferEscapeComma(&buf, path);
    virCommandAddArgBuffer(cmd, &buf);

    ret = 0;

 cleanup:
    virBufferFreeAndReset(&buf);
    VIR_FREE(alias);
    VIR_FREE(path);
    return ret;
}


/**
 * qemuVirCommandGetFDSet:
 * @cmd: the command to modify
 * @fd: fd to reassign to the child
 *
 * Get the parameters for the QEMU -add-fd command line option
 * for the given file descriptor. The file descriptor must previously
 * have been 'transferred' in a virCommandPassFD() call.
 * This function for example returns "set=10,fd=20".
 */
static char *
qemuVirCommandGetFDSet(virCommandPtr cmd, int fd)
{
    char *result = NULL;
    int idx = virCommandPassFDGetFDIndex(cmd, fd);

    if (idx >= 0) {
        ignore_value(virAsprintf(&result, "set=%d,fd=%d", idx, fd));
    } else {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("file descriptor %d has not been transferred"), fd);
    }

    return result;
}


/**
 * qemuVirCommandGetDevSet:
 * @cmd: the command to modify
 * @fd: fd to reassign to the child
 *
 * Get the parameters for the QEMU path= parameter where a file
 * descriptor is accessed via a file descriptor set, for example
 * /dev/fdset/10. The file descriptor must previously have been
 * 'transferred' in a virCommandPassFD() call.
 */
static char *
qemuVirCommandGetDevSet(virCommandPtr cmd, int fd)
{
    char *result = NULL;
    int idx = virCommandPassFDGetFDIndex(cmd, fd);

    if (idx >= 0) {
        ignore_value(virAsprintf(&result, "/dev/fdset/%d", idx));
    } else {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("file descriptor %d has not been transferred"), fd);
    }
    return result;
}


static int
qemuBuildDeviceAddressStr(virBufferPtr buf,
                          const virDomainDef *domainDef,
                          virDomainDeviceInfoPtr info,
                          virQEMUCapsPtr qemuCaps)
{
    int ret = -1;
    char *devStr = NULL;
    const char *contAlias = NULL;
    bool contIsPHB = false;
    int contTargetIndex = 0;

    if (info->type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI) {
        size_t i;

        if (!(devStr = virPCIDeviceAddressAsString(&info->addr.pci)))
            goto cleanup;
        for (i = 0; i < domainDef->ncontrollers; i++) {
            virDomainControllerDefPtr cont = domainDef->controllers[i];

            if (cont->type == VIR_DOMAIN_CONTROLLER_TYPE_PCI &&
                cont->idx == info->addr.pci.bus) {
                contAlias = cont->info.alias;
                contIsPHB = virDomainControllerIsPSeriesPHB(cont);
                contTargetIndex = cont->opts.pciopts.targetIndex;

                if (!contAlias) {
                    virReportError(VIR_ERR_INTERNAL_ERROR,
                                   _("Device alias was not set for PCI "
                                     "controller with index %u required "
                                     "for device at address %s"),
                                   info->addr.pci.bus, devStr);
                    goto cleanup;
                }

                if (virDomainDeviceAliasIsUserAlias(contAlias)) {
                    /* When domain has builtin pci-root controller we don't put it
                     * onto cmd line. Therefore we can't set its alias. In that
                     * case, use the default one. */
                    if (!qemuDomainIsPSeries(domainDef) &&
                        cont->model == VIR_DOMAIN_CONTROLLER_MODEL_PCI_ROOT) {
                        if (virQEMUCapsHasPCIMultiBus(qemuCaps, domainDef))
                            contAlias = "pci.0";
                        else
                            contAlias = "pci";
                    } else if (cont->model == VIR_DOMAIN_CONTROLLER_MODEL_PCIE_ROOT) {
                        contAlias = "pcie.0";
                    }
                }
                break;
            }
        }
        if (!contAlias) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Could not find PCI "
                             "controller with index %u required "
                             "for device at address %s"),
                           info->addr.pci.bus, devStr);
            goto cleanup;
        }

        if (contIsPHB && contTargetIndex > 0) {
            /* The PCI bus created by a spapr-pci-host-bridge device with
             * alias 'x' will be called 'x.0' rather than 'x'; however,
             * this does not apply to the implicit PHB in a pSeries guest,
             * which always has the hardcoded name 'pci.0' */
            virBufferAsprintf(buf, ",bus=%s.0", contAlias);
        } else {
            /* For all other controllers, the bus name matches the alias
             * of the corresponding controller */
            virBufferAsprintf(buf, ",bus=%s", contAlias);
        }

        if (info->addr.pci.multi == VIR_TRISTATE_SWITCH_ON)
            virBufferAddLit(buf, ",multifunction=on");
        else if (info->addr.pci.multi == VIR_TRISTATE_SWITCH_OFF)
            virBufferAddLit(buf, ",multifunction=off");
        virBufferAsprintf(buf, ",addr=0x%x", info->addr.pci.slot);
        if (info->addr.pci.function != 0)
           virBufferAsprintf(buf, ".0x%x", info->addr.pci.function);
    } else if (info->type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_USB) {
        if (!(contAlias = virDomainControllerAliasFind(domainDef,
                                                       VIR_DOMAIN_CONTROLLER_TYPE_USB,
                                                       info->addr.usb.bus)))
            goto cleanup;
        virBufferAsprintf(buf, ",bus=%s.0", contAlias);
        if (virDomainUSBAddressPortIsValid(info->addr.usb.port)) {
            virBufferAddLit(buf, ",port=");
            virDomainUSBAddressPortFormatBuf(buf, info->addr.usb.port);
        }
    } else if (info->type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_SPAPRVIO) {
        if (info->addr.spaprvio.has_reg)
            virBufferAsprintf(buf, ",reg=0x%08llx", info->addr.spaprvio.reg);
    } else if (info->type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_CCW) {
        if (info->addr.ccw.assigned)
            virBufferAsprintf(buf, ",devno=%x.%x.%04x",
                              info->addr.ccw.cssid,
                              info->addr.ccw.ssid,
                              info->addr.ccw.devno);
    } else if (info->type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_ISA) {
        virBufferAsprintf(buf, ",iobase=0x%x,irq=0x%x",
                          info->addr.isa.iobase,
                          info->addr.isa.irq);
    }

    ret = 0;
 cleanup:
    VIR_FREE(devStr);
    return ret;
}


/**
 * qemuBuildVirtioDevStr
 * @buf: virBufferPtr to append the built string
 * @baseName: qemu virtio device basename string. Ex: virtio-rng for <rng>
 * @qemuCaps: virQEMUCapPtr
 * @devtype: virDomainDeviceType of the device. Ex: VIR_DOMAIN_DEVICE_TYPE_RNG
 * @devdata: *DefPtr of the device definition
 *
 * Build the qemu virtio -device name from the passed parameters. Currently
 * this is mostly about attaching the correct string prefix to @baseName for
 * the passed @type. So for @baseName "virtio-rng" and devdata->info.type
 * VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI, generate "virtio-rng-pci"
 *
 * Returns: -1 on failure, 0 on success
 */
static int
qemuBuildVirtioDevStr(virBufferPtr buf,
                      const char *baseName,
                      virQEMUCapsPtr qemuCaps,
                      virDomainDeviceType devtype,
                      void *devdata)
{
    const char *implName = NULL;
    virDomainDeviceDef device = { .type = devtype };
    virDomainDeviceInfoPtr info;
    bool has_tmodel, has_ntmodel;

    virDomainDeviceSetData(&device, devdata);
    info = virDomainDeviceGetInfo(&device);

    switch ((virDomainDeviceAddressType) info->type) {
    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI:
        implName = "pci";
        break;

    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_VIRTIO_MMIO:
        implName = "device";
        break;

    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_CCW:
        implName = "ccw";
        break;

    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_VIRTIO_S390:
        implName = "s390";
        break;

    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_DRIVE:
    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_VIRTIO_SERIAL:
    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_CCID:
    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_USB:
    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_SPAPRVIO:
    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_ISA:
    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_DIMM:
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Unexpected address type for '%s'"), baseName);
        return -1;

    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_NONE:
    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_LAST:
    default:
        virReportEnumRangeError(virDomainDeviceAddressType, info->type);
        return -1;
    }

    virBufferAsprintf(buf, "%s-%s", baseName, implName);

    switch (devtype) {
        case VIR_DOMAIN_DEVICE_DISK:
            has_tmodel = device.data.disk->model == VIR_DOMAIN_DISK_MODEL_VIRTIO_TRANSITIONAL;
            has_ntmodel = device.data.disk->model == VIR_DOMAIN_DISK_MODEL_VIRTIO_NON_TRANSITIONAL;
            break;

        case VIR_DOMAIN_DEVICE_NET:
            has_tmodel = device.data.net->model == VIR_DOMAIN_NET_MODEL_VIRTIO_TRANSITIONAL;
            has_ntmodel = device.data.net->model == VIR_DOMAIN_NET_MODEL_VIRTIO_NON_TRANSITIONAL;
            break;

        case VIR_DOMAIN_DEVICE_HOSTDEV:
            if (device.data.hostdev->source.subsys.type != VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_SCSI_HOST)
                return 0;
            has_tmodel = device.data.hostdev->source.subsys.u.scsi_host.model == VIR_DOMAIN_HOSTDEV_SUBSYS_SCSI_VHOST_MODEL_TYPE_VIRTIO_TRANSITIONAL;
            has_ntmodel = device.data.hostdev->source.subsys.u.scsi_host.model == VIR_DOMAIN_HOSTDEV_SUBSYS_SCSI_VHOST_MODEL_TYPE_VIRTIO_NON_TRANSITIONAL;
            break;

        case VIR_DOMAIN_DEVICE_RNG:
            has_tmodel = device.data.rng->model == VIR_DOMAIN_RNG_MODEL_VIRTIO_TRANSITIONAL;
            has_ntmodel = device.data.rng->model == VIR_DOMAIN_RNG_MODEL_VIRTIO_NON_TRANSITIONAL;
            break;

        case VIR_DOMAIN_DEVICE_FS:
            has_tmodel = device.data.fs->model == VIR_DOMAIN_FS_MODEL_VIRTIO_TRANSITIONAL;
            has_ntmodel = device.data.fs->model == VIR_DOMAIN_FS_MODEL_VIRTIO_NON_TRANSITIONAL;
            break;

        case VIR_DOMAIN_DEVICE_MEMBALLOON:
            has_tmodel = device.data.memballoon->model == VIR_DOMAIN_MEMBALLOON_MODEL_VIRTIO_TRANSITIONAL;
            has_ntmodel = device.data.memballoon->model == VIR_DOMAIN_MEMBALLOON_MODEL_VIRTIO_NON_TRANSITIONAL;
            break;

        case VIR_DOMAIN_DEVICE_VSOCK:
            has_tmodel = device.data.vsock->model == VIR_DOMAIN_VSOCK_MODEL_VIRTIO_TRANSITIONAL;
            has_ntmodel = device.data.vsock->model == VIR_DOMAIN_VSOCK_MODEL_VIRTIO_NON_TRANSITIONAL;
            break;

        case VIR_DOMAIN_DEVICE_INPUT:
            if (device.data.input->type != VIR_DOMAIN_INPUT_TYPE_PASSTHROUGH)
                return 0;
            has_tmodel = device.data.input->model == VIR_DOMAIN_INPUT_MODEL_VIRTIO_TRANSITIONAL;
            has_ntmodel = device.data.input->model == VIR_DOMAIN_INPUT_MODEL_VIRTIO_NON_TRANSITIONAL;
            break;

        case VIR_DOMAIN_DEVICE_CONTROLLER:
            if (device.data.controller->type == VIR_DOMAIN_CONTROLLER_TYPE_VIRTIO_SERIAL) {
                has_tmodel = device.data.controller->model == VIR_DOMAIN_CONTROLLER_MODEL_VIRTIO_SERIAL_VIRTIO_TRANSITIONAL;
                has_ntmodel = device.data.controller->model == VIR_DOMAIN_CONTROLLER_MODEL_VIRTIO_SERIAL_VIRTIO_NON_TRANSITIONAL;
            } else if (device.data.controller->type == VIR_DOMAIN_CONTROLLER_TYPE_SCSI) {
                has_tmodel = device.data.controller->model == VIR_DOMAIN_CONTROLLER_MODEL_SCSI_VIRTIO_TRANSITIONAL;
                has_ntmodel = device.data.controller->model == VIR_DOMAIN_CONTROLLER_MODEL_SCSI_VIRTIO_NON_TRANSITIONAL;
            } else {
                return 0;
            }
            break;

        case VIR_DOMAIN_DEVICE_LEASE:
        case VIR_DOMAIN_DEVICE_SOUND:
        case VIR_DOMAIN_DEVICE_VIDEO:
        case VIR_DOMAIN_DEVICE_WATCHDOG:
        case VIR_DOMAIN_DEVICE_GRAPHICS:
        case VIR_DOMAIN_DEVICE_HUB:
        case VIR_DOMAIN_DEVICE_REDIRDEV:
        case VIR_DOMAIN_DEVICE_NONE:
        case VIR_DOMAIN_DEVICE_SMARTCARD:
        case VIR_DOMAIN_DEVICE_CHR:
        case VIR_DOMAIN_DEVICE_NVRAM:
        case VIR_DOMAIN_DEVICE_SHMEM:
        case VIR_DOMAIN_DEVICE_TPM:
        case VIR_DOMAIN_DEVICE_PANIC:
        case VIR_DOMAIN_DEVICE_MEMORY:
        case VIR_DOMAIN_DEVICE_IOMMU:
        case VIR_DOMAIN_DEVICE_LAST:
        default:
            return 0;
    }

    if (info->type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI &&
        (has_tmodel || has_ntmodel)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("virtio (non-)transitional models are not "
                         "supported for address type=%s"),
                       virDomainDeviceAddressTypeToString(info->type));
        return -1;
    }

    if (has_tmodel) {
        if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_VIRTIO_PCI_TRANSITIONAL)) {
            virBufferAddLit(buf, "-transitional");
        } else if (virQEMUCapsGet(qemuCaps,
                                  QEMU_CAPS_VIRTIO_PCI_DISABLE_LEGACY)) {
            virBufferAddLit(buf, ",disable-legacy=off,disable-modern=off");
        }
        /* No error if -transitional is not supported: our address
         * allocation will force the device into plain PCI bus, which
         * is functionally identical to standard 'virtio-XXX' behavior
         */
    } else if (has_ntmodel) {
        if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_VIRTIO_PCI_TRANSITIONAL)) {
            virBufferAddLit(buf, "-non-transitional");
        } else if (virQEMUCapsGet(qemuCaps,
                                  QEMU_CAPS_VIRTIO_PCI_DISABLE_LEGACY)) {
            /* Even if the QEMU binary doesn't support the non-transitional
             * device, we can still make it work by manually disabling legacy
             * VirtIO and enabling modern VirtIO */
            virBufferAddLit(buf, ",disable-legacy=on,disable-modern=off");
        } else {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("virtio non-transitional model not supported "
                             "for this qemu"));
            return -1;
        }
    }

    return 0;
}

static int
qemuBuildVirtioOptionsStr(virBufferPtr buf,
                          virDomainVirtioOptionsPtr virtio,
                          virQEMUCapsPtr qemuCaps)
{
    if (!virtio)
        return 0;

    if (virtio->iommu != VIR_TRISTATE_SWITCH_ABSENT) {
        if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_VIRTIO_PCI_IOMMU_PLATFORM)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("the iommu setting is not supported "
                             "with this QEMU binary"));
            return -1;
        }
        virBufferAsprintf(buf, ",iommu_platform=%s",
                          virTristateSwitchTypeToString(virtio->iommu));
    }
    if (virtio->ats != VIR_TRISTATE_SWITCH_ABSENT) {
        if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_VIRTIO_PCI_ATS)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("the ats setting is not supported with this "
                             "QEMU binary"));
            return -1;
        }
        virBufferAsprintf(buf, ",ats=%s",
                          virTristateSwitchTypeToString(virtio->ats));
    }

    return 0;
}

static int
qemuBuildRomStr(virBufferPtr buf,
                virDomainDeviceInfoPtr info)
{
    if (info->romenabled || info->rombar || info->romfile) {
        if (info->type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           "%s", _("ROM tuning is only supported for PCI devices"));
            return -1;
        }

        /* Passing an empty romfile= tells QEMU to disable ROM entirely for
         * this device, and makes other settings irrelevant */
        if (info->romenabled == VIR_TRISTATE_BOOL_NO) {
            virBufferAddLit(buf, ",romfile=");
            return 0;
        }

        switch (info->rombar) {
        case VIR_TRISTATE_SWITCH_OFF:
            virBufferAddLit(buf, ",rombar=0");
            break;
        case VIR_TRISTATE_SWITCH_ON:
            virBufferAddLit(buf, ",rombar=1");
            break;
        default:
            break;
        }
        if (info->romfile) {
           virBufferAddLit(buf, ",romfile=");
           virQEMUBuildBufferEscapeComma(buf, info->romfile);
        }
    }

    return 0;
}

static int
qemuBuildIoEventFdStr(virBufferPtr buf,
                      virTristateSwitch use,
                      virQEMUCapsPtr qemuCaps)
{
    if (use && virQEMUCapsGet(qemuCaps, QEMU_CAPS_VIRTIO_IOEVENTFD))
        virBufferAsprintf(buf, ",ioeventfd=%s",
                          virTristateSwitchTypeToString(use));
    return 0;
}

#define QEMU_SERIAL_PARAM_ACCEPTED_CHARS \
  "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_ .+"

static int
qemuSafeSerialParamValue(const char *value)
{
    if (strspn(value, QEMU_SERIAL_PARAM_ACCEPTED_CHARS) != strlen(value)) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("driver serial '%s' contains unsafe characters"),
                       value);
        return -1;
    }

    return 0;
}


/**
 * qemuBuildSecretInfoProps:
 * @secinfo: pointer to the secret info object
 * @props: json properties to return
 *
 * Build the JSON properties for the secret info type.
 *
 * Returns 0 on success with the filled in JSON property; otherwise,
 * returns -1 on failure error message set.
 */
int
qemuBuildSecretInfoProps(qemuDomainSecretInfoPtr secinfo,
                         virJSONValuePtr *propsret)
{
    int ret = -1;
    char *keyid = NULL;

    if (!(keyid = qemuDomainGetMasterKeyAlias()))
        return -1;

    ret = qemuMonitorCreateObjectProps(propsret,
                                       "secret", secinfo->s.aes.alias,
                                       "s:data", secinfo->s.aes.ciphertext,
                                       "s:keyid", keyid,
                                       "s:iv", secinfo->s.aes.iv,
                                       "s:format", "base64", NULL);

    VIR_FREE(keyid);
    return ret;
}


/**
 * qemuBuildObjectSecretCommandLine:
 * @cmd: the command to modify
 * @secinfo: pointer to the secret info object
 *
 * If the secinfo is available and associated with an AES secret,
 * then format the command line for the secret object. This object
 * will be referenced by the device that needs/uses it, so it needs
 * to be in place first.
 *
 * Returns 0 on success, -1 w/ error message on failure
 */
static int
qemuBuildObjectSecretCommandLine(virCommandPtr cmd,
                                 qemuDomainSecretInfoPtr secinfo)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    int ret = -1;
    virJSONValuePtr props = NULL;

    if (qemuBuildSecretInfoProps(secinfo, &props) < 0)
        return -1;

    if (virQEMUBuildObjectCommandlineFromJSON(&buf, props) < 0)
        goto cleanup;

    virCommandAddArg(cmd, "-object");
    virCommandAddArgBuffer(cmd, &buf);

    ret  = 0;

 cleanup:
    virBufferFreeAndReset(&buf);
    virJSONValueFree(props);
    return ret;
}


/* qemuBuildDiskSecinfoCommandLine:
 * @cmd: Pointer to the command string
 * @secinfo: Pointer to a possible secinfo
 *
 * Add the secret object for the disks that will be using it to perform
 * their authentication.
 *
 * Returns 0 on success, -1 w/ error on some sort of failure.
 */
static int
qemuBuildDiskSecinfoCommandLine(virCommandPtr cmd,
                                qemuDomainSecretInfoPtr secinfo)
{
    /* Not necessary for non AES secrets */
    if (!secinfo || secinfo->type != VIR_DOMAIN_SECRET_INFO_TYPE_AES)
        return 0;

    return qemuBuildObjectSecretCommandLine(cmd, secinfo);
}


/* qemuBuildGeneralSecinfoURI:
 * @uri: Pointer to the URI structure to add to
 * @secinfo: Pointer to the secret info data (if present)
 *
 * If we have a secinfo, then build the command line options for
 * the secret info for the "general" case (somewhat a misnomer since
 * an iscsi disk is the only one with a secinfo).
 *
 * Returns 0 on success or if no secinfo,
 * -1 and error message if fail to add secret information
 */
static int
qemuBuildGeneralSecinfoURI(virURIPtr uri,
                           qemuDomainSecretInfoPtr secinfo)
{
    if (!secinfo)
        return 0;

    switch ((qemuDomainSecretInfoType) secinfo->type) {
    case VIR_DOMAIN_SECRET_INFO_TYPE_PLAIN:
        if (secinfo->s.plain.secret) {
            if (!virStringBufferIsPrintable(secinfo->s.plain.secret,
                                            secinfo->s.plain.secretlen)) {
                virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                               _("found non printable characters in secret"));
                return -1;
            }
            if (virAsprintf(&uri->user, "%s:%s",
                            secinfo->s.plain.username,
                            secinfo->s.plain.secret) < 0)
                return -1;
        } else {
            if (VIR_STRDUP(uri->user, secinfo->s.plain.username) < 0)
                return -1;
        }
        break;

    case VIR_DOMAIN_SECRET_INFO_TYPE_AES:
    case VIR_DOMAIN_SECRET_INFO_TYPE_LAST:
        return -1;
    }

    return 0;
}


/* qemuBuildRBDSecinfoURI:
 * @uri: Pointer to the URI structure to add to
 * @secinfo: Pointer to the secret info data (if present)
 *
 * If we have a secinfo, then build the command line options for
 * the secret info for the RBD network storage. Assumption for this
 * is both username and secret exist for plaintext
 *
 * Returns 0 on success or if no secinfo,
 * -1 and error message if fail to add secret information
 */
static int
qemuBuildRBDSecinfoURI(virBufferPtr buf,
                       qemuDomainSecretInfoPtr secinfo)
{
    VIR_AUTODISPOSE_STR base64secret = NULL;

    if (!secinfo) {
        virBufferAddLit(buf, ":auth_supported=none");
        return 0;
    }

    switch ((qemuDomainSecretInfoType) secinfo->type) {
    case VIR_DOMAIN_SECRET_INFO_TYPE_PLAIN:
        if (!(base64secret = virStringEncodeBase64(secinfo->s.plain.secret,
                                                   secinfo->s.plain.secretlen)))
            return -1;
        virBufferEscape(buf, '\\', ":", ":id=%s", secinfo->s.plain.username);
        virBufferEscape(buf, '\\', ":",
                        ":key=%s:auth_supported=cephx\\;none",
                        base64secret);
        break;

    case VIR_DOMAIN_SECRET_INFO_TYPE_AES:
        virBufferEscape(buf, '\\', ":", ":id=%s:auth_supported=cephx\\;none",
                        secinfo->s.aes.username);
        break;

    case VIR_DOMAIN_SECRET_INFO_TYPE_LAST:
        return -1;
    }

    return 0;
}


/* qemuBuildTLSx509BackendProps:
 * @tlspath: path to the TLS credentials
 * @listen: boolen listen for client or server setting
 * @verifypeer: boolean to enable peer verification (form of authorization)
 * @alias: alias for the TLS credentials object
 * @secalias: if one exists, the alias of the security object for passwordid
 * @qemuCaps: capabilities
 * @propsret: json properties to return
 *
 * Create a backend string for the tls-creds-x509 object.
 *
 * Returns 0 on success, -1 on failure with error set.
 */
int
qemuBuildTLSx509BackendProps(const char *tlspath,
                             bool isListen,
                             bool verifypeer,
                             const char *alias,
                             const char *secalias,
                             virQEMUCapsPtr qemuCaps,
                             virJSONValuePtr *propsret)
{
    if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_OBJECT_TLS_CREDS_X509)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("tls-creds-x509 not supported in this QEMU binary"));
        return -1;
    }

    if (qemuMonitorCreateObjectProps(propsret, "tls-creds-x509", alias,
                                     "s:dir", tlspath,
                                     "s:endpoint", (isListen ? "server": "client"),
                                     "b:verify-peer", (isListen ? verifypeer : true),
                                     "S:passwordid", secalias,
                                     NULL) < 0)
        return -1;

    return 0;
}


/* qemuBuildTLSx509CommandLine:
 * @cmd: Pointer to command
 * @tlspath: path to the TLS credentials
 * @listen: boolen listen for client or server setting
 * @verifypeer: boolean to enable peer verification (form of authorization)
 * @certEncSecretAlias: alias of a 'secret' object for decrypting TLS private key
 *                      (optional)
 * @alias: TLS object alias
 * @qemuCaps: capabilities
 *
 * Create the command line for a TLS object
 *
 * Returns 0 on success, -1 on failure with error set.
 */
static int
qemuBuildTLSx509CommandLine(virCommandPtr cmd,
                            const char *tlspath,
                            bool isListen,
                            bool verifypeer,
                            const char *certEncSecretAlias,
                            const char *alias,
                            virQEMUCapsPtr qemuCaps)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    int ret = -1;
    virJSONValuePtr props = NULL;

    if (qemuBuildTLSx509BackendProps(tlspath, isListen, verifypeer, alias,
                                     certEncSecretAlias, qemuCaps, &props) < 0)
        goto cleanup;

    if (virQEMUBuildObjectCommandlineFromJSON(&buf, props) < 0)
        goto cleanup;

    virCommandAddArg(cmd, "-object");
    virCommandAddArgBuffer(cmd, &buf);

    ret = 0;

 cleanup:
    virBufferFreeAndReset(&buf);
    virJSONValueFree(props);
    return ret;
}


static char *
qemuBuildNetworkDriveURI(virStorageSourcePtr src,
                         qemuDomainSecretInfoPtr secinfo)
{
    virURIPtr uri = NULL;
    char *ret = NULL;

    if (!(uri = qemuBlockStorageSourceGetURI(src)))
        goto cleanup;

    if (src->hosts->socket &&
        virAsprintf(&uri->query, "socket=%s", src->hosts->socket) < 0)
        goto cleanup;

    if (qemuBuildGeneralSecinfoURI(uri, secinfo) < 0)
        goto cleanup;

    ret = virURIFormat(uri);

 cleanup:
    virURIFree(uri);
    return ret;
}


static char *
qemuBuildNetworkDriveStr(virStorageSourcePtr src,
                         qemuDomainSecretInfoPtr secinfo)
{
    char *ret = NULL;
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    size_t i;

    switch ((virStorageNetProtocol) src->protocol) {
        case VIR_STORAGE_NET_PROTOCOL_NBD:
            if (src->nhosts != 1) {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("protocol '%s' accepts only one host"),
                               virStorageNetProtocolTypeToString(src->protocol));
                goto cleanup;
            }

            if (!((src->hosts->name && strchr(src->hosts->name, ':')) ||
                  (src->hosts->transport == VIR_STORAGE_NET_HOST_TRANS_TCP &&
                   !src->hosts->name) ||
                  (src->hosts->transport == VIR_STORAGE_NET_HOST_TRANS_UNIX &&
                   src->hosts->socket &&
                   src->hosts->socket[0] != '/'))) {

                virBufferAddLit(&buf, "nbd:");

                switch (src->hosts->transport) {
                case VIR_STORAGE_NET_HOST_TRANS_TCP:
                    virBufferAsprintf(&buf, "%s:%u",
                                      src->hosts->name, src->hosts->port);
                    break;

                case VIR_STORAGE_NET_HOST_TRANS_UNIX:
                    if (!src->hosts->socket) {
                        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                                       _("socket attribute required for "
                                         "unix transport"));
                        goto cleanup;
                    }

                    virBufferAsprintf(&buf, "unix:%s", src->hosts->socket);
                    break;

                default:
                    virReportError(VIR_ERR_INTERNAL_ERROR,
                                   _("nbd does not support transport '%s'"),
                                   virStorageNetHostTransportTypeToString(src->hosts->transport));
                    goto cleanup;
                }

                if (src->path)
                    virBufferAsprintf(&buf, ":exportname=%s", src->path);

                if (virBufferCheckError(&buf) < 0)
                    goto cleanup;

                ret = virBufferContentAndReset(&buf);
                goto cleanup;
            }
            /* NBD code uses URI formatting scheme as others in some cases */
            ret = qemuBuildNetworkDriveURI(src, secinfo);
            break;

        case VIR_STORAGE_NET_PROTOCOL_HTTP:
        case VIR_STORAGE_NET_PROTOCOL_HTTPS:
        case VIR_STORAGE_NET_PROTOCOL_FTP:
        case VIR_STORAGE_NET_PROTOCOL_FTPS:
        case VIR_STORAGE_NET_PROTOCOL_TFTP:
        case VIR_STORAGE_NET_PROTOCOL_ISCSI:
        case VIR_STORAGE_NET_PROTOCOL_GLUSTER:
            ret = qemuBuildNetworkDriveURI(src, secinfo);
            break;

        case VIR_STORAGE_NET_PROTOCOL_SHEEPDOG:
            if (!src->path) {
                virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                               _("missing disk source for 'sheepdog' protocol"));
                goto cleanup;
            }

            if (src->nhosts == 0) {
                if (virAsprintf(&ret, "sheepdog:%s", src->path) < 0)
                    goto cleanup;
            } else if (src->nhosts == 1) {
                if (virAsprintf(&ret, "sheepdog:%s:%u:%s",
                                src->hosts->name, src->hosts->port,
                                src->path) < 0)
                    goto cleanup;
            } else {
                virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                               _("protocol 'sheepdog' accepts up to one host"));
                goto cleanup;
            }

            break;

        case VIR_STORAGE_NET_PROTOCOL_RBD:
            if (strchr(src->path, ':')) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                               _("':' not allowed in RBD source volume name '%s'"),
                               src->path);
                goto cleanup;
            }

            virBufferStrcat(&buf, "rbd:", src->volume, "/", src->path, NULL);

            if (src->snapshot)
                virBufferEscape(&buf, '\\', ":", "@%s", src->snapshot);

            if (qemuBuildRBDSecinfoURI(&buf, secinfo) < 0)
                goto cleanup;

            if (src->nhosts > 0) {
                virBufferAddLit(&buf, ":mon_host=");
                for (i = 0; i < src->nhosts; i++) {
                    if (i)
                        virBufferAddLit(&buf, "\\;");

                    /* assume host containing : is ipv6 */
                    if (strchr(src->hosts[i].name, ':'))
                        virBufferEscape(&buf, '\\', ":", "[%s]",
                                        src->hosts[i].name);
                    else
                        virBufferAsprintf(&buf, "%s", src->hosts[i].name);

                    if (src->hosts[i].port)
                        virBufferAsprintf(&buf, "\\:%u", src->hosts[i].port);
                }
            }

            if (src->configFile)
                virBufferEscape(&buf, '\\', ":", ":conf=%s", src->configFile);

            if (virBufferCheckError(&buf) < 0)
                goto cleanup;

            ret = virBufferContentAndReset(&buf);
            break;

        case VIR_STORAGE_NET_PROTOCOL_VXHS:
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("VxHS protocol does not support URI syntax"));
            goto cleanup;

        case VIR_STORAGE_NET_PROTOCOL_SSH:
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("'ssh' protocol is not yet supported"));
            goto cleanup;

        case VIR_STORAGE_NET_PROTOCOL_LAST:
        case VIR_STORAGE_NET_PROTOCOL_NONE:
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Unexpected network protocol '%s'"),
                           virStorageNetProtocolTypeToString(src->protocol));
            goto cleanup;
    }

 cleanup:
    virBufferFreeAndReset(&buf);

    return ret;
}


int
qemuGetDriveSourceString(virStorageSourcePtr src,
                         qemuDomainSecretInfoPtr secinfo,
                         char **source)
{
    int actualType = virStorageSourceGetActualType(src);
    int ret = -1;

    *source = NULL;

    /* return 1 for empty sources */
    if (virStorageSourceIsEmpty(src))
        return 1;

    switch ((virStorageType)actualType) {
    case VIR_STORAGE_TYPE_BLOCK:
    case VIR_STORAGE_TYPE_FILE:
    case VIR_STORAGE_TYPE_DIR:
        if (VIR_STRDUP(*source, src->path) < 0)
            goto cleanup;

        break;

    case VIR_STORAGE_TYPE_NETWORK:
        if (!(*source = qemuBuildNetworkDriveStr(src, secinfo)))
            goto cleanup;
        break;

    case VIR_STORAGE_TYPE_VOLUME:
    case VIR_STORAGE_TYPE_NONE:
    case VIR_STORAGE_TYPE_LAST:
        break;
    }

    ret = 0;

 cleanup:
    return ret;
}


static bool
qemuDiskConfigBlkdeviotuneHasBasic(virDomainDiskDefPtr disk)
{
    return disk->blkdeviotune.total_bytes_sec ||
           disk->blkdeviotune.read_bytes_sec ||
           disk->blkdeviotune.write_bytes_sec ||
           disk->blkdeviotune.total_iops_sec ||
           disk->blkdeviotune.read_iops_sec ||
           disk->blkdeviotune.write_iops_sec;
}


static bool
qemuDiskConfigBlkdeviotuneHasMax(virDomainDiskDefPtr disk)
{
    return disk->blkdeviotune.total_bytes_sec_max ||
           disk->blkdeviotune.read_bytes_sec_max ||
           disk->blkdeviotune.write_bytes_sec_max ||
           disk->blkdeviotune.total_iops_sec_max ||
           disk->blkdeviotune.read_iops_sec_max ||
           disk->blkdeviotune.write_iops_sec_max ||
           disk->blkdeviotune.size_iops_sec;
}


static bool
qemuDiskConfigBlkdeviotuneHasMaxLength(virDomainDiskDefPtr disk)
{
    return disk->blkdeviotune.total_bytes_sec_max_length ||
           disk->blkdeviotune.read_bytes_sec_max_length ||
           disk->blkdeviotune.write_bytes_sec_max_length ||
           disk->blkdeviotune.total_iops_sec_max_length ||
           disk->blkdeviotune.read_iops_sec_max_length ||
           disk->blkdeviotune.write_iops_sec_max_length;
}


bool
qemuDiskConfigBlkdeviotuneEnabled(virDomainDiskDefPtr disk)
{
    return !!disk->blkdeviotune.group_name ||
           qemuDiskConfigBlkdeviotuneHasBasic(disk) ||
           qemuDiskConfigBlkdeviotuneHasMax(disk) ||
           qemuDiskConfigBlkdeviotuneHasMaxLength(disk);
}


/**
 * qemuCheckDiskConfigBlkdeviotune:
 * @disk: disk configuration
 * @qemuCaps: qemu capabilities, NULL if checking cold-configuration
 *
 * Checks whether block io tuning settings make sense. Returns -1 on error and
 * reports a proper libvirt error.
 */
static int
qemuCheckDiskConfigBlkdeviotune(virDomainDiskDefPtr disk,
                                virQEMUCapsPtr qemuCaps)
{
    /* group_name by itself is ignored by qemu */
    if (disk->blkdeviotune.group_name &&
        !qemuDiskConfigBlkdeviotuneHasBasic(disk) &&
        !qemuDiskConfigBlkdeviotuneHasMax(disk) &&
        !qemuDiskConfigBlkdeviotuneHasMaxLength(disk)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("group_name can be configured only together with "
                         "settings"));
        return -1;
    }

    if (disk->blkdeviotune.total_bytes_sec > QEMU_BLOCK_IOTUNE_MAX ||
        disk->blkdeviotune.read_bytes_sec > QEMU_BLOCK_IOTUNE_MAX ||
        disk->blkdeviotune.write_bytes_sec > QEMU_BLOCK_IOTUNE_MAX ||
        disk->blkdeviotune.total_iops_sec > QEMU_BLOCK_IOTUNE_MAX ||
        disk->blkdeviotune.read_iops_sec > QEMU_BLOCK_IOTUNE_MAX ||
        disk->blkdeviotune.write_iops_sec > QEMU_BLOCK_IOTUNE_MAX ||
        disk->blkdeviotune.total_bytes_sec_max > QEMU_BLOCK_IOTUNE_MAX ||
        disk->blkdeviotune.read_bytes_sec_max > QEMU_BLOCK_IOTUNE_MAX ||
        disk->blkdeviotune.write_bytes_sec_max > QEMU_BLOCK_IOTUNE_MAX ||
        disk->blkdeviotune.total_iops_sec_max > QEMU_BLOCK_IOTUNE_MAX ||
        disk->blkdeviotune.read_iops_sec_max > QEMU_BLOCK_IOTUNE_MAX ||
        disk->blkdeviotune.write_iops_sec_max > QEMU_BLOCK_IOTUNE_MAX ||
        disk->blkdeviotune.size_iops_sec > QEMU_BLOCK_IOTUNE_MAX) {
        virReportError(VIR_ERR_ARGUMENT_UNSUPPORTED,
                      _("block I/O throttle limit must "
                        "be no more than %llu using QEMU"), QEMU_BLOCK_IOTUNE_MAX);
        return -1;
    }

    if (qemuCaps) {
        /* block I/O throttling 1.7 */
        if (qemuDiskConfigBlkdeviotuneHasMax(disk) &&
            !virQEMUCapsGet(qemuCaps, QEMU_CAPS_DRIVE_IOTUNE_MAX)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("there are some block I/O throttling parameters "
                             "that are not supported with this QEMU binary"));
            return -1;
        }

        /* block I/O group 2.4 */
        if (disk->blkdeviotune.group_name &&
            !virQEMUCapsGet(qemuCaps, QEMU_CAPS_DRIVE_IOTUNE_GROUP)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("the block I/O throttling group parameter is "
                             "not supported with this QEMU binary"));
            return -1;
        }

        /* block I/O throttling length 2.6 */
        if (qemuDiskConfigBlkdeviotuneHasMaxLength(disk) &&
            !virQEMUCapsGet(qemuCaps, QEMU_CAPS_DRIVE_IOTUNE_MAX_LENGTH)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("there are some block I/O throttling length parameters "
                             "that are not supported with this QEMU binary"));
            return -1;
        }
    }

    return 0;
}


/**
 * qemuCheckDiskConfig:
 * @disk: disk definition
 * @qemuCaps: qemu capabilities, may be NULL for cold-plug check
 *
 * Perform disk definition config validity checks. Returns -1 on error with
 * error reported.
 */
int
qemuCheckDiskConfig(virDomainDiskDefPtr disk,
                    virQEMUCapsPtr qemuCaps)
{
    if (qemuCheckDiskConfigBlkdeviotune(disk, qemuCaps) < 0)
        return -1;

    if (virDiskNameToIndex(disk->dst) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unsupported disk type '%s'"), disk->dst);
        return -1;
    }

    if (disk->wwn) {
        if ((disk->bus != VIR_DOMAIN_DISK_BUS_IDE) &&
            (disk->bus != VIR_DOMAIN_DISK_BUS_SCSI)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("Only ide and scsi disk support wwn"));
            return -1;
        }
    }

    if ((disk->vendor || disk->product) &&
        disk->bus != VIR_DOMAIN_DISK_BUS_SCSI) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("Only scsi disk supports vendor and product"));
            return -1;
    }

    if (disk->device == VIR_DOMAIN_DISK_DEVICE_LUN) {
        /* make sure that both the bus supports type='lun' (SG_IO). */
        if (disk->bus != VIR_DOMAIN_DISK_BUS_VIRTIO &&
            disk->bus != VIR_DOMAIN_DISK_BUS_SCSI) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("disk device='lun' is not supported for bus='%s'"),
                           virDomainDiskQEMUBusTypeToString(disk->bus));
            return -1;
        }

        if (disk->bus == VIR_DOMAIN_DISK_BUS_SCSI &&
            disk->src->format != VIR_STORAGE_FILE_RAW) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("disk device 'lun' using target 'scsi' must use "
                             "'raw' format"));
            return -1;
        }

        if (qemuDomainDefValidateDiskLunSource(disk->src) < 0)
            return -1;

        if (disk->wwn) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("Setting wwn is not supported for lun device"));
            return -1;
        }
        if (disk->vendor || disk->product) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("Setting vendor or product is not supported "
                             "for lun device"));
            return -1;
        }
    }

    switch (disk->bus) {
    case VIR_DOMAIN_DISK_BUS_SCSI:
        if (disk->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_DRIVE) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("unexpected address type for scsi disk"));
            return -1;
        }

        /* Setting bus= attr for SCSI drives, causes a controller
         * to be created. Yes this is slightly odd. It is not possible
         * to have > 1 bus on a SCSI controller (yet). */
        if (disk->info.addr.drive.bus != 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           "%s", _("SCSI controller only supports 1 bus"));
            return -1;
        }
        break;

    case VIR_DOMAIN_DISK_BUS_IDE:
        if (disk->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_DRIVE) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("unexpected address type for ide disk"));
            return -1;
        }
        /* We can only have 1 IDE controller (currently) */
        if (disk->info.addr.drive.controller != 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("Only 1 IDE controller is supported"));
            return -1;
        }
        break;

    case VIR_DOMAIN_DISK_BUS_FDC:
        if (disk->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_DRIVE) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("unexpected address type for fdc disk"));
            return -1;
        }
        /* We can only have 1 FDC controller (currently) */
        if (disk->info.addr.drive.controller != 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("Only 1 fdc controller is supported"));
            return -1;
        }
        /* We can only have 1 FDC bus (currently) */
        if (disk->info.addr.drive.bus != 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("Only 1 fdc bus is supported"));
            return -1;
        }
        if (disk->info.addr.drive.target != 0) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("target must be 0 for controller fdc"));
            return -1;
        }
        break;

    case VIR_DOMAIN_DISK_BUS_VIRTIO:
    case VIR_DOMAIN_DISK_BUS_XEN:
    case VIR_DOMAIN_DISK_BUS_SD:
        break;
    }

    if (disk->src->readonly &&
        disk->device == VIR_DOMAIN_DISK_DEVICE_DISK) {
        if (disk->bus == VIR_DOMAIN_DISK_BUS_IDE) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("readonly ide disks are not supported"));
            return -1;
        }

        if (disk->bus == VIR_DOMAIN_DISK_BUS_SATA) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("readonly sata disks are not supported"));
            return -1;
        }
    }

    if (disk->transient) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("transient disks not supported yet"));
        return -1;
    }

    if (disk->iomode == VIR_DOMAIN_DISK_IO_NATIVE &&
        disk->cachemode != VIR_DOMAIN_DISK_CACHE_DIRECTSYNC &&
        disk->cachemode != VIR_DOMAIN_DISK_CACHE_DISABLE) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("native I/O needs either no disk cache "
                         "or directsync cache mode, QEMU will fallback "
                         "to aio=threads"));
        return -1;
    }

    if (qemuCaps) {
        if (disk->serial &&
            disk->bus == VIR_DOMAIN_DISK_BUS_SCSI &&
            disk->device == VIR_DOMAIN_DISK_DEVICE_LUN) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("scsi-block 'lun' devices do not support the "
                             "serial property"));
            return -1;
        }

        if (disk->discard &&
            !virQEMUCapsGet(qemuCaps, QEMU_CAPS_DRIVE_DISCARD)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("discard is not supported by this QEMU binary"));
            return -1;
        }

        if (disk->detect_zeroes &&
            !virQEMUCapsGet(qemuCaps, QEMU_CAPS_DRIVE_DETECT_ZEROES)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("detect_zeroes is not supported by this QEMU binary"));
            return -1;
        }
    }

    if (disk->serial &&
        qemuSafeSerialParamValue(disk->serial) < 0)
        return -1;

    return 0;
}


/* QEMU 1.2 and later have a binary flag -enable-fips that must be
 * used for VNC auth to obey FIPS settings; but the flag only
 * exists on Linux, and with no way to probe for it via QMP.  Our
 * solution: if FIPS mode is required, then unconditionally use
 * the flag, regardless of qemu version, for the following matrix:
 *
 *                          old QEMU            new QEMU
 * FIPS enabled             doesn't start       VNC auth disabled
 * FIPS disabled/missing    VNC auth enabled    VNC auth enabled
 */
bool
qemuCheckFips(void)
{
    bool ret = false;

    if (virFileExists("/proc/sys/crypto/fips_enabled")) {
        char *buf = NULL;

        if (virFileReadAll("/proc/sys/crypto/fips_enabled", 10, &buf) < 0)
            return ret;
        if (STREQ(buf, "1\n"))
            ret = true;
        VIR_FREE(buf);
    }

    return ret;
}


/**
 * qemuDiskBusNeedsDriveArg:
 * @bus: disk bus
 *
 * Unfortunately it is not possible to use -device for SD devices.
 * Fortunately, those don't need static PCI addresses, so we can use -drive
 * without -device.
 */
bool
qemuDiskBusNeedsDriveArg(int bus)
{
    return bus == VIR_DOMAIN_DISK_BUS_SD;
}


/**
 * qemuDiskSourceNeedsProps:
 * @src: disk source
 *
 * Returns true, if the disk source needs to be generated from the JSON
 * representation. Otherwise, the disk source should be represented using
 * the legacy representation.
 */
static bool
qemuDiskSourceNeedsProps(virStorageSourcePtr src,
                         virQEMUCapsPtr qemuCaps)
{
    int actualType = virStorageSourceGetActualType(src);

    if (actualType == VIR_STORAGE_TYPE_NETWORK &&
        src->protocol == VIR_STORAGE_NET_PROTOCOL_GLUSTER &&
        src->nhosts > 1)
        return true;

    if (actualType == VIR_STORAGE_TYPE_NETWORK &&
        src->protocol == VIR_STORAGE_NET_PROTOCOL_VXHS)
        return true;

    if (actualType == VIR_STORAGE_TYPE_NETWORK &&
        src->protocol == VIR_STORAGE_NET_PROTOCOL_ISCSI &&
        virQEMUCapsGet(qemuCaps, QEMU_CAPS_ISCSI_PASSWORD_SECRET))
        return true;

    if (actualType == VIR_STORAGE_TYPE_NETWORK &&
        src->protocol == VIR_STORAGE_NET_PROTOCOL_NBD &&
        src->haveTLS == VIR_TRISTATE_BOOL_YES)
        return true;

    return false;
}


/**
 * qemuDiskSourceGetProps:
 * @src: disk source struct
 *
 * Returns the disk source struct wrapped so that it can be used as disk source
 * directly by converting it from json.
 */
static virJSONValuePtr
qemuDiskSourceGetProps(virStorageSourcePtr src)
{
    virJSONValuePtr props;
    virJSONValuePtr ret;

    if (!(props = qemuBlockStorageSourceGetBackendProps(src, true)))
        return NULL;

    if (virJSONValueObjectCreate(&ret, "a:file", &props, NULL) < 0) {
        virJSONValueFree(props);
        return NULL;
    }

    return ret;
}


static int
qemuBuildDriveSourcePR(virBufferPtr buf,
                       virDomainDiskDefPtr disk)
{
    char *alias = NULL;
    const char *defaultAlias = NULL;

    if (!disk->src->pr)
        return 0;

    if (virStoragePRDefIsManaged(disk->src->pr))
        defaultAlias = qemuDomainGetManagedPRAlias();
    else if (!(alias = qemuDomainGetUnmanagedPRAlias(disk->info.alias)))
        return -1;


    virBufferAsprintf(buf, ",file.pr-manager=%s", alias ? alias : defaultAlias);
    VIR_FREE(alias);
    return 0;
}


static int
qemuBuildDriveSourceStr(virDomainDiskDefPtr disk,
                        virQEMUCapsPtr qemuCaps,
                        virBufferPtr buf)
{
    int actualType = virStorageSourceGetActualType(disk->src);
    qemuDomainStorageSourcePrivatePtr srcpriv = QEMU_DOMAIN_STORAGE_SOURCE_PRIVATE(disk->src);
    qemuDomainSecretInfoPtr secinfo = NULL;
    qemuDomainSecretInfoPtr encinfo = NULL;
    virJSONValuePtr srcprops = NULL;
    char *source = NULL;
    bool rawluks = false;
    int ret = -1;

    if (srcpriv) {
        secinfo = srcpriv->secinfo;
        encinfo = srcpriv->encinfo;
    }

    if (qemuDiskSourceNeedsProps(disk->src, qemuCaps) &&
        !(srcprops = qemuDiskSourceGetProps(disk->src)))
        goto cleanup;

    if (!srcprops &&
        qemuGetDriveSourceString(disk->src, secinfo, &source) < 0)
        goto cleanup;

    /* nothing to format if the drive is empty */
    if (!(source || srcprops) ||
        ((disk->device == VIR_DOMAIN_DISK_DEVICE_FLOPPY ||
          disk->device == VIR_DOMAIN_DISK_DEVICE_CDROM) &&
         disk->tray_status == VIR_DOMAIN_DISK_TRAY_OPEN)) {
        ret = 0;
        goto cleanup;
    }

    if (actualType == VIR_STORAGE_TYPE_BLOCK &&
        disk->tray_status == VIR_DOMAIN_DISK_TRAY_OPEN) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       disk->src->type == VIR_STORAGE_TYPE_VOLUME ?
                       _("tray status 'open' is invalid for block type volume") :
                       _("tray status 'open' is invalid for block type disk"));
        goto cleanup;
    }

    if (source) {
        virBufferAddLit(buf, "file=");

        /* for now the DIR based storage is handled by the magic FAT format */
        if (actualType == VIR_STORAGE_TYPE_DIR) {
            virBufferAddLit(buf, "fat:");

            if (disk->device == VIR_DOMAIN_DISK_DEVICE_FLOPPY)
                virBufferAddLit(buf, "floppy:");
        }

        virQEMUBuildBufferEscapeComma(buf, source);

        if (secinfo && secinfo->type == VIR_DOMAIN_SECRET_INFO_TYPE_AES)
            virBufferAsprintf(buf, ",file.password-secret=%s", secinfo->s.aes.alias);

        if (disk->src->debug)
            virBufferAsprintf(buf, ",file.debug=%d", disk->src->debugLevel);

        if (qemuBuildDriveSourcePR(buf, disk) < 0)
            goto cleanup;
    } else {
        if (!(source = virQEMUBuildDriveCommandlineFromJSON(srcprops)))
            goto cleanup;

        virBufferAdd(buf, source, -1);
    }
    virBufferAddLit(buf, ",");

    if (encinfo) {
        if (disk->src->format == VIR_STORAGE_FILE_RAW) {
            virBufferAsprintf(buf, "key-secret=%s,", encinfo->s.aes.alias);
            rawluks = true;
        } else if (disk->src->format == VIR_STORAGE_FILE_QCOW2 &&
                   disk->src->encryption->format == VIR_STORAGE_ENCRYPTION_FORMAT_LUKS) {
            virBufferAddLit(buf, "encrypt.format=luks,");
            virBufferAsprintf(buf, "encrypt.key-secret=%s,", encinfo->s.aes.alias);
        }
    }

    if (disk->src->format > 0 &&
        actualType != VIR_STORAGE_TYPE_DIR) {
        const char *qemuformat = virStorageFileFormatTypeToString(disk->src->format);
        if (rawluks)
            qemuformat = "luks";
        virBufferAsprintf(buf, "format=%s,", qemuformat);
    }

    ret = 0;

 cleanup:
    VIR_FREE(source);
    virJSONValueFree(srcprops);
    return ret;
}


static void
qemuBuildDiskThrottling(virDomainDiskDefPtr disk,
                        virBufferPtr buf)
{
#define IOTUNE_ADD(_field, _label) \
    if (disk->blkdeviotune._field) { \
        virBufferAsprintf(buf, ",throttling." _label "=%llu", \
                          disk->blkdeviotune._field); \
    }

    IOTUNE_ADD(total_bytes_sec, "bps-total");
    IOTUNE_ADD(read_bytes_sec, "bps-read");
    IOTUNE_ADD(write_bytes_sec, "bps-write");
    IOTUNE_ADD(total_iops_sec, "iops-total");
    IOTUNE_ADD(read_iops_sec, "iops-read");
    IOTUNE_ADD(write_iops_sec, "iops-write");

    IOTUNE_ADD(total_bytes_sec_max, "bps-total-max");
    IOTUNE_ADD(read_bytes_sec_max, "bps-read-max");
    IOTUNE_ADD(write_bytes_sec_max, "bps-write-max");
    IOTUNE_ADD(total_iops_sec_max, "iops-total-max");
    IOTUNE_ADD(read_iops_sec_max, "iops-read-max");
    IOTUNE_ADD(write_iops_sec_max, "iops-write-max");

    IOTUNE_ADD(size_iops_sec, "iops-size");
    if (disk->blkdeviotune.group_name) {
        virBufferAddLit(buf, ",throttling.group=");
        virQEMUBuildBufferEscapeComma(buf, disk->blkdeviotune.group_name);
    }

    IOTUNE_ADD(total_bytes_sec_max_length, "bps-total-max-length");
    IOTUNE_ADD(read_bytes_sec_max_length, "bps-read-max-length");
    IOTUNE_ADD(write_bytes_sec_max_length, "bps-write-max-length");
    IOTUNE_ADD(total_iops_sec_max_length, "iops-total-max-length");
    IOTUNE_ADD(read_iops_sec_max_length, "iops-read-max-length");
    IOTUNE_ADD(write_iops_sec_max_length, "iops-write-max-length");
#undef IOTUNE_ADD
}


static void
qemuBuildDiskFrontendAttributeErrorPolicy(virDomainDiskDefPtr disk,
                                          virBufferPtr buf)
{
    const char *wpolicy = NULL;
    const char *rpolicy = NULL;

    if (disk->error_policy)
        wpolicy = virDomainDiskErrorPolicyTypeToString(disk->error_policy);

    if (disk->rerror_policy)
        rpolicy = virDomainDiskErrorPolicyTypeToString(disk->rerror_policy);

    if (disk->error_policy == VIR_DOMAIN_DISK_ERROR_POLICY_ENOSPACE) {
        /* in the case of enospace, the option is spelled
         * differently in qemu, and it's only valid for werror,
         * not for rerror, so leave rerror NULL.
         */
        wpolicy = "enospc";
    } else if (!rpolicy) {
        /* for other policies, rpolicy can match wpolicy */
        rpolicy = wpolicy;
    }

    if (wpolicy)
        virBufferAsprintf(buf, ",werror=%s", wpolicy);
    if (rpolicy)
        virBufferAsprintf(buf, ",rerror=%s", rpolicy);
}


static void
qemuBuildDiskFrontendAttributes(virDomainDiskDefPtr disk,
                                virBufferPtr buf)
{
    /* generate geometry command string */
    if (disk->geometry.cylinders > 0 &&
        disk->geometry.heads > 0 &&
        disk->geometry.sectors > 0) {
        virBufferAsprintf(buf, ",cyls=%u,heads=%u,secs=%u",
                          disk->geometry.cylinders,
                          disk->geometry.heads,
                          disk->geometry.sectors);

        if (disk->geometry.trans != VIR_DOMAIN_DISK_TRANS_DEFAULT)
            virBufferAsprintf(buf, ",bios-chs-trans=%s",
                              virDomainDiskGeometryTransTypeToString(disk->geometry.trans));
    }

    if (disk->serial) {
        virBufferAddLit(buf, ",serial=");
        virBufferEscape(buf, '\\', " ", "%s", disk->serial);
    }
}


static char *
qemuBuildDriveStr(virDomainDiskDefPtr disk,
                  virQEMUCapsPtr qemuCaps)
{
    virBuffer opt = VIR_BUFFER_INITIALIZER;
    int detect_zeroes = virDomainDiskGetDetectZeroesMode(disk->discard,
                                                         disk->detect_zeroes);

    if (qemuBuildDriveSourceStr(disk, qemuCaps, &opt) < 0)
        goto error;

    if (!qemuDiskBusNeedsDriveArg(disk->bus)) {
        char *drivealias = qemuAliasDiskDriveFromDisk(disk);
        if (!drivealias)
            goto error;

        virBufferAddLit(&opt, "if=none");
        virBufferAsprintf(&opt, ",id=%s", drivealias);
        VIR_FREE(drivealias);
    } else {
        int idx = virDiskNameToIndex(disk->dst);

        if (idx < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("unsupported disk type '%s'"), disk->dst);
            goto error;
        }

        /* if we are using -device this will be checked elsewhere */
        if (qemuCheckDiskConfig(disk, qemuCaps) < 0)
            goto error;

        virBufferAsprintf(&opt, "if=%s",
                          virDomainDiskQEMUBusTypeToString(disk->bus));
        virBufferAsprintf(&opt, ",index=%d", idx);
    }

    /* werror/rerror are really frontend attributes, but older
     * qemu requires them on -drive instead of -device */
    if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_USB_STORAGE_WERROR))
        qemuBuildDiskFrontendAttributeErrorPolicy(disk, &opt);

    if (disk->src->readonly)
        virBufferAddLit(&opt, ",readonly=on");

    /* qemu rejects some parameters for an empty -drive, so we need to skip
     * them in that case:
     * cache: modifies properties of the format driver which is not present
     * copy_on_read: really only works for floppies
     * discard: modifies properties of format driver
     * detect_zeroes: works but really depends on discard so it's useless
     * iomode: setting it to 'native' requires a specific cache mode
     */
    if (!virStorageSourceIsEmpty(disk->src)) {
        if (disk->cachemode) {
            virBufferAsprintf(&opt, ",cache=%s",
                              qemuDiskCacheV2TypeToString(disk->cachemode));
        }

        if (disk->copy_on_read) {
            virBufferAsprintf(&opt, ",copy-on-read=%s",
                              virTristateSwitchTypeToString(disk->copy_on_read));
        }

        if (disk->discard) {
            virBufferAsprintf(&opt, ",discard=%s",
                              virDomainDiskDiscardTypeToString(disk->discard));
        }

        if (detect_zeroes) {
            virBufferAsprintf(&opt, ",detect-zeroes=%s",
                              virDomainDiskDetectZeroesTypeToString(detect_zeroes));
        }

        if (disk->iomode) {
            virBufferAsprintf(&opt, ",aio=%s",
                              virDomainDiskIoTypeToString(disk->iomode));
        }
    }

    qemuBuildDiskThrottling(disk, &opt);

    if (virBufferCheckError(&opt) < 0)
        goto error;

    return virBufferContentAndReset(&opt);

 error:
    virBufferFreeAndReset(&opt);
    return NULL;
}


static bool
qemuCheckIOThreads(const virDomainDef *def,
                   virDomainDiskDefPtr disk)
{
    /* Right "type" of disk" */
    switch ((virDomainDiskBus)disk->bus) {
    case VIR_DOMAIN_DISK_BUS_VIRTIO:
        if (disk->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI &&
            disk->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_CCW) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                            _("IOThreads only available for virtio pci and "
                              "virtio ccw disk"));
            return false;
        }
        break;

    case VIR_DOMAIN_DISK_BUS_IDE:
    case VIR_DOMAIN_DISK_BUS_FDC:
    case VIR_DOMAIN_DISK_BUS_SCSI:
    case VIR_DOMAIN_DISK_BUS_XEN:
    case VIR_DOMAIN_DISK_BUS_USB:
    case VIR_DOMAIN_DISK_BUS_UML:
    case VIR_DOMAIN_DISK_BUS_SATA:
    case VIR_DOMAIN_DISK_BUS_SD:
    case VIR_DOMAIN_DISK_BUS_LAST:
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("IOThreads not available for bus %s target %s"),
                       virDomainDiskBusTypeToString(disk->bus), disk->dst);
        return false;
    }

    /* Can we find the disk iothread in the iothreadid list? */
    if (!virDomainIOThreadIDFind(def, disk->iothread)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Disk iothread '%u' not defined in iothreadid"),
                       disk->iothread);
        return false;
    }

    return true;
}


static int
qemuBuildDriveDevCacheStr(virDomainDiskDefPtr disk,
                          virBufferPtr buf,
                          virQEMUCapsPtr qemuCaps)
{
    bool wb;

    if (disk->cachemode == VIR_DOMAIN_DISK_CACHE_DEFAULT)
        return 0;

    /* VIR_DOMAIN_DISK_DEVICE_LUN translates into 'scsi-block'
     * where any caching setting makes no sense. */
    if (disk->device == VIR_DOMAIN_DISK_DEVICE_LUN)
        return 0;

    if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_DISK_WRITE_CACHE))
        return 0;

    if (qemuDomainDiskCachemodeFlags(disk->cachemode, &wb, NULL, NULL) < 0)
        return -1;

    virBufferStrcat(buf, ",write-cache=",
                    virTristateSwitchTypeToString(virTristateSwitchFromBool(wb)),
                    NULL);

    return 0;
}


char *
qemuBuildDiskDeviceStr(const virDomainDef *def,
                       virDomainDiskDefPtr disk,
                       unsigned int bootindex,
                       virQEMUCapsPtr qemuCaps)
{
    virBuffer opt = VIR_BUFFER_INITIALIZER;
    const char *bus = virDomainDiskQEMUBusTypeToString(disk->bus);
    const char *contAlias;
    char *backendAlias = NULL;
    VIR_AUTOFREE(char *) scsiVPDDeviceId = NULL;
    int controllerModel;

    if (qemuCheckDiskConfig(disk, qemuCaps) < 0)
        goto error;

    if (!qemuDomainCheckCCWS390AddressSupport(def, &disk->info, qemuCaps, disk->dst))
        goto error;

    if (disk->iothread && !qemuCheckIOThreads(def, disk))
        goto error;

    switch ((virDomainDiskBus) disk->bus) {
    case VIR_DOMAIN_DISK_BUS_IDE:
        if (disk->info.addr.drive.target != 0) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("target must be 0 for ide controller"));
            goto error;
        }

        if (disk->wwn &&
            !virQEMUCapsGet(qemuCaps, QEMU_CAPS_IDE_DRIVE_WWN)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("Setting wwn for ide disk is not supported "
                             "by this QEMU"));
            goto error;
        }

        if (disk->device == VIR_DOMAIN_DISK_DEVICE_CDROM)
            virBufferAddLit(&opt, "ide-cd");
        else
            virBufferAddLit(&opt, "ide-hd");

        /* When domain has builtin IDE controller we don't put it onto cmd
         * line. Therefore we can't set its alias. In that case, use the
         * default one. */
        if (qemuDomainHasBuiltinIDE(def)) {
            contAlias = "ide";
        } else {
            if (!(contAlias = virDomainControllerAliasFind(def,
                                                           VIR_DOMAIN_CONTROLLER_TYPE_IDE,
                                                           disk->info.addr.drive.controller)))
                goto error;
        }
        virBufferAsprintf(&opt, ",bus=%s.%d,unit=%d",
                          contAlias,
                          disk->info.addr.drive.bus,
                          disk->info.addr.drive.unit);
        break;

    case VIR_DOMAIN_DISK_BUS_SCSI:
        if (disk->device == VIR_DOMAIN_DISK_DEVICE_LUN) {
            if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_SCSI_BLOCK)) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("This QEMU doesn't support scsi-block for "
                                 "lun passthrough"));
                goto error;
            }
        }

        if (disk->wwn &&
            !virQEMUCapsGet(qemuCaps, QEMU_CAPS_SCSI_DISK_WWN)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("Setting wwn for scsi disk is not supported "
                             "by this QEMU"));
            goto error;
        }

        /* Properties wwn, vendor and product were introduced in the
         * same QEMU release (1.2.0).
         */
        if ((disk->vendor || disk->product) &&
            !virQEMUCapsGet(qemuCaps, QEMU_CAPS_SCSI_DISK_WWN)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("Setting vendor or product for scsi disk is not "
                             "supported by this QEMU"));
            goto error;
        }

        controllerModel = qemuDomainFindSCSIControllerModel(def, &disk->info);
        if (controllerModel < 0)
            goto error;

        if (disk->device == VIR_DOMAIN_DISK_DEVICE_LUN) {
            virBufferAddLit(&opt, "scsi-block");
        } else {
            if (disk->device == VIR_DOMAIN_DISK_DEVICE_CDROM)
                virBufferAddLit(&opt, "scsi-cd");
            else
                virBufferAddLit(&opt, "scsi-hd");

            /* qemu historically used the name of -drive as one of the device
             * ids in the Vital Product Data Device Identification page if
             * disk serial was not set and the disk serial otherwise.
             * To avoid a guest-visible regression we need to provide it
             * ourselves especially for cases when -blockdev will be used */
            if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_SCSI_DISK_DEVICE_ID)) {
                if (disk->serial) {
                    if (VIR_STRDUP(scsiVPDDeviceId, disk->serial) < 0)
                        goto error;
                } else {
                    if (!(scsiVPDDeviceId = qemuAliasDiskDriveFromDisk(disk)))
                        goto error;
                }
            }
        }

        if (!(contAlias = virDomainControllerAliasFind(def, VIR_DOMAIN_CONTROLLER_TYPE_SCSI,
                                                       disk->info.addr.drive.controller)))
           goto error;

        if (controllerModel == VIR_DOMAIN_CONTROLLER_MODEL_SCSI_LSILOGIC) {
            if (disk->info.addr.drive.target != 0) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("target must be 0 for controller "
                                 "model 'lsilogic'"));
                goto error;
            }

            virBufferAsprintf(&opt, ",bus=%s.%d,scsi-id=%d",
                              contAlias,
                              disk->info.addr.drive.bus,
                              disk->info.addr.drive.unit);
        } else {
            if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_SCSI_DISK_CHANNEL)) {
                if (disk->info.addr.drive.target > 7) {
                    virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                                   _("This QEMU doesn't support target "
                                     "greater than 7"));
                    goto error;
                }

                if (disk->info.addr.drive.bus != 0 &&
                    disk->info.addr.drive.unit != 0) {
                    virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                                   _("This QEMU only supports both bus and "
                                     "unit equal to 0"));
                    goto error;
                }
            }

            virBufferAsprintf(&opt, ",bus=%s.0,channel=%d,scsi-id=%d,lun=%d",
                              contAlias,
                              disk->info.addr.drive.bus,
                              disk->info.addr.drive.target,
                              disk->info.addr.drive.unit);
        }

        if (scsiVPDDeviceId)
            virBufferStrcat(&opt, ",device_id=", scsiVPDDeviceId, NULL);

        break;

    case VIR_DOMAIN_DISK_BUS_SATA:
        if (disk->info.addr.drive.bus != 0) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("bus must be 0 for ide controller"));
            goto error;
        }
        if (disk->info.addr.drive.target != 0) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("target must be 0 for ide controller"));
            goto error;
        }

        if (disk->device == VIR_DOMAIN_DISK_DEVICE_CDROM)
            virBufferAddLit(&opt, "ide-cd");
        else
            virBufferAddLit(&opt, "ide-hd");

        /* When domain has builtin SATA controller we don't put it onto cmd
         * line. Therefore we can't set its alias. In that case, use the
         * default one. */
        if (qemuDomainIsQ35(def) &&
            disk->info.addr.drive.controller == 0) {
            contAlias = "ide";
        } else {
            if (!(contAlias = virDomainControllerAliasFind(def,
                                                           VIR_DOMAIN_CONTROLLER_TYPE_SATA,
                                                           disk->info.addr.drive.controller)))
                goto error;
        }
        virBufferAsprintf(&opt, ",bus=%s.%d",
                          contAlias,
                          disk->info.addr.drive.unit);
        break;

    case VIR_DOMAIN_DISK_BUS_VIRTIO:
        if (qemuBuildVirtioDevStr(&opt, "virtio-blk", qemuCaps,
                                  VIR_DOMAIN_DEVICE_DISK, disk) < 0) {
            goto error;
        }

        if (disk->iothread)
            virBufferAsprintf(&opt, ",iothread=iothread%u", disk->iothread);

        qemuBuildIoEventFdStr(&opt, disk->ioeventfd, qemuCaps);
        if (disk->event_idx &&
            virQEMUCapsGet(qemuCaps, QEMU_CAPS_VIRTIO_BLK_EVENT_IDX)) {
            virBufferAsprintf(&opt, ",event_idx=%s",
                              virTristateSwitchTypeToString(disk->event_idx));
        }
        if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_VIRTIO_BLK_SCSI)) {
            /* if sg_io is true but the scsi option isn't supported,
             * that means it's just always on in this version of qemu.
             */
            virBufferAsprintf(&opt, ",scsi=%s",
                              (disk->device == VIR_DOMAIN_DISK_DEVICE_LUN)
                              ? "on" : "off");
        }

        if (disk->queues) {
            if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_VIRTIO_BLK_NUM_QUEUES)) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("num-queues property isn't supported by this "
                                 "QEMU binary"));
                goto error;
            }

            virBufferAsprintf(&opt, ",num-queues=%u", disk->queues);
        }

        if (qemuBuildVirtioOptionsStr(&opt, disk->virtio, qemuCaps) < 0)
            goto error;

        if (qemuBuildDeviceAddressStr(&opt, def, &disk->info, qemuCaps) < 0)
            goto error;
        break;

    case VIR_DOMAIN_DISK_BUS_USB:
        if (disk->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_NONE &&
            disk->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_USB) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("unexpected address type for usb disk"));
            goto error;
        }
        if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE_USB_STORAGE)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("This QEMU doesn't support '-device "
                             "usb-storage'"));
            goto error;

        }
        virBufferAddLit(&opt, "usb-storage");

        if (qemuBuildDeviceAddressStr(&opt, def, &disk->info, qemuCaps) < 0)
            goto error;
        break;

    case VIR_DOMAIN_DISK_BUS_FDC:
        virBufferAsprintf(&opt, "floppy,unit=%d", disk->info.addr.drive.unit);
        break;

    case VIR_DOMAIN_DISK_BUS_XEN:
    case VIR_DOMAIN_DISK_BUS_UML:
    case VIR_DOMAIN_DISK_BUS_SD:
    case VIR_DOMAIN_DISK_BUS_LAST:
    default:
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unsupported disk bus '%s' with device setup"), bus);
        goto error;
    }

    if (disk->src->shared &&
        virQEMUCapsGet(qemuCaps, QEMU_CAPS_DISK_SHARE_RW))
        virBufferAddLit(&opt, ",share-rw=on");

    if (qemuDomainDiskGetBackendAlias(disk, qemuCaps, &backendAlias) < 0)
        goto error;

    if (backendAlias)
        virBufferAsprintf(&opt, ",drive=%s", backendAlias);
    VIR_FREE(backendAlias);

    virBufferAsprintf(&opt, ",id=%s", disk->info.alias);
    if (bootindex)
        virBufferAsprintf(&opt, ",bootindex=%u", bootindex);
    if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_BLOCKIO)) {
        if (disk->blockio.logical_block_size > 0)
            virBufferAsprintf(&opt, ",logical_block_size=%u",
                              disk->blockio.logical_block_size);
        if (disk->blockio.physical_block_size > 0)
            virBufferAsprintf(&opt, ",physical_block_size=%u",
                              disk->blockio.physical_block_size);
    }

    if (disk->wwn) {
        if (STRPREFIX(disk->wwn, "0x"))
            virBufferAsprintf(&opt, ",wwn=%s", disk->wwn);
        else
            virBufferAsprintf(&opt, ",wwn=0x%s", disk->wwn);
    }

    if (disk->vendor) {
        virBufferAddLit(&opt, ",vendor=");
        virQEMUBuildBufferEscapeComma(&opt, disk->vendor);
    }

    if (disk->product) {
        virBufferAddLit(&opt, ",product=");
        virQEMUBuildBufferEscapeComma(&opt, disk->product);
    }

    if (disk->bus == VIR_DOMAIN_DISK_BUS_USB) {
        if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_USB_STORAGE_REMOVABLE)) {
            if (disk->removable == VIR_TRISTATE_SWITCH_ON)
                virBufferAddLit(&opt, ",removable=on");
            else
                virBufferAddLit(&opt, ",removable=off");
        } else {
            if (disk->removable != VIR_TRISTATE_SWITCH_ABSENT) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("This QEMU doesn't support setting the "
                                 "removable flag of USB storage devices"));
                goto error;
            }
        }
    }

    if (qemuBuildDriveDevCacheStr(disk, &opt, qemuCaps) < 0)
        goto error;

    qemuBuildDiskFrontendAttributes(disk, &opt);

    if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_USB_STORAGE_WERROR))
        qemuBuildDiskFrontendAttributeErrorPolicy(disk, &opt);

    if (virBufferCheckError(&opt) < 0)
        goto error;

    return virBufferContentAndReset(&opt);

 error:
    VIR_FREE(backendAlias);
    virBufferFreeAndReset(&opt);
    return NULL;
}

char *
qemuBuildZPCIDevStr(virDomainDeviceInfoPtr dev)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;

    virBufferAsprintf(&buf,
                      "zpci,uid=%u,fid=%u,target=%s,id=zpci%u",
                      dev->addr.pci.zpci.uid,
                      dev->addr.pci.zpci.fid,
                      dev->alias,
                      dev->addr.pci.zpci.uid);

    if (virBufferCheckError(&buf) < 0) {
        virBufferFreeAndReset(&buf);
        return NULL;
    }

    return virBufferContentAndReset(&buf);
}

static int
qemuCommandAddZPCIDevice(virCommandPtr cmd,
                         virDomainDeviceInfoPtr dev)
{
    char *devstr = NULL;

    virCommandAddArg(cmd, "-device");

    if (!(devstr = qemuBuildZPCIDevStr(dev)))
        return -1;

    virCommandAddArg(cmd, devstr);

    VIR_FREE(devstr);
    return 0;
}

static int
qemuCommandAddExtDevice(virCommandPtr cmd,
                        virDomainDeviceInfoPtr dev)
{
    if (dev->type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI ||
        dev->addr.pci.extFlags == VIR_PCI_ADDRESS_EXTENSION_NONE) {
        return 0;
    }

    if (dev->addr.pci.extFlags & VIR_PCI_ADDRESS_EXTENSION_ZPCI)
        return qemuCommandAddZPCIDevice(cmd, dev);

    return 0;
}

static int
qemuBuildFloppyCommandLineControllerOptions(virCommandPtr cmd,
                                            const virDomainDef *def,
                                            virQEMUCapsPtr qemuCaps,
                                            unsigned int bootFloppy)
{
    virBuffer fdc_opts = VIR_BUFFER_INITIALIZER;
    bool explicitfdc = qemuDomainNeedsFDC(def);
    bool hasfloppy = false;
    unsigned int bootindex;
    char driveLetter;
    char *backendAlias = NULL;
    char *backendStr = NULL;
    char *bootindexStr = NULL;
    size_t i;
    int ret = -1;

    virBufferAddLit(&fdc_opts, "isa-fdc,");

    for (i = 0; i < def->ndisks; i++) {
        virDomainDiskDefPtr disk = def->disks[i];

        if (disk->bus != VIR_DOMAIN_DISK_BUS_FDC)
            continue;

        hasfloppy = true;

        if (disk->info.bootIndex) {
            bootindex = disk->info.bootIndex;
        } else {
            bootindex = bootFloppy;
            bootFloppy = 0;
        }

        if (disk->info.addr.drive.unit)
            driveLetter = 'B';
        else
            driveLetter = 'A';

        if (bootindex &&
            virAsprintf(&bootindexStr, "bootindex%c=%u", driveLetter, bootindex) < 0)
            goto cleanup;

        /* with -blockdev we setup the floppy device and it's backend with -device */
        if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_BLOCKDEV)) {
            if (qemuDomainDiskGetBackendAlias(disk, qemuCaps, &backendAlias) < 0)
                goto cleanup;

            if (backendAlias &&
                virAsprintf(&backendStr, "drive%c=%s", driveLetter, backendAlias) < 0)
                goto cleanup;
        }

        if (!explicitfdc) {
            if (backendStr) {
                virCommandAddArg(cmd, "-global");
                virCommandAddArgFormat(cmd, "isa-fdc.%s", backendStr);
            }

            if (bootindexStr) {
                virCommandAddArg(cmd, "-global");
                virCommandAddArgFormat(cmd, "isa-fdc.%s", bootindexStr);
            }
        } else {
            virBufferStrcat(&fdc_opts, backendStr, ",", NULL);
            virBufferStrcat(&fdc_opts, bootindexStr, ",", NULL);
        }

        VIR_FREE(backendAlias);
        VIR_FREE(backendStr);
        VIR_FREE(bootindexStr);
    }

    if (explicitfdc && hasfloppy) {
        /* Newer Q35 machine types require an explicit FDC controller */
        virBufferTrim(&fdc_opts, ",", -1);
        virCommandAddArg(cmd, "-device");
        virCommandAddArgBuffer(cmd, &fdc_opts);
    }

    ret = 0;

 cleanup:
    VIR_FREE(backendAlias);
    VIR_FREE(backendStr);
    VIR_FREE(bootindexStr);
    virBufferFreeAndReset(&fdc_opts);
    return ret;
}


static int
qemuBuildObjectCommandline(virCommandPtr cmd,
                           virJSONValuePtr objProps)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;

    if (!objProps)
        return 0;

    if (virQEMUBuildObjectCommandlineFromJSON(&buf, objProps) < 0) {
        virBufferFreeAndReset(&buf);
        return -1;
    }

    virCommandAddArg(cmd, "-object");
    virCommandAddArgBuffer(cmd, &buf);

    return 0;
}


static int
qemuBuildBlockStorageSourceAttachDataCommandline(virCommandPtr cmd,
                                                 qemuBlockStorageSourceAttachDataPtr data)
{
    char *tmp;

    if (qemuBuildObjectCommandline(cmd, data->prmgrProps) < 0 ||
        qemuBuildObjectCommandline(cmd, data->authsecretProps) < 0 ||
        qemuBuildObjectCommandline(cmd, data->encryptsecretProps) < 0 ||
        qemuBuildObjectCommandline(cmd, data->tlsProps) < 0)
        return -1;

    if (data->driveCmd)
        virCommandAddArgList(cmd, "-drive", data->driveCmd, NULL);

    if (data->storageProps) {
        if (!(tmp = virJSONValueToString(data->storageProps, false)))
            return -1;

        virCommandAddArgList(cmd, "-blockdev", tmp, NULL);
        VIR_FREE(tmp);
    }

    if (data->formatProps) {
        if (!(tmp = virJSONValueToString(data->formatProps, false)))
            return -1;

        virCommandAddArgList(cmd, "-blockdev", tmp, NULL);
        VIR_FREE(tmp);
    }

    return 0;
}


static int
qemuBuildDiskSourceCommandLine(virCommandPtr cmd,
                               virDomainDiskDefPtr disk,
                               virQEMUCapsPtr qemuCaps)
{
    VIR_AUTOPTR(qemuBlockStorageSourceChainData) data = NULL;
    VIR_AUTOPTR(virJSONValue) copyOnReadProps = NULL;
    VIR_AUTOFREE(char *) copyOnReadPropsStr = NULL;
    size_t i;

    if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_BLOCKDEV)) {
        if (virStorageSourceIsEmpty(disk->src))
            return 0;

        if (!(data = qemuBuildStorageSourceChainAttachPrepareBlockdev(disk->src,
                                                                      qemuCaps)))
            return -1;

        if (disk->copy_on_read == VIR_TRISTATE_SWITCH_ON &&
            !(copyOnReadProps = qemuBlockStorageGetCopyOnReadProps(disk)))
            return -1;
    } else {
        if (!(data = qemuBuildStorageSourceChainAttachPrepareDrive(disk, qemuCaps)))
            return -1;
    }

    for (i = data->nsrcdata; i > 0; i--) {
        if (qemuBuildBlockStorageSourceAttachDataCommandline(cmd,
                                                             data->srcdata[i - 1]) < 0)
            return -1;
    }

    if (copyOnReadProps) {
        if (!(copyOnReadPropsStr = virJSONValueToString(copyOnReadProps, false)))
            return -1;

        virCommandAddArgList(cmd, "-blockdev", copyOnReadPropsStr, NULL);
    }

    return 0;
}


static int
qemuBuildDiskCommandLine(virCommandPtr cmd,
                         const virDomainDef *def,
                         virDomainDiskDefPtr disk,
                         virQEMUCapsPtr qemuCaps,
                         unsigned int bootindex)
{
    char *optstr;

    if (qemuBuildDiskSourceCommandLine(cmd, disk, qemuCaps) < 0)
        return -1;

    if (!qemuDiskBusNeedsDriveArg(disk->bus)) {
        if (disk->bus != VIR_DOMAIN_DISK_BUS_FDC ||
            virQEMUCapsGet(qemuCaps, QEMU_CAPS_BLOCKDEV)) {
            if (qemuCommandAddExtDevice(cmd, &disk->info) < 0)
                return -1;

            virCommandAddArg(cmd, "-device");

            if (!(optstr = qemuBuildDiskDeviceStr(def, disk, bootindex,
                                                  qemuCaps)))
                return -1;
            virCommandAddArg(cmd, optstr);
            VIR_FREE(optstr);
        }
    }

    return 0;
}


static int
qemuBuildDisksCommandLine(virCommandPtr cmd,
                          const virDomainDef *def,
                          virQEMUCapsPtr qemuCaps)
{
    size_t i;
    unsigned int bootCD = 0;
    unsigned int bootFloppy = 0;
    unsigned int bootDisk = 0;
    bool blockdev = virQEMUCapsGet(qemuCaps, QEMU_CAPS_BLOCKDEV);

    for (i = 0; i < def->os.nBootDevs; i++) {
        switch (def->os.bootDevs[i]) {
        case VIR_DOMAIN_BOOT_CDROM:
            bootCD = i + 1;
            break;
        case VIR_DOMAIN_BOOT_FLOPPY:
            bootFloppy = i + 1;
            break;
        case VIR_DOMAIN_BOOT_DISK:
            bootDisk = i + 1;
            break;
        }
    }

    /* If we want to express the floppy drives via -device, the controller needs
     * to be instantiated prior to that */
    if (blockdev &&
        qemuBuildFloppyCommandLineControllerOptions(cmd, def, qemuCaps, bootFloppy) < 0)
        return -1;

    for (i = 0; i < def->ndisks; i++) {
        virDomainDiskDefPtr disk = def->disks[i];
        unsigned int bootindex = 0;

        if (disk->info.bootIndex) {
            bootindex = disk->info.bootIndex;
        } else {
            switch (disk->device) {
            case VIR_DOMAIN_DISK_DEVICE_CDROM:
                bootindex = bootCD;
                bootCD = 0;
                break;
            case VIR_DOMAIN_DISK_DEVICE_DISK:
            case VIR_DOMAIN_DISK_DEVICE_LUN:
                bootindex = bootDisk;
                bootDisk = 0;
                break;
            }
        }

        /* The floppy device itself does not support the bootindex property
         * so we need to set it up for the controller */
        if (disk->device == VIR_DOMAIN_DISK_DEVICE_FLOPPY)
            bootindex = 0;

        if (qemuBuildDiskCommandLine(cmd, def, disk, qemuCaps, bootindex) < 0)
            return -1;
    }

    if (!blockdev &&
        qemuBuildFloppyCommandLineControllerOptions(cmd, def, qemuCaps, bootFloppy) < 0)
        return -1;

    return 0;
}


static char *
qemuBuildFSStr(virDomainFSDefPtr fs)
{
    virBuffer opt = VIR_BUFFER_INITIALIZER;
    const char *driver = qemuDomainFSDriverTypeToString(fs->fsdriver);
    const char *wrpolicy = virDomainFSWrpolicyTypeToString(fs->wrpolicy);

    if (fs->type != VIR_DOMAIN_FS_TYPE_MOUNT) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("only supports mount filesystem type"));
        goto error;
    }

    if (!driver) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Filesystem driver type not supported"));
        goto error;
    }
    virBufferAdd(&opt, driver, -1);

    if (fs->fsdriver == VIR_DOMAIN_FS_DRIVER_TYPE_PATH ||
        fs->fsdriver == VIR_DOMAIN_FS_DRIVER_TYPE_DEFAULT) {
        if (fs->accessmode == VIR_DOMAIN_FS_ACCESSMODE_MAPPED) {
            virBufferAddLit(&opt, ",security_model=mapped");
        } else if (fs->accessmode == VIR_DOMAIN_FS_ACCESSMODE_PASSTHROUGH) {
            virBufferAddLit(&opt, ",security_model=passthrough");
        } else if (fs->accessmode == VIR_DOMAIN_FS_ACCESSMODE_SQUASH) {
            virBufferAddLit(&opt, ",security_model=none");
        }
    } else {
        /* For other fs drivers, default(passthru) should always
         * be supported */
        if (fs->accessmode != VIR_DOMAIN_FS_ACCESSMODE_PASSTHROUGH) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("only supports passthrough accessmode"));
            goto error;
        }
    }

    if (fs->wrpolicy)
        virBufferAsprintf(&opt, ",writeout=%s", wrpolicy);

    virBufferAsprintf(&opt, ",id=%s%s", QEMU_FSDEV_HOST_PREFIX, fs->info.alias);
    virBufferAddLit(&opt, ",path=");
    virQEMUBuildBufferEscapeComma(&opt, fs->src->path);

    if (fs->readonly)
        virBufferAddLit(&opt, ",readonly");

    if (virBufferCheckError(&opt) < 0)
        goto error;

    return virBufferContentAndReset(&opt);

 error:
    virBufferFreeAndReset(&opt);
    return NULL;
}


static char *
qemuBuildFSDevStr(const virDomainDef *def,
                  virDomainFSDefPtr fs,
                  virQEMUCapsPtr qemuCaps)
{
    virBuffer opt = VIR_BUFFER_INITIALIZER;

    if (fs->type != VIR_DOMAIN_FS_TYPE_MOUNT) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("can only passthrough directories"));
        goto error;
    }

    if (qemuBuildVirtioDevStr(&opt, "virtio-9p", qemuCaps,
                              VIR_DOMAIN_DEVICE_FS, fs) < 0) {
        goto error;
    }

    virBufferAsprintf(&opt, ",id=%s", fs->info.alias);
    virBufferAsprintf(&opt, ",fsdev=%s%s",
                      QEMU_FSDEV_HOST_PREFIX, fs->info.alias);
    virBufferAddLit(&opt, ",mount_tag=");
    virQEMUBuildBufferEscapeComma(&opt, fs->dst);

    if (qemuBuildVirtioOptionsStr(&opt, fs->virtio, qemuCaps) < 0)
        goto error;

    if (qemuBuildDeviceAddressStr(&opt, def, &fs->info, qemuCaps) < 0)
        goto error;

    if (virBufferCheckError(&opt) < 0)
        goto error;

    return virBufferContentAndReset(&opt);

 error:
    virBufferFreeAndReset(&opt);
    return NULL;
}


static int
qemuBuildFSDevCommandLine(virCommandPtr cmd,
                          const virDomainDef *def,
                          virQEMUCapsPtr qemuCaps)
{
    size_t i;

    for (i = 0; i < def->nfss; i++) {
        char *optstr;
        virDomainFSDefPtr fs = def->fss[i];

        virCommandAddArg(cmd, "-fsdev");
        if (!(optstr = qemuBuildFSStr(fs)))
            return -1;
        virCommandAddArg(cmd, optstr);
        VIR_FREE(optstr);

        if (qemuCommandAddExtDevice(cmd, &fs->info) < 0)
            return -1;

        virCommandAddArg(cmd, "-device");
        if (!(optstr = qemuBuildFSDevStr(def, fs, qemuCaps)))
            return -1;
        virCommandAddArg(cmd, optstr);
        VIR_FREE(optstr);
    }

    return 0;
}


static int
qemuControllerModelUSBToCaps(int model)
{
    switch (model) {
    case VIR_DOMAIN_CONTROLLER_MODEL_USB_PIIX3_UHCI:
        return QEMU_CAPS_PIIX3_USB_UHCI;
    case VIR_DOMAIN_CONTROLLER_MODEL_USB_PIIX4_UHCI:
        return QEMU_CAPS_PIIX4_USB_UHCI;
    case VIR_DOMAIN_CONTROLLER_MODEL_USB_EHCI:
        return QEMU_CAPS_USB_EHCI;
    case VIR_DOMAIN_CONTROLLER_MODEL_USB_ICH9_EHCI1:
    case VIR_DOMAIN_CONTROLLER_MODEL_USB_ICH9_UHCI1:
    case VIR_DOMAIN_CONTROLLER_MODEL_USB_ICH9_UHCI2:
    case VIR_DOMAIN_CONTROLLER_MODEL_USB_ICH9_UHCI3:
        return QEMU_CAPS_ICH9_USB_EHCI1;
    case VIR_DOMAIN_CONTROLLER_MODEL_USB_VT82C686B_UHCI:
        return QEMU_CAPS_VT82C686B_USB_UHCI;
    case VIR_DOMAIN_CONTROLLER_MODEL_USB_PCI_OHCI:
        return QEMU_CAPS_PCI_OHCI;
    case VIR_DOMAIN_CONTROLLER_MODEL_USB_NEC_XHCI:
        return QEMU_CAPS_NEC_USB_XHCI;
    case VIR_DOMAIN_CONTROLLER_MODEL_USB_QEMU_XHCI:
        return QEMU_CAPS_DEVICE_QEMU_XHCI;
    default:
        return -1;
    }
}


static const char *
qemuBuildUSBControllerFindMasterAlias(const virDomainDef *domainDef,
                                      const virDomainControllerDef *def)
{
    size_t i;

    for (i = 0; i < domainDef->ncontrollers; i++) {
        const virDomainControllerDef *tmp = domainDef->controllers[i];

        if (tmp->type != VIR_DOMAIN_CONTROLLER_TYPE_USB)
            continue;

        if (tmp->idx != def->idx)
            continue;

        if (tmp->info.mastertype == VIR_DOMAIN_CONTROLLER_MASTER_USB)
            continue;

        return tmp->info.alias;
    }

    return NULL;
}


static int
qemuBuildUSBControllerDevStr(const virDomainDef *domainDef,
                             virDomainControllerDefPtr def,
                             virQEMUCapsPtr qemuCaps,
                             virBuffer *buf)
{
    const char *smodel;
    int model, flags;

    model = def->model;

    if (model == VIR_DOMAIN_CONTROLLER_MODEL_USB_DEFAULT) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       "%s", _("no model provided for USB controller"));
        return -1;
    }

    smodel = qemuControllerModelUSBTypeToString(model);
    flags = qemuControllerModelUSBToCaps(model);

    if (flags == -1 || !virQEMUCapsGet(qemuCaps, flags)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("%s not supported in this QEMU binary"), smodel);
        return -1;
    }

    virBufferAsprintf(buf, "%s", smodel);

    if (def->opts.usbopts.ports != -1) {
        if ((model != VIR_DOMAIN_CONTROLLER_MODEL_USB_NEC_XHCI ||
             !virQEMUCapsGet(qemuCaps, QEMU_CAPS_NEC_USB_XHCI_PORTS)) &&
            model != VIR_DOMAIN_CONTROLLER_MODEL_USB_QEMU_XHCI) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("usb controller type %s doesn't support 'ports' "
                             "with this QEMU binary"), smodel);
            return -1;
        }

        virBufferAsprintf(buf, ",p2=%d,p3=%d",
                          def->opts.usbopts.ports, def->opts.usbopts.ports);
    }

    if (def->info.mastertype == VIR_DOMAIN_CONTROLLER_MASTER_USB) {
        const char *masterbus;

        if (!(masterbus = qemuBuildUSBControllerFindMasterAlias(domainDef, def))) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("masterbus not found"));
            return -1;
        }
        virBufferAsprintf(buf, ",masterbus=%s.0,firstport=%d",
                          masterbus, def->info.master.usb.startport);
    } else {
        virBufferAsprintf(buf, ",id=%s", def->info.alias);
    }

    return 0;
}


/**
 * qemuBuildControllerDevStr:
 * @domainDef: domain definition
 * @def: controller definition
 * @qemuCaps: QEMU binary capabilities
 * @devstr: device string
 * @nusbcontroller: number of USB controllers
 *
 * Turn @def into a description of the controller that QEMU will understand,
 * to be used either on the command line or through the monitor.
 *
 * The description will be returned in @devstr and can be NULL, eg. when
 * passing in one of the built-in controllers. The returned string must be
 * freed by the caller.
 *
 * The number pointed to by @nusbcontroller will be increased by one every
 * time the description for a USB controller has been generated successfully.
 *
 * Returns: 0 on success, <0 on failure
 */
int
qemuBuildControllerDevStr(const virDomainDef *domainDef,
                          virDomainControllerDefPtr def,
                          virQEMUCapsPtr qemuCaps,
                          char **devstr)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;

    *devstr = NULL;

    switch ((virDomainControllerType)def->type) {
    case VIR_DOMAIN_CONTROLLER_TYPE_SCSI:
        switch ((virDomainControllerModelSCSI) def->model) {
        case VIR_DOMAIN_CONTROLLER_MODEL_SCSI_VIRTIO_SCSI:
        case VIR_DOMAIN_CONTROLLER_MODEL_SCSI_VIRTIO_TRANSITIONAL:
        case VIR_DOMAIN_CONTROLLER_MODEL_SCSI_VIRTIO_NON_TRANSITIONAL:
            if (qemuBuildVirtioDevStr(&buf, "virtio-scsi", qemuCaps,
                                      VIR_DOMAIN_DEVICE_CONTROLLER, def) < 0) {
                goto error;
            }

            if (def->iothread) {
                virBufferAsprintf(&buf, ",iothread=iothread%u",
                                  def->iothread);
            }

            if (qemuBuildVirtioOptionsStr(&buf, def->virtio, qemuCaps) < 0)
                goto error;
            break;
        case VIR_DOMAIN_CONTROLLER_MODEL_SCSI_LSILOGIC:
            virBufferAddLit(&buf, "lsi");
            break;
        case VIR_DOMAIN_CONTROLLER_MODEL_SCSI_IBMVSCSI:
            virBufferAddLit(&buf, "spapr-vscsi");
            break;
        case VIR_DOMAIN_CONTROLLER_MODEL_SCSI_LSISAS1068:
            virBufferAddLit(&buf, "mptsas1068");
            break;
        case VIR_DOMAIN_CONTROLLER_MODEL_SCSI_LSISAS1078:
            virBufferAddLit(&buf, "megasas");
            break;
        case VIR_DOMAIN_CONTROLLER_MODEL_SCSI_AUTO:
        case VIR_DOMAIN_CONTROLLER_MODEL_SCSI_BUSLOGIC:
        case VIR_DOMAIN_CONTROLLER_MODEL_SCSI_VMPVSCSI:
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("Unsupported controller model: %s"),
                           virDomainControllerModelSCSITypeToString(def->model));
            goto error;
        case VIR_DOMAIN_CONTROLLER_MODEL_SCSI_DEFAULT:
        case VIR_DOMAIN_CONTROLLER_MODEL_SCSI_LAST:
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Unexpected SCSI controller model %d"),
                           def->model);
            goto error;
        }
        virBufferAsprintf(&buf, ",id=%s", def->info.alias);
        break;

    case VIR_DOMAIN_CONTROLLER_TYPE_VIRTIO_SERIAL:
        if (qemuBuildVirtioDevStr(&buf, "virtio-serial", qemuCaps,
                                  VIR_DOMAIN_DEVICE_CONTROLLER, def) < 0) {
            goto error;
        }

        virBufferAsprintf(&buf, ",id=%s", def->info.alias);
        if (def->opts.vioserial.ports != -1) {
            virBufferAsprintf(&buf, ",max_ports=%d",
                              def->opts.vioserial.ports);
        }
        if (def->opts.vioserial.vectors != -1) {
            virBufferAsprintf(&buf, ",vectors=%d",
                              def->opts.vioserial.vectors);
        }
        if (qemuBuildVirtioOptionsStr(&buf, def->virtio, qemuCaps) < 0)
            goto error;
        break;

    case VIR_DOMAIN_CONTROLLER_TYPE_CCID:
        virBufferAsprintf(&buf, "usb-ccid,id=%s", def->info.alias);
        break;

    case VIR_DOMAIN_CONTROLLER_TYPE_SATA:
        virBufferAsprintf(&buf, "ahci,id=%s", def->info.alias);
        break;

    case VIR_DOMAIN_CONTROLLER_TYPE_USB:
        if (qemuBuildUSBControllerDevStr(domainDef, def, qemuCaps, &buf) == -1)
            goto error;

        break;

    case VIR_DOMAIN_CONTROLLER_TYPE_PCI: {
        const virDomainPCIControllerOpts *pciopts = &def->opts.pciopts;
        const char *modelName = virDomainControllerPCIModelNameTypeToString(pciopts->modelName);

        /* Skip the implicit PHB for pSeries guests */
        if (def->model == VIR_DOMAIN_CONTROLLER_MODEL_PCI_ROOT &&
            pciopts->modelName == VIR_DOMAIN_CONTROLLER_PCI_MODEL_NAME_SPAPR_PCI_HOST_BRIDGE &&
            pciopts->targetIndex == 0) {
            goto done;
        }

        if (!modelName) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Unknown virDomainControllerPCIModelName value: %d"),
                           pciopts->modelName);
            return -1;
        }

        switch ((virDomainControllerModelPCI) def->model) {
        case VIR_DOMAIN_CONTROLLER_MODEL_PCI_BRIDGE:
            virBufferAsprintf(&buf, "%s,chassis_nr=%d,id=%s",
                              modelName, pciopts->chassisNr,
                              def->info.alias);
            break;
        case VIR_DOMAIN_CONTROLLER_MODEL_PCI_EXPANDER_BUS:
        case VIR_DOMAIN_CONTROLLER_MODEL_PCIE_EXPANDER_BUS:
            virBufferAsprintf(&buf, "%s,bus_nr=%d,id=%s",
                              modelName, pciopts->busNr,
                              def->info.alias);
            if (pciopts->numaNode != -1) {
                virBufferAsprintf(&buf, ",numa_node=%d",
                                  pciopts->numaNode);
            }
            break;
        case VIR_DOMAIN_CONTROLLER_MODEL_DMI_TO_PCI_BRIDGE:
        case VIR_DOMAIN_CONTROLLER_MODEL_PCIE_SWITCH_UPSTREAM_PORT:
        case VIR_DOMAIN_CONTROLLER_MODEL_PCIE_TO_PCI_BRIDGE:
            virBufferAsprintf(&buf, "%s,id=%s", modelName, def->info.alias);
            break;
        case VIR_DOMAIN_CONTROLLER_MODEL_PCIE_ROOT_PORT:
        case VIR_DOMAIN_CONTROLLER_MODEL_PCIE_SWITCH_DOWNSTREAM_PORT:
            virBufferAsprintf(&buf, "%s,port=0x%x,chassis=%d,id=%s",
                              modelName, pciopts->port,
                              pciopts->chassis, def->info.alias);
            break;
        case VIR_DOMAIN_CONTROLLER_MODEL_PCI_ROOT:
            virBufferAsprintf(&buf, "%s,index=%d,id=%s",
                              modelName, pciopts->targetIndex,
                              def->info.alias);

            if (pciopts->numaNode != -1)
                virBufferAsprintf(&buf, ",numa_node=%d", pciopts->numaNode);
            break;
        case VIR_DOMAIN_CONTROLLER_MODEL_PCIE_ROOT:
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("Unsupported PCI Express root controller"));
            goto error;
        case VIR_DOMAIN_CONTROLLER_MODEL_PCI_DEFAULT:
        case VIR_DOMAIN_CONTROLLER_MODEL_PCI_LAST:
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Unexpected PCI controller model %d"),
                           def->model);
            goto error;
        }
        break;
    }

    case VIR_DOMAIN_CONTROLLER_TYPE_IDE:
    case VIR_DOMAIN_CONTROLLER_TYPE_FDC:
    case VIR_DOMAIN_CONTROLLER_TYPE_XENBUS:
    case VIR_DOMAIN_CONTROLLER_TYPE_LAST:
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Unsupported controller type: %s"),
                       virDomainControllerTypeToString(def->type));
        goto error;
    }

    if (def->queues)
        virBufferAsprintf(&buf, ",num_queues=%u", def->queues);

    if (def->cmd_per_lun)
        virBufferAsprintf(&buf, ",cmd_per_lun=%u", def->cmd_per_lun);

    if (def->max_sectors)
        virBufferAsprintf(&buf, ",max_sectors=%u", def->max_sectors);

    qemuBuildIoEventFdStr(&buf, def->ioeventfd, qemuCaps);

    if (qemuBuildDeviceAddressStr(&buf, domainDef, &def->info, qemuCaps) < 0)
        goto error;

 done:
    if (virBufferCheckError(&buf) < 0)
        goto error;

    *devstr = virBufferContentAndReset(&buf);
    return 0;

 error:
    virBufferFreeAndReset(&buf);
    return -1;
}


static bool
qemuBuildDomainForbidLegacyUSBController(const virDomainDef *def)
{
    if (qemuDomainIsQ35(def) ||
        qemuDomainIsARMVirt(def) ||
        qemuDomainIsRISCVVirt(def))
        return true;

    return false;
}


static int
qemuBuildLegacyUSBControllerCommandLine(virCommandPtr cmd,
                                        const virDomainDef *def)
{
    size_t i;
    size_t nlegacy = 0;
    size_t nusb = 0;

    for (i = 0; i < def->ncontrollers; i++) {
        virDomainControllerDefPtr cont = def->controllers[i];

        if (cont->type != VIR_DOMAIN_CONTROLLER_TYPE_USB)
            continue;

        /* If we have mode='none', there are no other USB controllers */
        if (cont->model == VIR_DOMAIN_CONTROLLER_MODEL_USB_NONE)
            return 0;

        if (cont->model == VIR_DOMAIN_CONTROLLER_MODEL_USB_DEFAULT)
            nlegacy++;
        else
            nusb++;
    }

    if (nlegacy > 1) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Multiple legacy USB controllers are "
                         "not supported"));
        return -1;
    }

    if (nusb == 0 &&
        !qemuBuildDomainForbidLegacyUSBController(def) &&
        !ARCH_IS_S390(def->os.arch)) {
        /* We haven't added any USB controller yet, but we haven't been asked
         * not to add one either. Add a legacy USB controller, unless we're
         * creating a kind of guest we want to keep legacy-free */
        virCommandAddArg(cmd, "-usb");
    }

    return 0;
}


/**
 * qemuBuildSkipController:
 * @controller: Controller to check
 * @def: Domain definition
 *
 * Returns true if this controller can be skipped for command line
 * generation or device validation.
 */
static bool
qemuBuildSkipController(const virDomainControllerDef *controller,
                        const virDomainDef *def)
{
    /* skip pcie-root */
    if (controller->type == VIR_DOMAIN_CONTROLLER_TYPE_PCI &&
        controller->model == VIR_DOMAIN_CONTROLLER_MODEL_PCIE_ROOT)
        return true;

    /* Skip pci-root, except for pSeries guests (which actually
     * support more than one PCI Host Bridge per guest) */
    if (!qemuDomainIsPSeries(def) &&
        controller->type == VIR_DOMAIN_CONTROLLER_TYPE_PCI &&
        controller->model == VIR_DOMAIN_CONTROLLER_MODEL_PCI_ROOT)
        return true;

    /* first SATA controller on Q35 machines is implicit */
    if (controller->type == VIR_DOMAIN_CONTROLLER_TYPE_SATA &&
        controller->idx == 0 && qemuDomainIsQ35(def))
        return true;

    /* first IDE controller is implicit on various machines */
    if (controller->type == VIR_DOMAIN_CONTROLLER_TYPE_IDE &&
        controller->idx == 0 && qemuDomainHasBuiltinIDE(def))
        return true;

    return false;
}


static int
qemuBuildControllersByTypeCommandLine(virCommandPtr cmd,
                                      const virDomainDef *def,
                                      virQEMUCapsPtr qemuCaps,
                                      virDomainControllerType type)
{
    char *devstr = NULL;
    int ret = -1;
    size_t i;

    for (i = 0; i < def->ncontrollers; i++) {
        virDomainControllerDefPtr cont = def->controllers[i];

        if (cont->type != type)
            continue;

        if (qemuBuildSkipController(cont, def))
            continue;

        /* skip USB controllers with type none.*/
        if (cont->type == VIR_DOMAIN_CONTROLLER_TYPE_USB &&
            cont->model == VIR_DOMAIN_CONTROLLER_MODEL_USB_NONE) {
            continue;
        }

        if (cont->type == VIR_DOMAIN_CONTROLLER_TYPE_USB &&
            cont->model == VIR_DOMAIN_CONTROLLER_MODEL_USB_DEFAULT &&
            !qemuBuildDomainForbidLegacyUSBController(def)) {

            /* An appropriate default USB controller model should already
             * have been selected in qemuDomainDeviceDefPostParse(); if
             * we still have no model by now, we have to fall back to the
             * legacy USB controller.
             *
             * Note that we *don't* want to end up with the legacy USB
             * controller for q35 and virt machines, so we go ahead and
             * fail in qemuBuildControllerDevStr(); on the other hand,
             * for s390 machines we want to ignore any USB controller
             * (see 548ba43028 for the full story), so we skip
             * qemuBuildControllerDevStr() but we don't ultimately end
             * up adding the legacy USB controller */
            continue;
        }

        VIR_FREE(devstr);
        if (qemuBuildControllerDevStr(def, cont, qemuCaps, &devstr) < 0)
            goto cleanup;

        if (devstr) {
            if (qemuCommandAddExtDevice(cmd, &cont->info) < 0)
                goto cleanup;

            virCommandAddArg(cmd, "-device");
            virCommandAddArg(cmd, devstr);
        }
    }

    ret = 0;
 cleanup:
    VIR_FREE(devstr);
    return ret;
}


static int
qemuBuildControllersCommandLine(virCommandPtr cmd,
                                const virDomainDef *def,
                                virQEMUCapsPtr qemuCaps)
{
    size_t i;
    int contOrder[] = {
        /*
         * List of controller types that we add commandline args for,
         * *in the order we want to add them*.
         *
         * The floppy controller is implicit on PIIX4 and older Q35
         * machines. For newer Q35 machines it is added out of the
         * controllers loop, after the floppy drives.
         *
         * We don't add PCI/PCIe root controller either, because it's
         * implicit, but we do add PCI bridges and other PCI
         * controllers, so we leave that in to check each
         * one. Likewise, we don't do anything for the primary IDE
         * controller on an i440fx machine or primary SATA on q35, but
         * we do add those beyond these two exceptions.
         *
         * CCID controllers are formatted separately after USB hubs,
         * because they go on the USB bus.
         */
        VIR_DOMAIN_CONTROLLER_TYPE_PCI,
        VIR_DOMAIN_CONTROLLER_TYPE_USB,
        VIR_DOMAIN_CONTROLLER_TYPE_SCSI,
        VIR_DOMAIN_CONTROLLER_TYPE_IDE,
        VIR_DOMAIN_CONTROLLER_TYPE_SATA,
        VIR_DOMAIN_CONTROLLER_TYPE_VIRTIO_SERIAL,
    };
    int ret = -1;

    for (i = 0; i < ARRAY_CARDINALITY(contOrder); i++) {
        if (qemuBuildControllersByTypeCommandLine(cmd, def, qemuCaps, contOrder[i]) < 0)
            goto cleanup;
    }

    if (qemuBuildLegacyUSBControllerCommandLine(cmd, def) < 0)
        goto cleanup;

    ret = 0;

 cleanup:
    return ret;
}


static int
qemuBuildMemoryBackendPropsShare(virJSONValuePtr props,
                                 virDomainMemoryAccess memAccess)
{
    switch (memAccess) {
    case VIR_DOMAIN_MEMORY_ACCESS_SHARED:
        return virJSONValueObjectAdd(props, "b:share", true, NULL);

    case VIR_DOMAIN_MEMORY_ACCESS_PRIVATE:
        return virJSONValueObjectAdd(props, "b:share", false, NULL);

    case VIR_DOMAIN_MEMORY_ACCESS_DEFAULT:
    case VIR_DOMAIN_MEMORY_ACCESS_LAST:
        break;
    }

    return 0;
}


static int
qemuBuildMemoryGetDefaultPagesize(virQEMUDriverConfigPtr cfg,
                                  unsigned long long *pagesize)
{
    virHugeTLBFSPtr p;

    if (!cfg->nhugetlbfs) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("hugetlbfs filesystem is not mounted "
                               "or disabled by administrator config"));
        return -1;
    }

    if (!(p = virFileGetDefaultHugepage(cfg->hugetlbfs, cfg->nhugetlbfs)))
        p = &cfg->hugetlbfs[0];

    *pagesize = p->size;
    return 0;
}


/**
 * qemuBuildMemoryBackendProps:
 * @backendProps: [out] constructed object
 * @alias: alias of the device
 * @cfg: qemu driver config object
 * @priv: pointer to domain private object
 * @def: domain definition object
 * @mem: memory definition object
 * @force: forcibly use one of the backends
 *
 * Creates a configuration object that represents memory backend of given guest
 * NUMA node (domain @def and @mem). Use @priv->autoNodeset to fine tune the
 * placement of the memory on the host NUMA nodes.
 *
 * By default, if no memory-backend-* object is necessary to fulfil the guest
 * configuration value of 1 is returned. This behaviour can be suppressed by
 * setting @force to true in which case 0 would be returned.
 *
 * Then, if one of the three memory-backend-* should be used, the @priv->qemuCaps
 * is consulted to check if qemu does support it.
 *
 * Returns: 0 on success,
 *          1 on success and if there's no need to use memory-backend-*
 *         -1 on error.
 */
int
qemuBuildMemoryBackendProps(virJSONValuePtr *backendProps,
                            const char *alias,
                            virQEMUDriverConfigPtr cfg,
                            qemuDomainObjPrivatePtr priv,
                            virDomainDefPtr def,
                            virDomainMemoryDefPtr mem,
                            bool force)
{
    const char *backendType = "memory-backend-file";
    virDomainNumatuneMemMode mode;
    const long system_page_size = virGetSystemPageSizeKB();
    virDomainMemoryAccess memAccess = mem->access;
    size_t i;
    char *memPath = NULL;
    bool prealloc = false;
    virBitmapPtr nodemask = NULL;
    int ret = -1;
    virJSONValuePtr props = NULL;
    bool nodeSpecified = virDomainNumatuneNodeSpecified(def->numa, mem->targetNode);
    unsigned long long pagesize = mem->pagesize;
    bool needHugepage = !!pagesize;
    bool useHugepage = !!pagesize;
    int discard = mem->discard;

    /* The difference between @needHugepage and @useHugepage is that the latter
     * is true whenever huge page is defined for the current memory cell.
     * Either directly, or transitively via global domain huge pages. The
     * former is true whenever "memory-backend-file" must be used to satisfy
     * @useHugepage. */

    *backendProps = NULL;

    if (mem->targetNode >= 0) {
        /* memory devices could provide a invalid guest node */
        if (mem->targetNode >= virDomainNumaGetNodeCount(def->numa)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("can't add memory backend for guest node '%d' as "
                             "the guest has only '%zu' NUMA nodes configured"),
                           mem->targetNode, virDomainNumaGetNodeCount(def->numa));
            return -1;
        }

        if (memAccess == VIR_DOMAIN_MEMORY_ACCESS_DEFAULT)
            memAccess = virDomainNumaGetNodeMemoryAccessMode(def->numa, mem->targetNode);

        if (discard == VIR_TRISTATE_BOOL_ABSENT)
            discard = virDomainNumaGetNodeDiscard(def->numa, mem->targetNode);
    }

    if (memAccess == VIR_DOMAIN_MEMORY_ACCESS_DEFAULT)
        memAccess = def->mem.access;

    if (discard == VIR_TRISTATE_BOOL_ABSENT)
        discard = def->mem.discard;

    if (virDomainNumatuneGetMode(def->numa, mem->targetNode, &mode) < 0 &&
        virDomainNumatuneGetMode(def->numa, -1, &mode) < 0)
        mode = VIR_DOMAIN_NUMATUNE_MEM_STRICT;

    if (pagesize == 0) {
        virDomainHugePagePtr master_hugepage = NULL;
        virDomainHugePagePtr hugepage = NULL;
        bool thisHugepage = false;

        /* Find the huge page size we want to use */
        for (i = 0; i < def->mem.nhugepages; i++) {
            hugepage = &def->mem.hugepages[i];

            if (!hugepage->nodemask) {
                master_hugepage = hugepage;
                continue;
            }

            /* just find the master hugepage in case we don't use NUMA */
            if (mem->targetNode < 0)
                continue;

            if (virBitmapGetBit(hugepage->nodemask, mem->targetNode,
                                &thisHugepage) < 0) {
                /* Ignore this error. It's not an error after all. Well,
                 * the nodemask for this <page/> can contain lower NUMA
                 * nodes than we are querying in here. */
                continue;
            }

            if (thisHugepage) {
                /* Hooray, we've found the page size */
                needHugepage = true;
                break;
            }
        }

        if (i == def->mem.nhugepages) {
            /* We have not found specific huge page to be used with this
             * NUMA node. Use the generic setting then (<page/> without any
             * @nodemask) if possible. */
            hugepage = master_hugepage;
        }

        if (hugepage) {
            pagesize = hugepage->size;
            useHugepage = true;
        }
    }

    if (pagesize == system_page_size) {
        /* However, if user specified to use "huge" page
         * of regular system page size, it's as if they
         * hasn't specified any huge pages at all. */
        pagesize = 0;
        needHugepage = false;
        useHugepage = false;
    } else if (useHugepage && pagesize == 0) {
        if (qemuBuildMemoryGetDefaultPagesize(cfg, &pagesize) < 0)
            goto cleanup;
    }

    if (!(props = virJSONValueNewObject()))
        return -1;

    if (def->mem.source == VIR_DOMAIN_MEMORY_SOURCE_MEMFD) {
        backendType = "memory-backend-memfd";

        if (useHugepage &&
            (virJSONValueObjectAdd(props, "b:hugetlb", useHugepage, NULL) < 0 ||
             virJSONValueObjectAdd(props, "U:hugetlbsize", pagesize << 10, NULL) < 0)) {
            goto cleanup;
        }

        if (qemuBuildMemoryBackendPropsShare(props, memAccess) < 0)
            goto cleanup;

    } else if (useHugepage || mem->nvdimmPath || memAccess ||
        def->mem.source == VIR_DOMAIN_MEMORY_SOURCE_FILE) {

        if (mem->nvdimmPath) {
            if (VIR_STRDUP(memPath, mem->nvdimmPath) < 0)
                goto cleanup;
            if (!priv->memPrealloc)
                prealloc = true;
        } else if (useHugepage) {
            if (qemuGetDomainHupageMemPath(def, cfg, pagesize, &memPath) < 0)
                goto cleanup;
            if (!priv->memPrealloc)
                prealloc = true;
        } else {
            /* We can have both pagesize and mem source. If that's the case,
             * prefer hugepages as those are more specific. */
            if (qemuGetMemoryBackingPath(def, cfg, mem->info.alias, &memPath) < 0)
                goto cleanup;
        }

        if (virJSONValueObjectAdd(props,
                                  "B:prealloc", prealloc,
                                  "s:mem-path", memPath,
                                  NULL) < 0)
            goto cleanup;

        if (!mem->nvdimmPath &&
            discard == VIR_TRISTATE_BOOL_YES) {
            if (!virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_OBJECT_MEMORY_FILE_DISCARD)) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("this QEMU doesn't support memory discard"));
                goto cleanup;
            }

            if (virJSONValueObjectAdd(props,
                                      "B:discard-data", true,
                                      NULL) < 0)
                goto cleanup;
        }

        if (qemuBuildMemoryBackendPropsShare(props, memAccess) < 0)
            goto cleanup;
    } else {
        backendType = "memory-backend-ram";
    }

    if (virJSONValueObjectAdd(props, "U:size", mem->size * 1024, NULL) < 0)
        goto cleanup;

    if (mem->alignsize) {
        if (!virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_OBJECT_MEMORY_FILE_ALIGN)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("nvdimm align property is not available "
                             "with this QEMU binary"));
            goto cleanup;
        }
        if (virJSONValueObjectAdd(props, "U:align", mem->alignsize * 1024, NULL) < 0)
            goto cleanup;
    }

    if (mem->nvdimmPmem) {
        if (!virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_OBJECT_MEMORY_FILE_PMEM)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("nvdimm pmem property is not available "
                             "with this QEMU binary"));
            goto cleanup;
        }
        if (virJSONValueObjectAdd(props, "s:pmem", "on", NULL) < 0)
            goto cleanup;
    }

    if (mem->sourceNodes) {
        nodemask = mem->sourceNodes;
    } else {
        if (virDomainNumatuneMaybeGetNodeset(def->numa, priv->autoNodeset,
                                             &nodemask, mem->targetNode) < 0)
            goto cleanup;
    }

    if (nodemask) {
        if (!virNumaNodesetIsAvailable(nodemask))
            goto cleanup;
        if (virJSONValueObjectAdd(props,
                                  "m:host-nodes", nodemask,
                                  "S:policy", qemuNumaPolicyTypeToString(mode),
                                  NULL) < 0)
            goto cleanup;
    }

    /* If none of the following is requested... */
    if (!needHugepage && !mem->sourceNodes && !nodeSpecified &&
        !mem->nvdimmPath &&
        memAccess == VIR_DOMAIN_MEMORY_ACCESS_DEFAULT &&
        def->mem.source != VIR_DOMAIN_MEMORY_SOURCE_FILE &&
        def->mem.source != VIR_DOMAIN_MEMORY_SOURCE_MEMFD &&
        !force) {
        /* report back that using the new backend is not necessary
         * to achieve the desired configuration */
        ret = 1;
    } else {
        /* otherwise check the required capability */
        if (STREQ(backendType, "memory-backend-file") &&
            !virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_OBJECT_MEMORY_FILE)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("this qemu doesn't support the "
                             "memory-backend-file object"));
            goto cleanup;
        } else if (STREQ(backendType, "memory-backend-ram") &&
                   !virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_OBJECT_MEMORY_RAM)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("this qemu doesn't support the "
                             "memory-backend-ram object"));
            goto cleanup;
        } else if (STREQ(backendType, "memory-backend-memory") &&
                   !virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_OBJECT_MEMORY_MEMFD)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("this qemu doesn't support the "
                             "memory-backend-memfd object"));
            goto cleanup;
        }

        ret = 0;
    }

    if (!(*backendProps = qemuMonitorCreateObjectPropsWrap(backendType, alias,
                                                           &props)))
        ret = -1;

 cleanup:
    virJSONValueFree(props);
    VIR_FREE(memPath);
    return ret;
}


static int
qemuBuildMemoryCellBackendStr(virDomainDefPtr def,
                              virQEMUDriverConfigPtr cfg,
                              size_t cell,
                              qemuDomainObjPrivatePtr priv,
                              virBufferPtr buf)
{
    virJSONValuePtr props = NULL;
    char *alias = NULL;
    int ret = -1;
    int rc;
    virDomainMemoryDef mem = { 0 };
    unsigned long long memsize = virDomainNumaGetNodeMemorySize(def->numa,
                                                                cell);

    if (virAsprintf(&alias, "ram-node%zu", cell) < 0)
        goto cleanup;

    mem.size = memsize;
    mem.targetNode = cell;
    mem.info.alias = alias;

    if ((rc = qemuBuildMemoryBackendProps(&props, alias, cfg,
                                          priv, def, &mem, false)) < 0)
        goto cleanup;

    if (virQEMUBuildObjectCommandlineFromJSON(buf, props) < 0)
        goto cleanup;

    ret = rc;

 cleanup:
    VIR_FREE(alias);
    virJSONValueFree(props);

    return ret;
}


static int
qemuBuildMemoryDimmBackendStr(virBufferPtr buf,
                              virDomainMemoryDefPtr mem,
                              virDomainDefPtr def,
                              virQEMUDriverConfigPtr cfg,
                              qemuDomainObjPrivatePtr priv)
{
    virJSONValuePtr props = NULL;
    char *alias = NULL;
    int ret = -1;

    if (!mem->info.alias) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("memory device alias is not assigned"));
        return -1;
    }

    if (virAsprintf(&alias, "mem%s", mem->info.alias) < 0)
        goto cleanup;

    if (qemuBuildMemoryBackendProps(&props, alias, cfg,
                                    priv, def, mem, true) < 0)
        goto cleanup;

    if (virQEMUBuildObjectCommandlineFromJSON(buf, props) < 0)
        goto cleanup;

    ret = 0;

 cleanup:
    VIR_FREE(alias);
    virJSONValueFree(props);

    return ret;
}


char *
qemuBuildMemoryDeviceStr(virDomainMemoryDefPtr mem,
                         qemuDomainObjPrivatePtr priv)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    const char *device;

    if (!mem->info.alias) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("missing alias for memory device"));
        return NULL;
    }

    switch ((virDomainMemoryModel) mem->model) {
    case VIR_DOMAIN_MEMORY_MODEL_NVDIMM:
    case VIR_DOMAIN_MEMORY_MODEL_DIMM:

        if (mem->model == VIR_DOMAIN_MEMORY_MODEL_DIMM)
            device = "pc-dimm";
        else
            device = "nvdimm";

        virBufferAsprintf(&buf, "%s,", device);

        if (mem->targetNode >= 0)
            virBufferAsprintf(&buf, "node=%d,", mem->targetNode);

        if (mem->labelsize)
            virBufferAsprintf(&buf, "label-size=%llu,", mem->labelsize * 1024);

        if (mem->readonly) {
            if (!virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_DEVICE_NVDIMM_UNARMED)) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("nvdimm readonly property is not available "
                                 "with this QEMU binary"));
                return NULL;
            }
            virBufferAddLit(&buf, "unarmed=on,");
        }

        virBufferAsprintf(&buf, "memdev=mem%s,id=%s",
                          mem->info.alias, mem->info.alias);

        if (mem->info.type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_DIMM) {
            virBufferAsprintf(&buf, ",slot=%d", mem->info.addr.dimm.slot);
            if (mem->info.addr.dimm.base)
                virBufferAsprintf(&buf, ",addr=%llu", mem->info.addr.dimm.base);
        }

        break;

    case VIR_DOMAIN_MEMORY_MODEL_NONE:
    case VIR_DOMAIN_MEMORY_MODEL_LAST:
        break;

    }

    if (virBufferCheckError(&buf) < 0)
        return NULL;

    return virBufferContentAndReset(&buf);
}


static char *
qemuBuildLegacyNicStr(virDomainNetDefPtr net)
{
    char *str;
    char macaddr[VIR_MAC_STRING_BUFLEN];
    const char *netmodel = virDomainNetGetModelString(net);

    ignore_value(virAsprintf(&str,
                             "nic,macaddr=%s,netdev=host%s%s%s%s%s",
                             virMacAddrFormat(&net->mac, macaddr),
                             net->info.alias,
                             netmodel ? ",model=" : "",
                             NULLSTR_EMPTY(netmodel),
                             (net->info.alias ? ",id=" : ""),
                             NULLSTR_EMPTY(net->info.alias)));
    return str;
}


char *
qemuBuildNicDevStr(virDomainDefPtr def,
                   virDomainNetDefPtr net,
                   unsigned int bootindex,
                   size_t vhostfdSize,
                   virQEMUCapsPtr qemuCaps)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    bool usingVirtio = false;
    char macaddr[VIR_MAC_STRING_BUFLEN];

    if (virDomainNetIsVirtioModel(net)) {
        if (qemuBuildVirtioDevStr(&buf, "virtio-net", qemuCaps,
                                  VIR_DOMAIN_DEVICE_NET, net) < 0) {
            goto error;
        }

        usingVirtio = true;
    } else {
        virBufferAddStr(&buf, virDomainNetGetModelString(net));
    }

    if (usingVirtio && net->driver.virtio.txmode) {
        if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_VIRTIO_TX_ALG)) {
            virBufferAddLit(&buf, ",tx=");
            switch (net->driver.virtio.txmode) {
                case VIR_DOMAIN_NET_VIRTIO_TX_MODE_IOTHREAD:
                    virBufferAddLit(&buf, "bh");
                    break;

                case VIR_DOMAIN_NET_VIRTIO_TX_MODE_TIMER:
                    virBufferAddLit(&buf, "timer");
                    break;

                case VIR_DOMAIN_NET_VIRTIO_TX_MODE_DEFAULT:
                    break;

                case VIR_DOMAIN_NET_VIRTIO_TX_MODE_LAST:
                default:
                    /* this should never happen, if it does, we need
                     * to add another case to this switch.
                     */
                    virReportEnumRangeError(virDomainNetVirtioTxModeType,
                                            net->driver.virtio.txmode);
                    goto error;
            }
        } else {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("virtio-net-pci 'tx' option not supported in this QEMU binary"));
            goto error;
        }
    }
    if (usingVirtio) {
        qemuBuildIoEventFdStr(&buf, net->driver.virtio.ioeventfd, qemuCaps);
        if (net->driver.virtio.event_idx &&
            virQEMUCapsGet(qemuCaps, QEMU_CAPS_VIRTIO_NET_EVENT_IDX)) {
            virBufferAsprintf(&buf, ",event_idx=%s",
                              virTristateSwitchTypeToString(net->driver.virtio.event_idx));
        }
        if (net->driver.virtio.host.csum) {
            virBufferAsprintf(&buf, ",csum=%s",
                              virTristateSwitchTypeToString(net->driver.virtio.host.csum));
        }
        if (net->driver.virtio.host.gso) {
            virBufferAsprintf(&buf, ",gso=%s",
                              virTristateSwitchTypeToString(net->driver.virtio.host.gso));
        }
        if (net->driver.virtio.host.tso4) {
            virBufferAsprintf(&buf, ",host_tso4=%s",
                              virTristateSwitchTypeToString(net->driver.virtio.host.tso4));
        }
        if (net->driver.virtio.host.tso6) {
            virBufferAsprintf(&buf, ",host_tso6=%s",
                              virTristateSwitchTypeToString(net->driver.virtio.host.tso6));
        }
        if (net->driver.virtio.host.ecn) {
            virBufferAsprintf(&buf, ",host_ecn=%s",
                              virTristateSwitchTypeToString(net->driver.virtio.host.ecn));
        }
        if (net->driver.virtio.host.ufo) {
            virBufferAsprintf(&buf, ",host_ufo=%s",
                              virTristateSwitchTypeToString(net->driver.virtio.host.ufo));
        }
        if (net->driver.virtio.host.mrg_rxbuf) {
            virBufferAsprintf(&buf, ",mrg_rxbuf=%s",
                              virTristateSwitchTypeToString(net->driver.virtio.host.mrg_rxbuf));
        }
        if (net->driver.virtio.guest.csum) {
            virBufferAsprintf(&buf, ",guest_csum=%s",
                              virTristateSwitchTypeToString(net->driver.virtio.guest.csum));
        }
        if (net->driver.virtio.guest.tso4) {
            virBufferAsprintf(&buf, ",guest_tso4=%s",
                              virTristateSwitchTypeToString(net->driver.virtio.guest.tso4));
        }
        if (net->driver.virtio.guest.tso6) {
            virBufferAsprintf(&buf, ",guest_tso6=%s",
                              virTristateSwitchTypeToString(net->driver.virtio.guest.tso6));
        }
        if (net->driver.virtio.guest.ecn) {
            virBufferAsprintf(&buf, ",guest_ecn=%s",
                              virTristateSwitchTypeToString(net->driver.virtio.guest.ecn));
        }
        if (net->driver.virtio.guest.ufo) {
            virBufferAsprintf(&buf, ",guest_ufo=%s",
                              virTristateSwitchTypeToString(net->driver.virtio.guest.ufo));
        }
    }
    if (usingVirtio && vhostfdSize > 1) {
        if (net->info.type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_CCW) {
            /* ccw provides a one to one relation of fds to queues and
             * does not support the vectors option
             */
            virBufferAddLit(&buf, ",mq=on");
        } else {
            /* As advised at https://www.linux-kvm.org/page/Multiqueue
             * we should add vectors=2*N+2 where N is the vhostfdSize
             */
            virBufferAsprintf(&buf, ",mq=on,vectors=%zu", 2 * vhostfdSize + 2);
        }
    }
    if (usingVirtio && net->driver.virtio.rx_queue_size) {
        if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_VIRTIO_NET_RX_QUEUE_SIZE)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("virtio rx_queue_size option is not supported with this QEMU binary"));
            goto error;
        }
        virBufferAsprintf(&buf, ",rx_queue_size=%u", net->driver.virtio.rx_queue_size);
    }
    if (usingVirtio && net->driver.virtio.tx_queue_size) {
        if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_VIRTIO_NET_TX_QUEUE_SIZE)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("virtio tx_queue_size option is not supported with this QEMU binary"));
            goto error;
        }
        virBufferAsprintf(&buf, ",tx_queue_size=%u", net->driver.virtio.tx_queue_size);
    }

    if (usingVirtio && net->mtu) {
        if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_VIRTIO_NET_HOST_MTU)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("setting MTU is not supported with this QEMU binary"));
            goto error;
        }
        virBufferAsprintf(&buf, ",host_mtu=%u", net->mtu);
    }

    virBufferAsprintf(&buf, ",netdev=host%s", net->info.alias);
    virBufferAsprintf(&buf, ",id=%s", net->info.alias);
    virBufferAsprintf(&buf, ",mac=%s",
                      virMacAddrFormat(&net->mac, macaddr));

    if (qemuBuildDeviceAddressStr(&buf, def, &net->info, qemuCaps) < 0)
        goto error;
    if (qemuBuildRomStr(&buf, &net->info) < 0)
        goto error;
    if (bootindex)
        virBufferAsprintf(&buf, ",bootindex=%u", bootindex);
    if (usingVirtio &&
        qemuBuildVirtioOptionsStr(&buf, net->virtio, qemuCaps) < 0)
        goto error;

    if (virBufferCheckError(&buf) < 0)
        goto error;

    return virBufferContentAndReset(&buf);

 error:
    virBufferFreeAndReset(&buf);
    return NULL;
}


char *
qemuBuildHostNetStr(virDomainNetDefPtr net,
                    virQEMUDriverPtr driver,
                    char **tapfd,
                    size_t tapfdSize,
                    char **vhostfd,
                    size_t vhostfdSize)
{
    bool is_tap = false;
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    virDomainNetType netType = virDomainNetGetActualType(net);
    virQEMUDriverConfigPtr cfg = virQEMUDriverGetConfig(driver);
    size_t i;
    char *addr = NULL;
    char *ret = NULL;

    if (net->script && netType != VIR_DOMAIN_NET_TYPE_ETHERNET) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("scripts are not supported on interfaces of type %s"),
                       virDomainNetTypeToString(netType));
        goto cleanup;
    }

    switch (netType) {
        /*
         * If type='bridge', and we're running as privileged user
         * or -netdev bridge is not supported then it will fall
         * through, -net tap,fd
         */
    case VIR_DOMAIN_NET_TYPE_BRIDGE:
    case VIR_DOMAIN_NET_TYPE_NETWORK:
    case VIR_DOMAIN_NET_TYPE_DIRECT:
    case VIR_DOMAIN_NET_TYPE_ETHERNET:
        virBufferAddLit(&buf, "tap,");
        /* for one tapfd 'fd=' shall be used,
         * for more than one 'fds=' is the right choice */
        if (tapfdSize == 1) {
            virBufferAsprintf(&buf, "fd=%s,", tapfd[0]);
        } else {
            virBufferAddLit(&buf, "fds=");
            for (i = 0; i < tapfdSize; i++) {
                if (i)
                    virBufferAddChar(&buf, ':');
                virBufferAdd(&buf, tapfd[i], -1);
            }
            virBufferAddChar(&buf, ',');
        }
        is_tap = true;
        break;

    case VIR_DOMAIN_NET_TYPE_CLIENT:
        virBufferAsprintf(&buf, "socket,connect=%s:%d,",
                          net->data.socket.address,
                          net->data.socket.port);
        break;

    case VIR_DOMAIN_NET_TYPE_SERVER:
        virBufferAsprintf(&buf, "socket,listen=%s:%d,",
                          NULLSTR_EMPTY(net->data.socket.address),
                          net->data.socket.port);
        break;

    case VIR_DOMAIN_NET_TYPE_MCAST:
        virBufferAsprintf(&buf, "socket,mcast=%s:%d,",
                          net->data.socket.address,
                          net->data.socket.port);
        break;

    case VIR_DOMAIN_NET_TYPE_UDP:
        virBufferAsprintf(&buf, "socket,udp=%s:%d,localaddr=%s:%d,",
                          net->data.socket.address,
                          net->data.socket.port,
                          net->data.socket.localaddr,
                          net->data.socket.localport);
        break;

    case VIR_DOMAIN_NET_TYPE_USER:
        virBufferAddLit(&buf, "user,");
        for (i = 0; i < net->guestIP.nips; i++) {
            const virNetDevIPAddr *ip = net->guestIP.ips[i];
            const char *prefix = "";

            if (!(addr = virSocketAddrFormat(&ip->address)))
                goto cleanup;

            if (VIR_SOCKET_ADDR_IS_FAMILY(&ip->address, AF_INET))
                prefix = "net=";
            if (VIR_SOCKET_ADDR_IS_FAMILY(&ip->address, AF_INET6))
                prefix = "ipv6-net=";

            virBufferAsprintf(&buf, "%s%s", prefix, addr);
            if (ip->prefix)
                virBufferAsprintf(&buf, "/%u", ip->prefix);
            virBufferAddChar(&buf, ',');
            VIR_FREE(addr);
        }
        break;

    case VIR_DOMAIN_NET_TYPE_INTERNAL:
        virBufferAddLit(&buf, "user,");
        break;

    case VIR_DOMAIN_NET_TYPE_VHOSTUSER:
        virBufferAsprintf(&buf, "vhost-user,chardev=char%s,",
                          net->info.alias);
        if (net->driver.virtio.queues > 1)
            virBufferAsprintf(&buf, "queues=%u,",
                              net->driver.virtio.queues);
        break;

    case VIR_DOMAIN_NET_TYPE_HOSTDEV:
        /* Should have been handled earlier via PCI/USB hotplug code. */
    case VIR_DOMAIN_NET_TYPE_LAST:
        break;
    }

    virBufferAsprintf(&buf, "id=host%s,", net->info.alias);

    if (is_tap) {
        if (vhostfdSize) {
            virBufferAddLit(&buf, "vhost=on,");
            if (vhostfdSize == 1) {
                virBufferAsprintf(&buf, "vhostfd=%s,", vhostfd[0]);
            } else {
                virBufferAddLit(&buf, "vhostfds=");
                for (i = 0; i < vhostfdSize; i++) {
                    if (i)
                        virBufferAddChar(&buf, ':');
                    virBufferAdd(&buf, vhostfd[i], -1);
                }
                virBufferAddChar(&buf, ',');
            }
        }
        if (net->tune.sndbuf_specified)
            virBufferAsprintf(&buf, "sndbuf=%lu,", net->tune.sndbuf);
    }


    virBufferTrim(&buf, ",", -1);
    if (virBufferCheckError(&buf) < 0)
        goto cleanup;

    ret = virBufferContentAndReset(&buf);
 cleanup:
    virBufferFreeAndReset(&buf);
    virObjectUnref(cfg);
    VIR_FREE(addr);
    return ret;
}


char *
qemuBuildWatchdogDevStr(const virDomainDef *def,
                        virDomainWatchdogDefPtr dev,
                        virQEMUCapsPtr qemuCaps)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;

    const char *model = virDomainWatchdogModelTypeToString(dev->model);
    if (!model) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("missing watchdog model"));
        goto error;
    }

    virBufferAsprintf(&buf, "%s,id=%s", model, dev->info.alias);
    if (qemuBuildDeviceAddressStr(&buf, def, &dev->info, qemuCaps) < 0)
        goto error;

    if (virBufferCheckError(&buf) < 0)
        goto error;

    return virBufferContentAndReset(&buf);

 error:
    virBufferFreeAndReset(&buf);
    return NULL;
}


static int
qemuBuildWatchdogCommandLine(virCommandPtr cmd,
                             const virDomainDef *def,
                             virQEMUCapsPtr qemuCaps)
{
    virDomainWatchdogDefPtr watchdog = def->watchdog;
    char *optstr;
    const char *action;
    int actualAction;

    if (!def->watchdog)
        return 0;

    if (qemuCommandAddExtDevice(cmd, &def->watchdog->info) < 0)
        return -1;

    virCommandAddArg(cmd, "-device");

    optstr = qemuBuildWatchdogDevStr(def, watchdog, qemuCaps);
    if (!optstr)
        return -1;

    virCommandAddArg(cmd, optstr);
    VIR_FREE(optstr);

    /* qemu doesn't have a 'dump' action; we tell qemu to 'pause', then
       libvirt listens for the watchdog event, and we perform the dump
       ourselves. so convert 'dump' to 'pause' for the qemu cli */
    actualAction = watchdog->action;
    if (watchdog->action == VIR_DOMAIN_WATCHDOG_ACTION_DUMP)
        actualAction = VIR_DOMAIN_WATCHDOG_ACTION_PAUSE;

    action = virDomainWatchdogActionTypeToString(actualAction);
    if (!action) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("invalid watchdog action"));
        return -1;
    }
    virCommandAddArgList(cmd, "-watchdog-action", action, NULL);

    return 0;
}


static int
qemuBuildMemballoonCommandLine(virCommandPtr cmd,
                               const virDomainDef *def,
                               virQEMUCapsPtr qemuCaps)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;

    if (!virDomainDefHasMemballoon(def))
        return 0;

    if (qemuBuildVirtioDevStr(&buf, "virtio-balloon", qemuCaps,
                              VIR_DOMAIN_DEVICE_MEMBALLOON,
                              def->memballoon) < 0) {
        goto error;
    }

    virBufferAsprintf(&buf, ",id=%s", def->memballoon->info.alias);
    if (qemuBuildDeviceAddressStr(&buf, def, &def->memballoon->info, qemuCaps) < 0)
        goto error;

    if (def->memballoon->autodeflate != VIR_TRISTATE_SWITCH_ABSENT) {
        virBufferAsprintf(&buf, ",deflate-on-oom=%s",
                          virTristateSwitchTypeToString(def->memballoon->autodeflate));
    }

    if (qemuBuildVirtioOptionsStr(&buf, def->memballoon->virtio, qemuCaps) < 0)
        goto error;

    if (qemuCommandAddExtDevice(cmd, &def->memballoon->info) < 0)
        goto error;

    virCommandAddArg(cmd, "-device");
    virCommandAddArgBuffer(cmd, &buf);
    return 0;

 error:
    virBufferFreeAndReset(&buf);
    return -1;
}


static char *
qemuBuildNVRAMDevStr(virDomainNVRAMDefPtr dev)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;

    if (dev->info.type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_SPAPRVIO &&
        dev->info.addr.spaprvio.has_reg) {
        virBufferAsprintf(&buf, "spapr-nvram.reg=0x%llx",
                          dev->info.addr.spaprvio.reg);
    } else {
        virReportError(VIR_ERR_XML_ERROR, "%s",
                       _("nvram address type must be spaprvio"));
        goto error;
    }

    if (virBufferCheckError(&buf) < 0)
        goto error;

    return virBufferContentAndReset(&buf);

 error:
    virBufferFreeAndReset(&buf);
    return NULL;
}


static int
qemuBuildNVRAMCommandLine(virCommandPtr cmd,
                          const virDomainDef *def,
                          virQEMUCapsPtr qemuCaps)
{
    if (!def->nvram)
        return 0;

    if (qemuDomainIsPSeries(def)) {
        if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE_NVRAM)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("nvram device is not supported by "
                             "this QEMU binary"));
            return -1;
        }

        char *optstr;
        virCommandAddArg(cmd, "-global");
        optstr = qemuBuildNVRAMDevStr(def->nvram);
        if (!optstr)
            return -1;
        virCommandAddArg(cmd, optstr);
        VIR_FREE(optstr);
    } else {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                      _("nvram device is only supported for PPC64"));
        return -1;
    }

    return 0;
}


static char *
qemuBuildVirtioInputDevStr(const virDomainDef *def,
                           virDomainInputDefPtr dev,
                           virQEMUCapsPtr qemuCaps)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;

    switch ((virDomainInputType)dev->type) {
    case VIR_DOMAIN_INPUT_TYPE_MOUSE:
        if (qemuBuildVirtioDevStr(&buf, "virtio-mouse", qemuCaps,
                                  VIR_DOMAIN_DEVICE_INPUT, dev) < 0) {
            goto error;
        }
        break;
    case VIR_DOMAIN_INPUT_TYPE_TABLET:
        if (qemuBuildVirtioDevStr(&buf, "virtio-tablet", qemuCaps,
                                  VIR_DOMAIN_DEVICE_INPUT, dev) < 0) {
            goto error;
        }
        break;
    case VIR_DOMAIN_INPUT_TYPE_KBD:
        if (qemuBuildVirtioDevStr(&buf, "virtio-keyboard", qemuCaps,
                                  VIR_DOMAIN_DEVICE_INPUT, dev) < 0) {
            goto error;
        }
        break;
    case VIR_DOMAIN_INPUT_TYPE_PASSTHROUGH:
        if (qemuBuildVirtioDevStr(&buf, "virtio-input-host", qemuCaps,
                                  VIR_DOMAIN_DEVICE_INPUT, dev) < 0) {
            goto error;
        }
        break;
    case VIR_DOMAIN_INPUT_TYPE_LAST:
    default:
        virReportEnumRangeError(virDomainInputType, dev->type);
        goto error;
    }

    virBufferAsprintf(&buf, ",id=%s", dev->info.alias);

    if (dev->type == VIR_DOMAIN_INPUT_TYPE_PASSTHROUGH) {
        virBufferAddLit(&buf, ",evdev=");
        virQEMUBuildBufferEscapeComma(&buf, dev->source.evdev);
    }

    if (qemuBuildDeviceAddressStr(&buf, def, &dev->info, qemuCaps) < 0)
        goto error;

    if (qemuBuildVirtioOptionsStr(&buf, dev->virtio, qemuCaps) < 0)
        goto error;

    if (virBufferCheckError(&buf) < 0)
        goto error;

    return virBufferContentAndReset(&buf);

 error:
    virBufferFreeAndReset(&buf);
    return NULL;
}

static char *
qemuBuildUSBInputDevStr(const virDomainDef *def,
                        virDomainInputDefPtr dev,
                        virQEMUCapsPtr qemuCaps)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;

    switch (dev->type) {
    case VIR_DOMAIN_INPUT_TYPE_MOUSE:
        virBufferAsprintf(&buf, "usb-mouse,id=%s", dev->info.alias);
        break;
    case VIR_DOMAIN_INPUT_TYPE_TABLET:
        virBufferAsprintf(&buf, "usb-tablet,id=%s", dev->info.alias);
        break;
    case VIR_DOMAIN_INPUT_TYPE_KBD:
        if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE_USB_KBD)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("usb keyboard is not supported by this "
                             "QEMU binary"));
            goto error;
        }
        virBufferAsprintf(&buf, "usb-kbd,id=%s", dev->info.alias);
        break;
    }

    if (qemuBuildDeviceAddressStr(&buf, def, &dev->info, qemuCaps) < 0)
        goto error;

    if (virBufferCheckError(&buf) < 0)
        goto error;

    return virBufferContentAndReset(&buf);

 error:
    virBufferFreeAndReset(&buf);
    return NULL;
}


int
qemuBuildInputDevStr(char **devstr,
                     const virDomainDef *def,
                     virDomainInputDefPtr input,
                     virQEMUCapsPtr qemuCaps)
{
    switch (input->bus) {
    case VIR_DOMAIN_INPUT_BUS_USB:
        if (!(*devstr = qemuBuildUSBInputDevStr(def, input, qemuCaps)))
            return -1;
        break;

    case VIR_DOMAIN_INPUT_BUS_VIRTIO:
        if (!(*devstr = qemuBuildVirtioInputDevStr(def, input, qemuCaps)))
            return -1;
        break;
    }
    return 0;
}


static int
qemuBuildInputCommandLine(virCommandPtr cmd,
                          const virDomainDef *def,
                          virQEMUCapsPtr qemuCaps)
{
    size_t i;

    for (i = 0; i < def->ninputs; i++) {
        virDomainInputDefPtr input = def->inputs[i];
        char *devstr = NULL;

        if (qemuCommandAddExtDevice(cmd, &input->info) < 0)
            return -1;

        if (qemuBuildInputDevStr(&devstr, def, input, qemuCaps) < 0)
            return -1;

        if (devstr) {
            virCommandAddArg(cmd, "-device");
            virCommandAddArg(cmd, devstr);
        }

        VIR_FREE(devstr);
    }

    return 0;
}


static char *
qemuBuildSoundDevStr(const virDomainDef *def,
                     virDomainSoundDefPtr sound,
                     virQEMUCapsPtr qemuCaps)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    const char *model = NULL;

    /* Hack for devices with different names in QEMU and libvirt */
    switch ((virDomainSoundModel) sound->model) {
    case VIR_DOMAIN_SOUND_MODEL_ES1370:
        model = "ES1370";
        break;
    case VIR_DOMAIN_SOUND_MODEL_AC97:
        model = "AC97";
        break;
    case VIR_DOMAIN_SOUND_MODEL_ICH6:
        model = "intel-hda";
        break;
    case VIR_DOMAIN_SOUND_MODEL_USB:
        model = "usb-audio";
        if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_OBJECT_USB_AUDIO)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("usb-audio controller is not supported "
                             "by this QEMU binary"));
            goto error;
        }
        break;
    case VIR_DOMAIN_SOUND_MODEL_ICH9:
        model = "ich9-intel-hda";
        if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE_ICH9_INTEL_HDA)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("The ich9-intel-hda audio controller "
                             "is not supported in this QEMU binary"));
            goto error;
        }
        break;
    case VIR_DOMAIN_SOUND_MODEL_SB16:
        model = "sb16";
        break;
    case VIR_DOMAIN_SOUND_MODEL_PCSPK: /* pc-speaker is handled separately */
    case VIR_DOMAIN_SOUND_MODEL_LAST:
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("sound card model '%s' is not supported by qemu"),
                       virDomainSoundModelTypeToString(sound->model));
        goto error;
    }

    virBufferAsprintf(&buf, "%s,id=%s", model, sound->info.alias);
    if (qemuBuildDeviceAddressStr(&buf, def, &sound->info, qemuCaps) < 0)
        goto error;

    if (virBufferCheckError(&buf) < 0)
        goto error;

    return virBufferContentAndReset(&buf);

 error:
    virBufferFreeAndReset(&buf);
    return NULL;
}


static int
qemuSoundCodecTypeToCaps(int type)
{
    switch (type) {
    case VIR_DOMAIN_SOUND_CODEC_TYPE_DUPLEX:
        return QEMU_CAPS_HDA_DUPLEX;
    case VIR_DOMAIN_SOUND_CODEC_TYPE_MICRO:
        return QEMU_CAPS_HDA_MICRO;
    case VIR_DOMAIN_SOUND_CODEC_TYPE_OUTPUT:
        return QEMU_CAPS_HDA_OUTPUT;
    default:
        return -1;
    }
}


static char *
qemuBuildSoundCodecStr(virDomainSoundDefPtr sound,
                       virDomainSoundCodecDefPtr codec,
                       virQEMUCapsPtr qemuCaps)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    const char *stype;
    int type, flags;

    type = codec->type;
    stype = qemuSoundCodecTypeToString(type);
    flags = qemuSoundCodecTypeToCaps(type);

    if (flags == -1 || !virQEMUCapsGet(qemuCaps, flags)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("%s not supported in this QEMU binary"), stype);
        goto error;
    }

    virBufferAsprintf(&buf, "%s,id=%s-codec%d,bus=%s.0,cad=%d",
                      stype, sound->info.alias, codec->cad, sound->info.alias, codec->cad);

    return virBufferContentAndReset(&buf);

 error:
    virBufferFreeAndReset(&buf);
    return NULL;
}


static int
qemuBuildSoundCommandLine(virCommandPtr cmd,
                          const virDomainDef *def,
                          virQEMUCapsPtr qemuCaps)
{
    size_t i, j;

    for (i = 0; i < def->nsounds; i++) {
        virDomainSoundDefPtr sound = def->sounds[i];
        char *str = NULL;

        /* Sadly pcspk device doesn't use -device syntax. Fortunately
         * we don't need to set any PCI address on it, so we don't
         * mind too much */
        if (sound->model == VIR_DOMAIN_SOUND_MODEL_PCSPK) {
            virCommandAddArgList(cmd, "-soundhw", "pcspk", NULL);
        } else {
            if (qemuCommandAddExtDevice(cmd, &sound->info) < 0)
                return -1;

            virCommandAddArg(cmd, "-device");
            if (!(str = qemuBuildSoundDevStr(def, sound, qemuCaps)))
                return -1;

            virCommandAddArg(cmd, str);
            VIR_FREE(str);
            if (sound->model == VIR_DOMAIN_SOUND_MODEL_ICH6 ||
                sound->model == VIR_DOMAIN_SOUND_MODEL_ICH9) {
                char *codecstr = NULL;

                for (j = 0; j < sound->ncodecs; j++) {
                    virCommandAddArg(cmd, "-device");
                    if (!(codecstr =
                          qemuBuildSoundCodecStr(sound, sound->codecs[j],
                                                 qemuCaps))) {
                        return -1;

                    }
                    virCommandAddArg(cmd, codecstr);
                    VIR_FREE(codecstr);
                }
                if (j == 0) {
                    virDomainSoundCodecDef codec = {
                        VIR_DOMAIN_SOUND_CODEC_TYPE_DUPLEX,
                        0
                    };
                    virCommandAddArg(cmd, "-device");
                    if (!(codecstr =
                          qemuBuildSoundCodecStr(sound, &codec,
                                                 qemuCaps))) {
                        return -1;

                    }
                    virCommandAddArg(cmd, codecstr);
                    VIR_FREE(codecstr);
                }
            }
        }
    }
    return 0;
}



static char *
qemuBuildDeviceVideoStr(const virDomainDef *def,
                        virDomainVideoDefPtr video,
                        virQEMUCapsPtr qemuCaps)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    const char *model;

    /* We try to chose the best model for primary video device by preferring
     * model with VGA compatibility mode.  For some video devices on some
     * architectures there might not be such model so fallback to one
     * without VGA compatibility mode. */
    if (video->primary && qemuDomainSupportsVideoVga(video, qemuCaps))
        model = qemuDeviceVideoTypeToString(video->type);
    else
        model = qemuDeviceVideoSecondaryTypeToString(video->type);

    if (!model || STREQ(model, "")) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("invalid model for video type '%s'"),
                       virDomainVideoTypeToString(video->type));
        goto error;
    }

    if (STREQ(model, "virtio-gpu")) {
        if (qemuBuildVirtioDevStr(&buf, "virtio-gpu", qemuCaps,
                                  VIR_DOMAIN_DEVICE_VIDEO, video) < 0) {
            goto error;
        }
    } else {
        virBufferAsprintf(&buf, "%s", model);
    }

    virBufferAsprintf(&buf, ",id=%s", video->info.alias);

    if (video->accel && video->accel->accel3d == VIR_TRISTATE_SWITCH_ON) {
        virBufferAsprintf(&buf, ",virgl=%s",
                          virTristateSwitchTypeToString(video->accel->accel3d));
    }

    if (video->type == VIR_DOMAIN_VIDEO_TYPE_QXL) {
        if (video->ram) {
            /* QEMU accepts bytes for ram_size. */
            virBufferAsprintf(&buf, ",ram_size=%u", video->ram * 1024);
        }

        if (video->vram) {
            /* QEMU accepts bytes for vram_size. */
            virBufferAsprintf(&buf, ",vram_size=%u", video->vram * 1024);
        }

        if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_QXL_VRAM64)) {
            /* QEMU accepts mebibytes for vram64_size_mb. */
            virBufferAsprintf(&buf, ",vram64_size_mb=%u", video->vram64 / 1024);
        }

        if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_QXL_VGAMEM)) {
            /* QEMU accepts mebibytes for vgamem_mb. */
            virBufferAsprintf(&buf, ",vgamem_mb=%u", video->vgamem / 1024);
        }

        if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_QXL_MAX_OUTPUTS)) {
            if (video->heads)
                virBufferAsprintf(&buf, ",max_outputs=%u", video->heads);
        }
    } else if (video->type == VIR_DOMAIN_VIDEO_TYPE_VIRTIO) {
        if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_VIRTIO_GPU_MAX_OUTPUTS)) {
            if (video->heads)
                virBufferAsprintf(&buf, ",max_outputs=%u", video->heads);
        }
    } else if (video->vram &&
        ((video->type == VIR_DOMAIN_VIDEO_TYPE_VGA &&
          virQEMUCapsGet(qemuCaps, QEMU_CAPS_VGA_VGAMEM)) ||
         (video->type == VIR_DOMAIN_VIDEO_TYPE_VMVGA &&
          virQEMUCapsGet(qemuCaps, QEMU_CAPS_VMWARE_SVGA_VGAMEM)))) {

        virBufferAsprintf(&buf, ",vgamem_mb=%u", video->vram / 1024);
    }

    if (qemuBuildDeviceAddressStr(&buf, def, &video->info, qemuCaps) < 0)
        goto error;

    if (qemuBuildVirtioOptionsStr(&buf, video->virtio, qemuCaps) < 0)
        goto error;

    if (virBufferCheckError(&buf) < 0)
        goto error;

    return virBufferContentAndReset(&buf);

 error:
    virBufferFreeAndReset(&buf);
    return NULL;
}


static int
qemuBuildVgaVideoCommand(virCommandPtr cmd,
                         virDomainVideoDefPtr video,
                         virQEMUCapsPtr qemuCaps)
{
    const char *vgastr = qemuVideoTypeToString(video->type);
    if (!vgastr || STREQ(vgastr, "")) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("invalid model for video type '%s'"),
                       virDomainVideoTypeToString(video->type));
        return -1;
    }

    virCommandAddArgList(cmd, "-vga", vgastr, NULL);

    /* If we cannot use --device option to specify the video device
     * in QEMU we will fallback to the old --vga option. To get the
     * correct device name for the --vga option the 'qemuVideo' is
     * used, but to set some device attributes we need to use the
     * --global option and for that we need to specify the device
     * name the same as for --device option and for that we need to
     * use 'qemuDeviceVideo'.
     *
     * See 'Graphics Devices' section in docs/qdev-device-use.txt in
     * QEMU repository.
     */
    const char *dev = qemuDeviceVideoTypeToString(video->type);

    if (video->type == VIR_DOMAIN_VIDEO_TYPE_QXL &&
        (video->vram || video->ram)) {
        unsigned int ram = video->ram;
        unsigned int vram = video->vram;
        unsigned int vram64 = video->vram64;
        unsigned int vgamem = video->vgamem;
        unsigned int heads = video->heads;

        if (ram) {
            virCommandAddArg(cmd, "-global");
            virCommandAddArgFormat(cmd, "%s.ram_size=%u",
                                   dev, ram * 1024);
        }
        if (vram) {
            virCommandAddArg(cmd, "-global");
            virCommandAddArgFormat(cmd, "%s.vram_size=%u",
                                   dev, vram * 1024);
        }
        if (vram64 &&
            virQEMUCapsGet(qemuCaps, QEMU_CAPS_QXL_VRAM64)) {
            virCommandAddArg(cmd, "-global");
            virCommandAddArgFormat(cmd, "%s.vram64_size_mb=%u",
                                   dev, vram64 / 1024);
        }
        if (vgamem &&
            virQEMUCapsGet(qemuCaps, QEMU_CAPS_QXL_VGAMEM)) {
            virCommandAddArg(cmd, "-global");
            virCommandAddArgFormat(cmd, "%s.vgamem_mb=%u",
                                   dev, vgamem / 1024);
        }
        if (heads &&
            virQEMUCapsGet(qemuCaps, QEMU_CAPS_QXL_MAX_OUTPUTS)) {
            virCommandAddArg(cmd, "-global");
            virCommandAddArgFormat(cmd, "%s.max_outputs=%u",
                                   dev, heads);
        }
    }

    if (video->vram &&
        ((video->type == VIR_DOMAIN_VIDEO_TYPE_VGA &&
          virQEMUCapsGet(qemuCaps, QEMU_CAPS_VGA_VGAMEM)) ||
         (video->type == VIR_DOMAIN_VIDEO_TYPE_VMVGA &&
          virQEMUCapsGet(qemuCaps, QEMU_CAPS_VMWARE_SVGA_VGAMEM)))) {
        unsigned int vram = video->vram;

        virCommandAddArg(cmd, "-global");
        virCommandAddArgFormat(cmd, "%s.vgamem_mb=%u",
                               dev, vram / 1024);
    }

    return 0;
}


static int
qemuBuildVideoCommandLine(virCommandPtr cmd,
                          const virDomainDef *def,
                          virQEMUCapsPtr qemuCaps)
{
    size_t i;

    for (i = 0; i < def->nvideos; i++) {
        char *str = NULL;
        virDomainVideoDefPtr video = def->videos[i];

        if (video->type == VIR_DOMAIN_VIDEO_TYPE_NONE)
            continue;

        if (video->primary) {
            if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE_VIDEO_PRIMARY)) {

                if (qemuCommandAddExtDevice(cmd,
                                            &def->videos[i]->info) < 0)
                    return -1;

                virCommandAddArg(cmd, "-device");

                if (!(str = qemuBuildDeviceVideoStr(def, video, qemuCaps)))
                    return -1;

                virCommandAddArg(cmd, str);
                VIR_FREE(str);
            } else {
                if (qemuBuildVgaVideoCommand(cmd, video, qemuCaps) < 0)
                    return -1;
            }
        } else {
            if (qemuCommandAddExtDevice(cmd, &def->videos[i]->info) < 0)
                return -1;

            virCommandAddArg(cmd, "-device");

            if (!(str = qemuBuildDeviceVideoStr(def, video, qemuCaps)))
                return -1;

            virCommandAddArg(cmd, str);
            VIR_FREE(str);
        }
    }

    return 0;
}


int
qemuOpenPCIConfig(virDomainHostdevDefPtr dev)
{
    virDomainHostdevSubsysPCIPtr pcisrc = &dev->source.subsys.u.pci;
    char *path = NULL;
    int configfd = -1;

    if (virAsprintf(&path, "/sys/bus/pci/devices/%04x:%02x:%02x.%01x/config",
                    pcisrc->addr.domain, pcisrc->addr.bus,
                    pcisrc->addr.slot, pcisrc->addr.function) < 0)
        return -1;

    configfd = open(path, O_RDWR, 0);

    if (configfd < 0)
        virReportSystemError(errno, _("Failed opening %s"), path);

    VIR_FREE(path);

    return configfd;
}

char *
qemuBuildPCIHostdevDevStr(const virDomainDef *def,
                          virDomainHostdevDefPtr dev,
                          unsigned int bootIndex, /* used iff dev->info->bootIndex == 0 */
                          const char *configfd,
                          virQEMUCapsPtr qemuCaps)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    virDomainHostdevSubsysPCIPtr pcisrc = &dev->source.subsys.u.pci;
    int backend = pcisrc->backend;

    /* caller has to assign proper passthrough backend type */
    switch ((virDomainHostdevSubsysPCIBackendType)backend) {
    case VIR_DOMAIN_HOSTDEV_PCI_BACKEND_KVM:
        virBufferAddLit(&buf, "pci-assign");
        if (configfd && *configfd)
            virBufferAsprintf(&buf, ",configfd=%s", configfd);
        break;

    case VIR_DOMAIN_HOSTDEV_PCI_BACKEND_VFIO:
        virBufferAddLit(&buf, "vfio-pci");
        break;

    case VIR_DOMAIN_HOSTDEV_PCI_BACKEND_DEFAULT:
    case VIR_DOMAIN_HOSTDEV_PCI_BACKEND_XEN:
    case VIR_DOMAIN_HOSTDEV_PCI_BACKEND_TYPE_LAST:
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("invalid PCI passthrough type '%s'"),
                       virDomainHostdevSubsysPCIBackendTypeToString(backend));
        goto error;
    }

    virBufferAddLit(&buf, ",host=");
    if (pcisrc->addr.domain)
        virBufferAsprintf(&buf, "%.4x:", pcisrc->addr.domain);
    virBufferAsprintf(&buf, "%.2x:%.2x.%.1x",
                      pcisrc->addr.bus, pcisrc->addr.slot,
                      pcisrc->addr.function);
    virBufferAsprintf(&buf, ",id=%s", dev->info->alias);
    if (dev->info->bootIndex)
        bootIndex = dev->info->bootIndex;
    if (bootIndex)
        virBufferAsprintf(&buf, ",bootindex=%u", bootIndex);
    if (qemuBuildDeviceAddressStr(&buf, def, dev->info, qemuCaps) < 0)
        goto error;
    if (qemuBuildRomStr(&buf, dev->info) < 0)
        goto error;

    if (virBufferCheckError(&buf) < 0)
        goto error;

    return virBufferContentAndReset(&buf);

 error:
    virBufferFreeAndReset(&buf);
    return NULL;
}


char *
qemuBuildUSBHostdevDevStr(const virDomainDef *def,
                          virDomainHostdevDefPtr dev,
                          virQEMUCapsPtr qemuCaps)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    virDomainHostdevSubsysUSBPtr usbsrc = &dev->source.subsys.u.usb;

    if (!dev->missing && !usbsrc->bus && !usbsrc->device) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("USB host device is missing bus/device information"));
        return NULL;
    }

    virBufferAddLit(&buf, "usb-host");
    if (!dev->missing) {
        virBufferAsprintf(&buf, ",hostbus=%d,hostaddr=%d",
                          usbsrc->bus, usbsrc->device);
    }
    virBufferAsprintf(&buf, ",id=%s", dev->info->alias);
    if (dev->info->bootIndex)
        virBufferAsprintf(&buf, ",bootindex=%u", dev->info->bootIndex);

    if (qemuBuildDeviceAddressStr(&buf, def, dev->info, qemuCaps) < 0)
        goto error;

    if (virBufferCheckError(&buf) < 0)
        goto error;

    return virBufferContentAndReset(&buf);

 error:
    virBufferFreeAndReset(&buf);
    return NULL;
}


static char *
qemuBuildHubDevStr(const virDomainDef *def,
                   virDomainHubDefPtr dev,
                   virQEMUCapsPtr qemuCaps)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;

    if (dev->type != VIR_DOMAIN_HUB_TYPE_USB) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("hub type %s not supported"),
                       virDomainHubTypeToString(dev->type));
        goto error;
    }

    if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_USB_HUB)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("usb-hub not supported by QEMU binary"));
        goto error;
    }

    virBufferAddLit(&buf, "usb-hub");
    virBufferAsprintf(&buf, ",id=%s", dev->info.alias);
    if (qemuBuildDeviceAddressStr(&buf, def, &dev->info, qemuCaps) < 0)
        goto error;

    if (virBufferCheckError(&buf) < 0)
        goto error;

    return virBufferContentAndReset(&buf);

 error:
    virBufferFreeAndReset(&buf);
    return NULL;
}


static int
qemuBuildHubCommandLine(virCommandPtr cmd,
                        const virDomainDef *def,
                        virQEMUCapsPtr qemuCaps)
{
    size_t i;

    for (i = 0; i < def->nhubs; i++) {
        virDomainHubDefPtr hub = def->hubs[i];
        char *optstr;

        virCommandAddArg(cmd, "-device");
        if (!(optstr = qemuBuildHubDevStr(def, hub, qemuCaps)))
            return -1;
        virCommandAddArg(cmd, optstr);
        VIR_FREE(optstr);
    }

    return 0;
}


static char *
qemuBuildSCSIHostHostdevDrvStr(virDomainHostdevDefPtr dev)
{
    virDomainHostdevSubsysSCSIPtr scsisrc = &dev->source.subsys.u.scsi;
    virDomainHostdevSubsysSCSIHostPtr scsihostsrc = &scsisrc->u.host;

    return virSCSIDeviceGetSgName(NULL,
                                  scsihostsrc->adapter,
                                  scsihostsrc->bus,
                                  scsihostsrc->target,
                                  scsihostsrc->unit);
}

static char *
qemuBuildSCSIiSCSIHostdevDrvStr(virDomainHostdevDefPtr dev,
                                virQEMUCapsPtr qemuCaps)
{
    char *ret = NULL;
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    char *netsource = NULL;
    virJSONValuePtr srcprops = NULL;
    virDomainHostdevSubsysSCSIPtr scsisrc = &dev->source.subsys.u.scsi;
    virDomainHostdevSubsysSCSIiSCSIPtr iscsisrc = &scsisrc->u.iscsi;
    qemuDomainStorageSourcePrivatePtr srcPriv =
        QEMU_DOMAIN_STORAGE_SOURCE_PRIVATE(iscsisrc->src);

    if (qemuDiskSourceNeedsProps(iscsisrc->src, qemuCaps)) {
        if (!(srcprops = qemuDiskSourceGetProps(iscsisrc->src)))
            goto cleanup;
        if (!(netsource = virQEMUBuildDriveCommandlineFromJSON(srcprops)))
            goto cleanup;
        virBufferAsprintf(&buf, "%s,if=none,format=raw", netsource);
    } else {
        /* Rather than pull what we think we want - use the network disk code */
        if (!(netsource = qemuBuildNetworkDriveStr(iscsisrc->src, srcPriv ?
                                                   srcPriv->secinfo : NULL)))
            goto cleanup;
        virBufferAddLit(&buf, "file=");
        virQEMUBuildBufferEscapeComma(&buf, netsource);
        virBufferAddLit(&buf, ",if=none,format=raw");
    }

    if (virBufferCheckError(&buf) < 0)
        goto cleanup;

    ret = virBufferContentAndReset(&buf);

 cleanup:
    VIR_FREE(netsource);
    virJSONValueFree(srcprops);
    virBufferFreeAndReset(&buf);
    return ret;
}

char *
qemuBuildSCSIVHostHostdevDevStr(const virDomainDef *def,
                           virDomainHostdevDefPtr dev,
                           virQEMUCapsPtr qemuCaps,
                           char *vhostfdName)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    virDomainHostdevSubsysSCSIVHostPtr hostsrc = &dev->source.subsys.u.scsi_host;

    if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE_VHOST_SCSI)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("This QEMU doesn't support vhost-scsi devices"));
        goto cleanup;
    }

    if (qemuBuildVirtioDevStr(&buf, "vhost-scsi", qemuCaps,
                              VIR_DOMAIN_DEVICE_HOSTDEV, dev) < 0) {
        goto cleanup;
    }

    virBufferAsprintf(&buf, ",wwpn=%s,vhostfd=%s,id=%s",
                      hostsrc->wwpn,
                      vhostfdName,
                      dev->info->alias);

    if (qemuBuildDeviceAddressStr(&buf, def, dev->info, qemuCaps) < 0)
        goto cleanup;

    if (virBufferCheckError(&buf) < 0)
        goto cleanup;

    return virBufferContentAndReset(&buf);

 cleanup:
    virBufferFreeAndReset(&buf);
    return NULL;
}

char *
qemuBuildSCSIHostdevDrvStr(virDomainHostdevDefPtr dev,
                           virQEMUCapsPtr qemuCaps)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    char *source = NULL;
    char *drivealias = NULL;
    virDomainHostdevSubsysSCSIPtr scsisrc = &dev->source.subsys.u.scsi;

    if (scsisrc->protocol == VIR_DOMAIN_HOSTDEV_SCSI_PROTOCOL_TYPE_ISCSI) {
        if (!(source = qemuBuildSCSIiSCSIHostdevDrvStr(dev, qemuCaps)))
            goto error;
        virBufferAdd(&buf, source, -1);
    } else {
        if (!(source = qemuBuildSCSIHostHostdevDrvStr(dev)))
            goto error;
        virBufferAsprintf(&buf, "file=/dev/%s,if=none,format=raw", source);
    }
    VIR_FREE(source);

    if (!(drivealias = qemuAliasFromHostdev(dev)))
        goto error;
    virBufferAsprintf(&buf, ",id=%s", drivealias);
    VIR_FREE(drivealias);

    if (dev->readonly)
        virBufferAddLit(&buf, ",readonly=on");

    if (virBufferCheckError(&buf) < 0)
        goto error;

    return virBufferContentAndReset(&buf);
 error:
    virBufferFreeAndReset(&buf);
    return NULL;
}

char *
qemuBuildSCSIHostdevDevStr(const virDomainDef *def,
                           virDomainHostdevDefPtr dev)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    int model = -1;
    char *driveAlias;
    const char *contAlias;

    model = qemuDomainFindSCSIControllerModel(def, dev->info);
    if (model < 0)
        goto error;

    if (model == VIR_DOMAIN_CONTROLLER_MODEL_SCSI_LSILOGIC) {
        if (dev->info->addr.drive.target != 0) {
           virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("target must be 0 for scsi host device "
                             "if its controller model is 'lsilogic'"));
            goto error;
        }

        if (dev->info->addr.drive.unit > 7) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("unit must be not more than 7 for scsi host "
                             "device if its controller model is 'lsilogic'"));
            goto error;
        }
    }

    virBufferAddLit(&buf, "scsi-generic");

    if (!(contAlias = virDomainControllerAliasFind(def, VIR_DOMAIN_CONTROLLER_TYPE_SCSI,
                                                   dev->info->addr.drive.controller)))
        goto error;

    if (model == VIR_DOMAIN_CONTROLLER_MODEL_SCSI_LSILOGIC) {
        virBufferAsprintf(&buf, ",bus=%s.%d,scsi-id=%d",
                          contAlias,
                          dev->info->addr.drive.bus,
                          dev->info->addr.drive.unit);
    } else {
        virBufferAsprintf(&buf, ",bus=%s.0,channel=%d,scsi-id=%d,lun=%d",
                          contAlias,
                          dev->info->addr.drive.bus,
                          dev->info->addr.drive.target,
                          dev->info->addr.drive.unit);
    }

    if (!(driveAlias = qemuAliasFromHostdev(dev)))
        goto error;
    virBufferAsprintf(&buf, ",drive=%s,id=%s", driveAlias, dev->info->alias);
    VIR_FREE(driveAlias);

    if (dev->info->bootIndex)
        virBufferAsprintf(&buf, ",bootindex=%u", dev->info->bootIndex);

    if (virBufferCheckError(&buf) < 0)
        goto error;

    return virBufferContentAndReset(&buf);
 error:
    virBufferFreeAndReset(&buf);
    return NULL;
}

static int
qemuBuildChrChardevFileStr(virLogManagerPtr logManager,
                           virCommandPtr cmd,
                           const virDomainDef *def,
                           virBufferPtr buf,
                           const char *filearg, const char *fileval,
                           const char *appendarg, int appendval)
{
    if (logManager) {
        char *fdset, *fdpath;
        int flags = 0;
        int logfd;

        if (appendval == VIR_TRISTATE_SWITCH_ABSENT ||
            appendval == VIR_TRISTATE_SWITCH_OFF)
            flags |= VIR_LOG_MANAGER_PROTOCOL_DOMAIN_OPEN_LOG_FILE_TRUNCATE;

        if ((logfd = virLogManagerDomainOpenLogFile(logManager,
                                                    "qemu",
                                                    def->uuid,
                                                    def->name,
                                                    fileval,
                                                    flags,
                                                    NULL, NULL)) < 0)
            return -1;

        virCommandPassFD(cmd, logfd, VIR_COMMAND_PASS_FD_CLOSE_PARENT);
        if (!(fdset = qemuVirCommandGetFDSet(cmd, logfd)))
            return -1;

        virCommandAddArg(cmd, "-add-fd");
        virCommandAddArg(cmd, fdset);
        VIR_FREE(fdset);

        if (!(fdpath = qemuVirCommandGetDevSet(cmd, logfd)))
            return -1;

        virBufferAsprintf(buf, ",%s=%s,%s=on", filearg, fdpath, appendarg);
        VIR_FREE(fdpath);
    } else {
        virBufferAsprintf(buf, ",%s=", filearg);
        virQEMUBuildBufferEscapeComma(buf, fileval);
        if (appendval != VIR_TRISTATE_SWITCH_ABSENT) {
            virBufferAsprintf(buf, ",%s=%s", appendarg,
                              virTristateSwitchTypeToString(appendval));
        }
    }

    return 0;
}


static void
qemuBuildChrChardevReconnectStr(virBufferPtr buf,
                                const virDomainChrSourceReconnectDef *def)
{
    if (def->enabled == VIR_TRISTATE_BOOL_YES) {
        virBufferAsprintf(buf, ",reconnect=%u", def->timeout);
    } else if (def->enabled == VIR_TRISTATE_BOOL_NO) {
        virBufferAddLit(buf, ",reconnect=0");
    }
}


int
qemuOpenChrChardevUNIXSocket(const virDomainChrSourceDef *dev)
{
    struct sockaddr_un addr;
    socklen_t addrlen = sizeof(addr);
    int fd;

    if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
        virReportSystemError(errno, "%s",
                             _("Unable to create UNIX socket"));
        goto error;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (virStrcpyStatic(addr.sun_path, dev->data.nix.path) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("UNIX socket path '%s' too long"),
                       dev->data.nix.path);
        goto error;
    }

    if (unlink(dev->data.nix.path) < 0 && errno != ENOENT) {
        virReportSystemError(errno,
                             _("Unable to unlink %s"),
                             dev->data.nix.path);
        goto error;
    }

    if (bind(fd, (struct sockaddr *)&addr, addrlen) < 0) {
        virReportSystemError(errno,
                             _("Unable to bind to UNIX socket path '%s'"),
                             dev->data.nix.path);
        goto error;
    }

    if (listen(fd, 1) < 0) {
        virReportSystemError(errno,
                             _("Unable to listen to UNIX socket path '%s'"),
                             dev->data.nix.path);
        goto error;
    }

    /* We run QEMU with umask 0002. Compensate for the umask
     * libvirtd might be running under to get the same permission
     * QEMU would have. */
    if (virFileUpdatePerm(dev->data.nix.path, 0002, 0664) < 0)
        goto error;

    return fd;

 error:
    VIR_FORCE_CLOSE(fd);
    return -1;
}


enum {
    QEMU_BUILD_CHARDEV_TCP_NOWAIT = (1 << 0),
    QEMU_BUILD_CHARDEV_FILE_LOGD  = (1 << 1),
    QEMU_BUILD_CHARDEV_UNIX_FD_PASS = (1 << 2),
};

/* This function outputs a -chardev command line option which describes only the
 * host side of the character device */
static char *
qemuBuildChrChardevStr(virLogManagerPtr logManager,
                       virSecurityManagerPtr secManager,
                       virCommandPtr cmd,
                       virQEMUDriverConfigPtr cfg,
                       const virDomainDef *def,
                       const virDomainChrSourceDef *dev,
                       const char *alias,
                       virQEMUCapsPtr qemuCaps,
                       unsigned int flags)
{
    qemuDomainChrSourcePrivatePtr chrSourcePriv = QEMU_DOMAIN_CHR_SOURCE_PRIVATE(dev);
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    bool telnet;
    char *charAlias = NULL;
    char *ret = NULL;

    if (!(charAlias = qemuAliasChardevFromDevAlias(alias)))
        goto cleanup;

    switch (dev->type) {
    case VIR_DOMAIN_CHR_TYPE_NULL:
        virBufferAsprintf(&buf, "null,id=%s", charAlias);
        break;

    case VIR_DOMAIN_CHR_TYPE_VC:
        virBufferAsprintf(&buf, "vc,id=%s", charAlias);
        break;

    case VIR_DOMAIN_CHR_TYPE_PTY:
        virBufferAsprintf(&buf, "pty,id=%s", charAlias);
        break;

    case VIR_DOMAIN_CHR_TYPE_DEV:
        virBufferAsprintf(&buf, "%s,id=%s,path=",
                          STRPREFIX(alias, "parallel") ? "parport" : "tty",
                          charAlias);
        virQEMUBuildBufferEscapeComma(&buf, dev->data.file.path);
        break;

    case VIR_DOMAIN_CHR_TYPE_FILE:
        virBufferAsprintf(&buf, "file,id=%s", charAlias);

        if (dev->data.file.append != VIR_TRISTATE_SWITCH_ABSENT &&
            !virQEMUCapsGet(qemuCaps, QEMU_CAPS_CHARDEV_FILE_APPEND)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("append not supported in this QEMU binary"));
            goto cleanup;
        }
        if (qemuBuildChrChardevFileStr(flags & QEMU_BUILD_CHARDEV_FILE_LOGD ?
                                       logManager : NULL,
                                       cmd, def, &buf,
                                       "path", dev->data.file.path,
                                       "append", dev->data.file.append) < 0)
            goto cleanup;
        break;

    case VIR_DOMAIN_CHR_TYPE_PIPE:
        virBufferAsprintf(&buf, "pipe,id=%s,path=", charAlias);
        virQEMUBuildBufferEscapeComma(&buf, dev->data.file.path);
        break;

    case VIR_DOMAIN_CHR_TYPE_STDIO:
        virBufferAsprintf(&buf, "stdio,id=%s", charAlias);
        break;

    case VIR_DOMAIN_CHR_TYPE_UDP: {
        const char *connectHost = dev->data.udp.connectHost;
        const char *bindHost = dev->data.udp.bindHost;
        const char *bindService = dev->data.udp.bindService;

        if (connectHost == NULL)
            connectHost = "";
        if (bindHost == NULL)
            bindHost = "";
        if (bindService == NULL)
            bindService = "0";

        virBufferAsprintf(&buf,
                          "udp,id=%s,host=%s,port=%s,localaddr=%s,"
                          "localport=%s",
                          charAlias,
                          connectHost,
                          dev->data.udp.connectService,
                          bindHost, bindService);
        break;
    }
    case VIR_DOMAIN_CHR_TYPE_TCP:
        telnet = dev->data.tcp.protocol == VIR_DOMAIN_CHR_TCP_PROTOCOL_TELNET;
        virBufferAsprintf(&buf,
                          "socket,id=%s,host=%s,port=%s%s",
                          charAlias,
                          dev->data.tcp.host,
                          dev->data.tcp.service,
                          telnet ? ",telnet" : "");

        if (dev->data.tcp.listen) {
            virBufferAddLit(&buf, ",server");
            if (flags & QEMU_BUILD_CHARDEV_TCP_NOWAIT)
                virBufferAddLit(&buf, ",nowait");
        }

        qemuBuildChrChardevReconnectStr(&buf, &dev->data.tcp.reconnect);

        if (dev->data.tcp.haveTLS == VIR_TRISTATE_BOOL_YES) {
            char *objalias = NULL;
            const char *tlsCertEncSecAlias = NULL;

            /* Add the secret object first if necessary. The
             * secinfo is added only to a TCP serial device during
             * qemuDomainSecretChardevPrepare. Subsequently called
             * functions can just check the config fields */
            if (chrSourcePriv && chrSourcePriv->secinfo) {
                if (qemuBuildObjectSecretCommandLine(cmd,
                                                     chrSourcePriv->secinfo) < 0)
                    goto cleanup;

                tlsCertEncSecAlias = chrSourcePriv->secinfo->s.aes.alias;
            }

            if (!(objalias = qemuAliasTLSObjFromSrcAlias(charAlias)))
                goto cleanup;

            if (qemuBuildTLSx509CommandLine(cmd, cfg->chardevTLSx509certdir,
                                            dev->data.tcp.listen,
                                            cfg->chardevTLSx509verify,
                                            tlsCertEncSecAlias,
                                            objalias, qemuCaps) < 0) {
                VIR_FREE(objalias);
                goto cleanup;
            }

            virBufferAsprintf(&buf, ",tls-creds=%s", objalias);
            VIR_FREE(objalias);
        }
        break;

    case VIR_DOMAIN_CHR_TYPE_UNIX:
        virBufferAsprintf(&buf, "socket,id=%s", charAlias);
        if (dev->data.nix.listen &&
            (flags & QEMU_BUILD_CHARDEV_UNIX_FD_PASS) &&
            virQEMUCapsGet(qemuCaps, QEMU_CAPS_CHARDEV_FD_PASS)) {
            if (qemuSecuritySetSocketLabel(secManager, (virDomainDefPtr)def) < 0)
                goto cleanup;
            int fd = qemuOpenChrChardevUNIXSocket(dev);
            if (qemuSecurityClearSocketLabel(secManager, (virDomainDefPtr)def) < 0) {
                VIR_FORCE_CLOSE(fd);
                goto cleanup;
            }
            if (fd < 0)
                goto cleanup;

            virBufferAsprintf(&buf, ",fd=%d", fd);

            virCommandPassFD(cmd, fd, VIR_COMMAND_PASS_FD_CLOSE_PARENT);
        } else {
            virBufferAddLit(&buf, ",path=");
            virQEMUBuildBufferEscapeComma(&buf, dev->data.nix.path);
        }
        if (dev->data.nix.listen) {
            virBufferAddLit(&buf, ",server");
            if (flags & QEMU_BUILD_CHARDEV_TCP_NOWAIT)
                virBufferAddLit(&buf, ",nowait");
        }

        qemuBuildChrChardevReconnectStr(&buf, &dev->data.nix.reconnect);
        break;

    case VIR_DOMAIN_CHR_TYPE_SPICEVMC:
        virBufferAsprintf(&buf, "spicevmc,id=%s,name=%s", charAlias,
                          virDomainChrSpicevmcTypeToString(dev->data.spicevmc));
        break;

    case VIR_DOMAIN_CHR_TYPE_SPICEPORT:
        virBufferAsprintf(&buf, "spiceport,id=%s,name=%s", charAlias,
                          dev->data.spiceport.channel);
        break;

    default:
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("unsupported chardev '%s'"),
                       virDomainChrTypeToString(dev->type));
        goto cleanup;
    }

    if (dev->logfile) {
        if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_CHARDEV_LOGFILE)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("logfile not supported in this QEMU binary"));
            goto cleanup;
        }
        if (qemuBuildChrChardevFileStr(logManager, cmd, def, &buf,
                                       "logfile", dev->logfile,
                                       "logappend", dev->logappend) < 0)
            goto cleanup;
    }

    if (virBufferCheckError(&buf) < 0)
        goto cleanup;

    ret = virBufferContentAndReset(&buf);
 cleanup:
    VIR_FREE(charAlias);
    virBufferFreeAndReset(&buf);
    return ret;
}

char *
qemuBuildHostdevMediatedDevStr(const virDomainDef *def,
                               virDomainHostdevDefPtr dev,
                               virQEMUCapsPtr qemuCaps)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    virDomainHostdevSubsysMediatedDevPtr mdevsrc = &dev->source.subsys.u.mdev;
    char *ret = NULL;
    char *mdevPath = NULL;
    const char *dev_str = NULL;

    if (!(mdevPath = virMediatedDeviceGetSysfsPath(mdevsrc->uuidstr)))
        goto cleanup;

    dev_str = virMediatedDeviceModelTypeToString(mdevsrc->model);

    if (!dev_str)
        goto cleanup;

    virBufferAdd(&buf, dev_str, -1);
    virBufferAsprintf(&buf, ",id=%s,sysfsdev=%s", dev->info->alias, mdevPath);

    if (mdevsrc->display != VIR_TRISTATE_SWITCH_ABSENT)
        virBufferAsprintf(&buf, ",display=%s",
                          virTristateSwitchTypeToString(mdevsrc->display));

    if (qemuBuildDeviceAddressStr(&buf, def, dev->info, qemuCaps) < 0)
        goto cleanup;

    if (virBufferCheckError(&buf) < 0)
        goto cleanup;

    ret = virBufferContentAndReset(&buf);

 cleanup:
    VIR_FREE(mdevPath);
    virBufferFreeAndReset(&buf);
    return ret;
}

static int
qemuBuildHostdevCommandLine(virCommandPtr cmd,
                            const virDomainDef *def,
                            virQEMUCapsPtr qemuCaps,
                            unsigned int *bootHostdevNet)
{
    size_t i;

    for (i = 0; i < def->nhostdevs; i++) {
        virDomainHostdevDefPtr hostdev = def->hostdevs[i];
        virDomainHostdevSubsysPtr subsys = &hostdev->source.subsys;
        char *devstr;

        if (hostdev->info->bootIndex) {
            if (hostdev->mode != VIR_DOMAIN_HOSTDEV_MODE_SUBSYS ||
                (subsys->type != VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_PCI &&
                 subsys->type != VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_USB &&
                 subsys->type != VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_SCSI)) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("booting from assigned devices is only "
                                 "supported for PCI, USB and SCSI devices"));
                return -1;
            }
        }

        /* USB */
        if (hostdev->mode == VIR_DOMAIN_HOSTDEV_MODE_SUBSYS &&
            subsys->type == VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_USB) {

            virCommandAddArg(cmd, "-device");
            if (!(devstr =
                  qemuBuildUSBHostdevDevStr(def, hostdev, qemuCaps)))
                return -1;
            virCommandAddArg(cmd, devstr);
            VIR_FREE(devstr);
        }

        /* PCI */
        if (hostdev->mode == VIR_DOMAIN_HOSTDEV_MODE_SUBSYS &&
            subsys->type == VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_PCI) {
            int backend = subsys->u.pci.backend;

            if (backend == VIR_DOMAIN_HOSTDEV_PCI_BACKEND_VFIO) {
                if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE_VFIO_PCI)) {
                    virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                                   _("VFIO PCI device assignment is not "
                                     "supported by this version of qemu"));
                    return -1;
                }
            }

            char *configfd_name = NULL;
            unsigned int bootIndex = hostdev->info->bootIndex;

            /* bootNet will be non-0 if boot order was set and no other
             * net devices were encountered
             */
            if (hostdev->parentnet && bootIndex == 0) {
                bootIndex = *bootHostdevNet;
                *bootHostdevNet = 0;
            }
            if (backend != VIR_DOMAIN_HOSTDEV_PCI_BACKEND_VFIO) {
                int configfd = qemuOpenPCIConfig(hostdev);

                if (configfd >= 0) {
                    if (virAsprintf(&configfd_name, "%d", configfd) < 0) {
                        VIR_FORCE_CLOSE(configfd);
                        return -1;
                    }

                    virCommandPassFD(cmd, configfd,
                                     VIR_COMMAND_PASS_FD_CLOSE_PARENT);
                }
            }

            if (qemuCommandAddExtDevice(cmd, hostdev->info) < 0)
                return -1;

            virCommandAddArg(cmd, "-device");
            devstr = qemuBuildPCIHostdevDevStr(def, hostdev, bootIndex,
                                               configfd_name, qemuCaps);
            VIR_FREE(configfd_name);
            if (!devstr)
                return -1;
            virCommandAddArg(cmd, devstr);
            VIR_FREE(devstr);
        }

        /* SCSI */
        if (virHostdevIsSCSIDevice(hostdev)) {
            virDomainHostdevSubsysSCSIPtr scsisrc =
                &hostdev->source.subsys.u.scsi;
            char *drvstr;

            if (scsisrc->protocol == VIR_DOMAIN_HOSTDEV_SCSI_PROTOCOL_TYPE_ISCSI) {
                virDomainHostdevSubsysSCSIiSCSIPtr iscsisrc =
                    &scsisrc->u.iscsi;
                qemuDomainStorageSourcePrivatePtr srcPriv =
                    QEMU_DOMAIN_STORAGE_SOURCE_PRIVATE(iscsisrc->src);

                if (qemuBuildDiskSecinfoCommandLine(cmd, srcPriv ?
                                                    srcPriv->secinfo :
                                                    NULL) < 0)
                    return -1;
            }

            virCommandAddArg(cmd, "-drive");
            if (!(drvstr = qemuBuildSCSIHostdevDrvStr(hostdev, qemuCaps)))
                return -1;
            virCommandAddArg(cmd, drvstr);
            VIR_FREE(drvstr);

            virCommandAddArg(cmd, "-device");
            if (!(devstr = qemuBuildSCSIHostdevDevStr(def, hostdev)))
                return -1;
            virCommandAddArg(cmd, devstr);
            VIR_FREE(devstr);
        }

        /* SCSI_host */
        if (hostdev->mode == VIR_DOMAIN_HOSTDEV_MODE_SUBSYS &&
            subsys->type == VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_SCSI_HOST) {
            if (hostdev->source.subsys.u.scsi_host.protocol ==
                VIR_DOMAIN_HOSTDEV_SUBSYS_SCSI_HOST_PROTOCOL_TYPE_VHOST) {
                char *vhostfdName = NULL;
                int vhostfd = -1;

                if (virSCSIVHostOpenVhostSCSI(&vhostfd) < 0)
                    return -1;

                if (virAsprintf(&vhostfdName, "%d", vhostfd) < 0) {
                    VIR_FORCE_CLOSE(vhostfd);
                    return -1;
                }

                virCommandPassFD(cmd, vhostfd,
                                 VIR_COMMAND_PASS_FD_CLOSE_PARENT);

                virCommandAddArg(cmd, "-device");
                if (!(devstr = qemuBuildSCSIVHostHostdevDevStr(def,
                                                               hostdev,
                                                               qemuCaps,
                                                               vhostfdName))) {
                    VIR_FREE(vhostfdName);
                    return -1;
                }
                virCommandAddArg(cmd, devstr);

                VIR_FREE(vhostfdName);
                VIR_FREE(devstr);
            }
        }

        /* MDEV */
        if (virHostdevIsMdevDevice(hostdev)) {
            virDomainHostdevSubsysMediatedDevPtr mdevsrc = &subsys->u.mdev;

            switch ((virMediatedDeviceModelType) mdevsrc->model) {
            case VIR_MDEV_MODEL_TYPE_VFIO_PCI:
                if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE_VFIO_PCI)) {
                    virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                                   _("VFIO PCI device assignment is not "
                                     "supported by this version of QEMU"));
                    return -1;
                }

                break;
            case VIR_MDEV_MODEL_TYPE_VFIO_CCW:
                if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE_VFIO_CCW)) {
                    virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                                   _("VFIO CCW device assignment is not "
                                     "supported by this version of QEMU"));
                    return -1;
                }
                break;
            case VIR_MDEV_MODEL_TYPE_VFIO_AP:
                if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE_VFIO_AP)) {
                    virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                                   _("VFIO AP device assignment is not "
                                     "supported by this version of QEMU"));
                    return -1;
                }
                break;
            case VIR_MDEV_MODEL_TYPE_LAST:
            default:
                virReportEnumRangeError(virMediatedDeviceModelType,
                                        subsys->u.mdev.model);
                return -1;
            }

            virCommandAddArg(cmd, "-device");
            if (!(devstr =
                  qemuBuildHostdevMediatedDevStr(def, hostdev, qemuCaps)))
                return -1;
            virCommandAddArg(cmd, devstr);
            VIR_FREE(devstr);
        }
    }

    return 0;
}


static int
qemuBuildMonitorCommandLine(virLogManagerPtr logManager,
                            virSecurityManagerPtr secManager,
                            virCommandPtr cmd,
                            virQEMUDriverConfigPtr cfg,
                            virDomainDefPtr def,
                            qemuDomainObjPrivatePtr priv)
{
    char *chrdev;
    unsigned int cdevflags = QEMU_BUILD_CHARDEV_TCP_NOWAIT |
        QEMU_BUILD_CHARDEV_UNIX_FD_PASS;
    if (priv->chardevStdioLogd)
        cdevflags |= QEMU_BUILD_CHARDEV_FILE_LOGD;

    if (!priv->monConfig)
        return 0;

    if (!(chrdev = qemuBuildChrChardevStr(logManager, secManager,
                                          cmd, cfg, def,
                                          priv->monConfig, "monitor",
                                          priv->qemuCaps, cdevflags)))
        return -1;
    virCommandAddArg(cmd, "-chardev");
    virCommandAddArg(cmd, chrdev);
    VIR_FREE(chrdev);

    virCommandAddArg(cmd, "-mon");
    virCommandAddArg(cmd, "chardev=charmonitor,id=monitor,mode=control");

    return 0;
}


static char *
qemuBuildVirtioSerialPortDevStr(const virDomainDef *def,
                                virDomainChrDefPtr dev)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    const char *contAlias;

    switch (dev->deviceType) {
    case VIR_DOMAIN_CHR_DEVICE_TYPE_CONSOLE:
        virBufferAddLit(&buf, "virtconsole");
        break;
    case VIR_DOMAIN_CHR_DEVICE_TYPE_CHANNEL:
        virBufferAddLit(&buf, "virtserialport");
        break;
    default:
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Cannot use virtio serial for parallel/serial devices"));
        return NULL;
    }

    if (dev->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_NONE &&
        dev->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_CCW &&
        dev->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_VIRTIO_S390) {
        /* Check it's a virtio-serial address */
        if (dev->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_VIRTIO_SERIAL) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           "%s", _("virtio serial device has invalid address type"));
            goto error;
        }

        contAlias = virDomainControllerAliasFind(def, VIR_DOMAIN_CONTROLLER_TYPE_VIRTIO_SERIAL,
                                                 dev->info.addr.vioserial.controller);
        if (!contAlias)
            goto error;

        virBufferAsprintf(&buf, ",bus=%s.%d,nr=%d", contAlias,
                          dev->info.addr.vioserial.bus,
                          dev->info.addr.vioserial.port);
    }

    if (dev->deviceType == VIR_DOMAIN_CHR_DEVICE_TYPE_CHANNEL &&
        dev->source->type == VIR_DOMAIN_CHR_TYPE_SPICEVMC &&
        dev->target.name &&
        STRNEQ(dev->target.name, "com.redhat.spice.0")) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Unsupported spicevmc target name '%s'"),
                       dev->target.name);
        goto error;
    }

    virBufferAsprintf(&buf, ",chardev=char%s,id=%s",
                      dev->info.alias, dev->info.alias);
    if (dev->deviceType == VIR_DOMAIN_CHR_DEVICE_TYPE_CHANNEL &&
        (dev->source->type == VIR_DOMAIN_CHR_TYPE_SPICEVMC ||
         dev->target.name)) {
        virBufferAsprintf(&buf, ",name=%s", dev->target.name
                          ? dev->target.name : "com.redhat.spice.0");
    }
    if (virBufferCheckError(&buf) < 0)
        goto error;

    return virBufferContentAndReset(&buf);

 error:
    virBufferFreeAndReset(&buf);
    return NULL;
}

static char *
qemuBuildSclpDevStr(virDomainChrDefPtr dev)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    if (dev->deviceType == VIR_DOMAIN_CHR_DEVICE_TYPE_CONSOLE) {
        switch (dev->targetType) {
        case VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_SCLP:
            virBufferAddLit(&buf, "sclpconsole");
            break;
        case VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_SCLPLM:
            virBufferAddLit(&buf, "sclplmconsole");
            break;
        }
    } else {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Cannot use slcp with devices other than console"));
        goto error;
    }
    virBufferAsprintf(&buf, ",chardev=char%s,id=%s",
                      dev->info.alias, dev->info.alias);
    if (virBufferCheckError(&buf) < 0)
        goto error;

    return virBufferContentAndReset(&buf);

 error:
    virBufferFreeAndReset(&buf);
    return NULL;
}


static int
qemuBuildRNGBackendChrdevStr(virLogManagerPtr logManager,
                             virSecurityManagerPtr secManager,
                             virCommandPtr cmd,
                             virQEMUDriverConfigPtr cfg,
                             const virDomainDef *def,
                             virDomainRNGDefPtr rng,
                             virQEMUCapsPtr qemuCaps,
                             char **chr,
                             bool chardevStdioLogd)
{
    unsigned int cdevflags = QEMU_BUILD_CHARDEV_TCP_NOWAIT |
        QEMU_BUILD_CHARDEV_UNIX_FD_PASS;

    *chr = NULL;

    if (chardevStdioLogd)
        cdevflags |= QEMU_BUILD_CHARDEV_FILE_LOGD;

    switch ((virDomainRNGBackend) rng->backend) {
    case VIR_DOMAIN_RNG_BACKEND_RANDOM:
    case VIR_DOMAIN_RNG_BACKEND_LAST:
        /* no chardev backend is needed */
        return 0;

    case VIR_DOMAIN_RNG_BACKEND_EGD:
        if (!(*chr = qemuBuildChrChardevStr(logManager, secManager,
                                            cmd, cfg, def,
                                            rng->source.chardev,
                                            rng->info.alias, qemuCaps,
                                            cdevflags)))
            return -1;
    }

    return 0;
}


int
qemuBuildRNGBackendProps(virDomainRNGDefPtr rng,
                         virQEMUCapsPtr qemuCaps,
                         virJSONValuePtr *props)
{
    char *objAlias = NULL;
    char *charBackendAlias = NULL;
    int ret = -1;

    if (virAsprintf(&objAlias, "obj%s", rng->info.alias) < 0)
        goto cleanup;

    switch ((virDomainRNGBackend) rng->backend) {
    case VIR_DOMAIN_RNG_BACKEND_RANDOM:
        if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_OBJECT_RNG_RANDOM)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("this qemu doesn't support the rng-random "
                             "backend"));
            goto cleanup;
        }

        if (qemuMonitorCreateObjectProps(props, "rng-random", objAlias,
                                         "s:filename", rng->source.file,
                                         NULL) < 0)
            goto cleanup;

        break;

    case VIR_DOMAIN_RNG_BACKEND_EGD:
        if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_OBJECT_RNG_EGD)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("this qemu doesn't support the rng-egd "
                             "backend"));
            goto cleanup;
        }

        if (!(charBackendAlias = qemuAliasChardevFromDevAlias(rng->info.alias)))
            goto cleanup;

        if (qemuMonitorCreateObjectProps(props, "rng-egd", objAlias,
                                         "s:chardev", charBackendAlias,
                                         NULL) < 0)
            goto cleanup;

        break;

    case VIR_DOMAIN_RNG_BACKEND_LAST:
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("unknown rng-random backend"));
        goto cleanup;
    }

    ret = 0;

 cleanup:
    VIR_FREE(objAlias);
    VIR_FREE(charBackendAlias);
    return ret;
}


char *
qemuBuildRNGDevStr(const virDomainDef *def,
                   virDomainRNGDefPtr dev,
                   virQEMUCapsPtr qemuCaps)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;

    if (!qemuDomainCheckCCWS390AddressSupport(def, &dev->info, qemuCaps,
                                              dev->source.file))
        goto error;

    if (qemuBuildVirtioDevStr(&buf, "virtio-rng", qemuCaps,
                              VIR_DOMAIN_DEVICE_RNG, dev) < 0) {
        goto error;
    }

    virBufferAsprintf(&buf, ",rng=obj%s,id=%s",
                      dev->info.alias, dev->info.alias);

    if (dev->rate > 0) {
        virBufferAsprintf(&buf, ",max-bytes=%u", dev->rate);
        if (dev->period)
            virBufferAsprintf(&buf, ",period=%u", dev->period);
        else
            virBufferAddLit(&buf, ",period=1000");
    }

    if (qemuBuildVirtioOptionsStr(&buf, dev->virtio, qemuCaps) < 0)
        goto error;

    if (qemuBuildDeviceAddressStr(&buf, def, &dev->info, qemuCaps) < 0)
        goto error;
    if (virBufferCheckError(&buf) < 0)
        goto error;

    return virBufferContentAndReset(&buf);

 error:
    virBufferFreeAndReset(&buf);
    return NULL;
}


static int
qemuBuildRNGCommandLine(virLogManagerPtr logManager,
                        virSecurityManagerPtr secManager,
                        virCommandPtr cmd,
                        virQEMUDriverConfigPtr cfg,
                        const virDomainDef *def,
                        virQEMUCapsPtr qemuCaps,
                        bool chardevStdioLogd)
{
    size_t i;

    for (i = 0; i < def->nrngs; i++) {
        virJSONValuePtr props;
        virBuffer buf = VIR_BUFFER_INITIALIZER;
        virDomainRNGDefPtr rng = def->rngs[i];
        char *tmp;
        int rc;

        if (!rng->info.alias) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("RNG device is missing alias"));
            return -1;
        }

        /* possibly add character device for backend */
        if (qemuBuildRNGBackendChrdevStr(logManager, secManager, cmd, cfg, def,
                                         rng, qemuCaps, &tmp,
                                         chardevStdioLogd) < 0)
            return -1;

        if (tmp) {
            virCommandAddArgList(cmd, "-chardev", tmp, NULL);
            VIR_FREE(tmp);
        }

        if (qemuBuildRNGBackendProps(rng, qemuCaps, &props) < 0)
            return -1;

        rc = virQEMUBuildObjectCommandlineFromJSON(&buf, props);
        virJSONValueFree(props);

        if (rc < 0)
            return -1;

        virCommandAddArg(cmd, "-object");
        virCommandAddArgBuffer(cmd, &buf);

        /* add the device */
        if (qemuCommandAddExtDevice(cmd, &rng->info) < 0)
            return -1;

        if (!(tmp = qemuBuildRNGDevStr(def, rng, qemuCaps)))
            return -1;
        virCommandAddArgList(cmd, "-device", tmp, NULL);
        VIR_FREE(tmp);
    }

    return 0;
}


static char *
qemuBuildSmbiosBiosStr(virSysinfoBIOSDefPtr def)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;

    if (!def)
        return NULL;

    virBufferAddLit(&buf, "type=0");

    /* 0:Vendor */
    if (def->vendor) {
        virBufferAddLit(&buf, ",vendor=");
        virQEMUBuildBufferEscapeComma(&buf, def->vendor);
    }
    /* 0:BIOS Version */
    if (def->version) {
        virBufferAddLit(&buf, ",version=");
        virQEMUBuildBufferEscapeComma(&buf, def->version);
    }
    /* 0:BIOS Release Date */
    if (def->date) {
        virBufferAddLit(&buf, ",date=");
        virQEMUBuildBufferEscapeComma(&buf, def->date);
    }
    /* 0:System BIOS Major Release and 0:System BIOS Minor Release */
    if (def->release) {
        virBufferAddLit(&buf, ",release=");
        virQEMUBuildBufferEscapeComma(&buf, def->release);
    }

    return virBufferContentAndReset(&buf);
}


static char *
qemuBuildSmbiosSystemStr(virSysinfoSystemDefPtr def,
                         bool skip_uuid)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;

    if (!def ||
        (!def->manufacturer && !def->product && !def->version &&
         !def->serial && (!def->uuid || skip_uuid) &&
         def->sku && !def->family))
        return NULL;

    virBufferAddLit(&buf, "type=1");

    /* 1:Manufacturer */
    if (def->manufacturer) {
        virBufferAddLit(&buf, ",manufacturer=");
        virQEMUBuildBufferEscapeComma(&buf, def->manufacturer);
    }
     /* 1:Product Name */
    if (def->product) {
        virBufferAddLit(&buf, ",product=");
        virQEMUBuildBufferEscapeComma(&buf, def->product);
    }
    /* 1:Version */
    if (def->version) {
        virBufferAddLit(&buf, ",version=");
        virQEMUBuildBufferEscapeComma(&buf, def->version);
    }
    /* 1:Serial Number */
    if (def->serial) {
        virBufferAddLit(&buf, ",serial=");
        virQEMUBuildBufferEscapeComma(&buf, def->serial);
    }
    /* 1:UUID */
    if (def->uuid && !skip_uuid) {
        virBufferAddLit(&buf, ",uuid=");
        virQEMUBuildBufferEscapeComma(&buf, def->uuid);
    }
    /* 1:SKU Number */
    if (def->sku) {
        virBufferAddLit(&buf, ",sku=");
        virQEMUBuildBufferEscapeComma(&buf, def->sku);
    }
    /* 1:Family */
    if (def->family) {
        virBufferAddLit(&buf, ",family=");
        virQEMUBuildBufferEscapeComma(&buf, def->family);
    }

    return virBufferContentAndReset(&buf);
}


static char *
qemuBuildSmbiosBaseBoardStr(virSysinfoBaseBoardDefPtr def)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;

    if (!def)
        return NULL;

    virBufferAddLit(&buf, "type=2");

    /* 2:Manufacturer */
    virBufferAddLit(&buf, ",manufacturer=");
    virQEMUBuildBufferEscapeComma(&buf, def->manufacturer);
    /* 2:Product Name */
    if (def->product) {
        virBufferAddLit(&buf, ",product=");
        virQEMUBuildBufferEscapeComma(&buf, def->product);
    }
    /* 2:Version */
    if (def->version) {
        virBufferAddLit(&buf, ",version=");
        virQEMUBuildBufferEscapeComma(&buf, def->version);
    }
    /* 2:Serial Number */
    if (def->serial) {
        virBufferAddLit(&buf, ",serial=");
        virQEMUBuildBufferEscapeComma(&buf, def->serial);
    }
    /* 2:Asset Tag */
    if (def->asset) {
        virBufferAddLit(&buf, ",asset=");
        virQEMUBuildBufferEscapeComma(&buf, def->asset);
    }
    /* 2:Location */
    if (def->location) {
        virBufferAddLit(&buf, ",location=");
        virQEMUBuildBufferEscapeComma(&buf, def->location);
    }

    if (virBufferCheckError(&buf) < 0)
        goto error;

    return virBufferContentAndReset(&buf);

 error:
    virBufferFreeAndReset(&buf);
    return NULL;
}


static char *
qemuBuildSmbiosOEMStringsStr(virSysinfoOEMStringsDefPtr def)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    size_t i;

    if (!def)
        return NULL;

    virBufferAddLit(&buf, "type=11");

    for (i = 0; i < def->nvalues; i++) {
        virBufferAddLit(&buf, ",value=");
        virQEMUBuildBufferEscapeComma(&buf, def->values[i]);
    }

    return virBufferContentAndReset(&buf);
}


static char *
qemuBuildSmbiosChassisStr(virSysinfoChassisDefPtr def)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;

    if (!def)
        return NULL;

    virBufferAddLit(&buf, "type=3");

    /* 3:Manufacturer */
    virBufferAddLit(&buf, ",manufacturer=");
    virQEMUBuildBufferEscapeComma(&buf, def->manufacturer);
    /* 3:Version */
    if (def->version) {
        virBufferAddLit(&buf, ",version=");
        virQEMUBuildBufferEscapeComma(&buf, def->version);
    }
    /* 3:Serial Number */
    if (def->serial) {
        virBufferAddLit(&buf, ",serial=");
        virQEMUBuildBufferEscapeComma(&buf, def->serial);
    }
    /* 3:Asset Tag */
    if (def->asset) {
        virBufferAddLit(&buf, ",asset=");
        virQEMUBuildBufferEscapeComma(&buf, def->asset);
    }
    /* 3:Sku */
    if (def->sku) {
        virBufferAddLit(&buf, ",sku=");
        virQEMUBuildBufferEscapeComma(&buf, def->sku);
    }

    if (virBufferCheckError(&buf) < 0)
        goto error;

    return virBufferContentAndReset(&buf);

 error:
    virBufferFreeAndReset(&buf);
    return NULL;
}


static int
qemuBuildSmbiosCommandLine(virCommandPtr cmd,
                           virQEMUDriverPtr driver,
                           const virDomainDef *def)
{
    size_t i;
    virSysinfoDefPtr source = NULL;
    bool skip_uuid = false;

    if (def->os.smbios_mode == VIR_DOMAIN_SMBIOS_NONE ||
        def->os.smbios_mode == VIR_DOMAIN_SMBIOS_EMULATE)
        return 0;

    /* should we really error out or just warn in those cases ? */
    if (def->os.smbios_mode == VIR_DOMAIN_SMBIOS_HOST) {
        if (driver->hostsysinfo == NULL) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("Host SMBIOS information is not available"));
            return -1;
        }
        source = driver->hostsysinfo;
        /* Host and guest uuid must differ, by definition of UUID. */
        skip_uuid = true;
    } else if (def->os.smbios_mode == VIR_DOMAIN_SMBIOS_SYSINFO) {
        if (def->sysinfo == NULL) {
            virReportError(VIR_ERR_XML_ERROR,
                           _("Domain '%s' sysinfo are not available"),
                           def->name);
            return -1;
        }
        source = def->sysinfo;
        /* domain_conf guaranteed that system_uuid matches guest uuid. */
    }
    if (source != NULL) {
        char *smbioscmd;

        smbioscmd = qemuBuildSmbiosBiosStr(source->bios);
        if (smbioscmd != NULL) {
            virCommandAddArgList(cmd, "-smbios", smbioscmd, NULL);
            VIR_FREE(smbioscmd);
        }
        smbioscmd = qemuBuildSmbiosSystemStr(source->system, skip_uuid);
        if (smbioscmd != NULL) {
            virCommandAddArgList(cmd, "-smbios", smbioscmd, NULL);
            VIR_FREE(smbioscmd);
        }

        if (source->nbaseBoard > 1) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("qemu does not support more than "
                             "one entry to Type 2 in SMBIOS table"));
            return -1;
        }

        for (i = 0; i < source->nbaseBoard; i++) {
            if (!(smbioscmd =
                  qemuBuildSmbiosBaseBoardStr(source->baseBoard + i)))
                return -1;

            virCommandAddArgList(cmd, "-smbios", smbioscmd, NULL);
            VIR_FREE(smbioscmd);
        }

        smbioscmd = qemuBuildSmbiosChassisStr(source->chassis);
        if (smbioscmd != NULL) {
            virCommandAddArgList(cmd, "-smbios", smbioscmd, NULL);
            VIR_FREE(smbioscmd);
        }

        if (source->oemStrings) {
            if (!(smbioscmd = qemuBuildSmbiosOEMStringsStr(source->oemStrings)))
                return -1;

            virCommandAddArgList(cmd, "-smbios", smbioscmd, NULL);
            VIR_FREE(smbioscmd);
        }
    }

    return 0;
}


static int
qemuBuildVMGenIDCommandLine(virCommandPtr cmd,
                            const virDomainDef *def,
                            virQEMUCapsPtr qemuCaps)
{
    virBuffer opts = VIR_BUFFER_INITIALIZER;
    char guid[VIR_UUID_STRING_BUFLEN];

    if (!def->genidRequested)
        return 0;

    if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE_VMGENID)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                      _("this QEMU does not support the 'genid' capability"));
        return -1;
    }

    virUUIDFormat(def->genid, guid);
    virBufferAsprintf(&opts, "vmgenid,guid=%s,id=vmgenid0", guid);

    virCommandAddArg(cmd, "-device");
    virCommandAddArgBuffer(cmd, &opts);

    virBufferFreeAndReset(&opts);
    return 0;
}


static int
qemuBuildSgaCommandLine(virCommandPtr cmd,
                        const virDomainDef *def,
                        virQEMUCapsPtr qemuCaps)
{
    /* Serial graphics adapter */
    if (def->os.bios.useserial == VIR_TRISTATE_BOOL_YES) {
        if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_SGA)) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("qemu does not support SGA"));
            return -1;
        }
        if (!def->nserials) {
            virReportError(VIR_ERR_XML_ERROR, "%s",
                           _("need at least one serial port to use SGA"));
            return -1;
        }
        virCommandAddArgList(cmd, "-device", "sga", NULL);
    }

    return 0;
}


static char *
qemuBuildClockArgStr(virDomainClockDefPtr def)
{
    size_t i;
    virBuffer buf = VIR_BUFFER_INITIALIZER;

    switch (def->offset) {
    case VIR_DOMAIN_CLOCK_OFFSET_UTC:
        virBufferAddLit(&buf, "base=utc");
        break;

    case VIR_DOMAIN_CLOCK_OFFSET_LOCALTIME:
    case VIR_DOMAIN_CLOCK_OFFSET_TIMEZONE:
        virBufferAddLit(&buf, "base=localtime");
        break;

    case VIR_DOMAIN_CLOCK_OFFSET_VARIABLE: {
        time_t now = time(NULL);
        struct tm nowbits;

        if (def->data.variable.basis == VIR_DOMAIN_CLOCK_BASIS_LOCALTIME) {
            long localOffset;

            /* in the case of basis='localtime', rather than trying to
             * keep that basis (and associated offset from UTC) in the
             * status and deal with adding in the difference each time
             * there is an RTC_CHANGE event, it is simpler and less
             * error prone to just convert the adjustment an offset
             * from UTC right now (and change the status to
             * "basis='utc' to reflect this). This eliminates
             * potential errors in both RTC_CHANGE events and in
             * migration (in the case that the status of DST, or the
             * timezone of the destination host, changed relative to
             * startup).
             */
            if (virTimeLocalOffsetFromUTC(&localOffset) < 0)
               goto error;
            def->data.variable.adjustment += localOffset;
            def->data.variable.basis = VIR_DOMAIN_CLOCK_BASIS_UTC;
        }

        now += def->data.variable.adjustment;
        gmtime_r(&now, &nowbits);

        /* when an RTC_CHANGE event is received from qemu, we need to
         * have the adjustment used at domain start time available to
         * compute the new offset from UTC. As this new value is
         * itself stored in def->data.variable.adjustment, we need to
         * save a copy of it now.
        */
        def->data.variable.adjustment0 = def->data.variable.adjustment;

        virBufferAsprintf(&buf, "base=%d-%02d-%02dT%02d:%02d:%02d",
                          nowbits.tm_year + 1900,
                          nowbits.tm_mon + 1,
                          nowbits.tm_mday,
                          nowbits.tm_hour,
                          nowbits.tm_min,
                          nowbits.tm_sec);
    }   break;

    default:
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("unsupported clock offset '%s'"),
                       virDomainClockOffsetTypeToString(def->offset));
        goto error;
    }

    /* Look for an 'rtc' timer element, and add in appropriate
     * clock= and driftfix= */
    for (i = 0; i < def->ntimers; i++) {
        if (def->timers[i]->name == VIR_DOMAIN_TIMER_NAME_RTC) {
            switch (def->timers[i]->track) {
            case -1: /* unspecified - use hypervisor default */
                break;
            case VIR_DOMAIN_TIMER_TRACK_BOOT:
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                               _("unsupported rtc timer track '%s'"),
                               virDomainTimerTrackTypeToString(def->timers[i]->track));
                goto error;
            case VIR_DOMAIN_TIMER_TRACK_GUEST:
                virBufferAddLit(&buf, ",clock=vm");
                break;
            case VIR_DOMAIN_TIMER_TRACK_WALL:
                virBufferAddLit(&buf, ",clock=host");
                break;
            }

            switch (def->timers[i]->tickpolicy) {
            case -1:
            case VIR_DOMAIN_TIMER_TICKPOLICY_DELAY:
                /* This is the default - missed ticks delivered when
                   next scheduled, at normal rate */
                break;
            case VIR_DOMAIN_TIMER_TICKPOLICY_CATCHUP:
                /* deliver ticks at a faster rate until caught up */
                virBufferAddLit(&buf, ",driftfix=slew");
                break;
            case VIR_DOMAIN_TIMER_TICKPOLICY_MERGE:
            case VIR_DOMAIN_TIMER_TICKPOLICY_DISCARD:
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                               _("unsupported rtc timer tickpolicy '%s'"),
                               virDomainTimerTickpolicyTypeToString(def->timers[i]->tickpolicy));
                goto error;
            }
            break; /* no need to check other timers - there is only one rtc */
        }
    }

    if (virBufferCheckError(&buf) < 0)
        goto error;

    return virBufferContentAndReset(&buf);

 error:
    virBufferFreeAndReset(&buf);
    return NULL;
}


/* NOTE: Building of commands can change def->clock->data.* values, so
 *       virDomainDef is not const here.
 */
static int
qemuBuildClockCommandLine(virCommandPtr cmd,
                          virDomainDefPtr def,
                          virQEMUCapsPtr qemuCaps)
{
    size_t i;
    char *rtcopt;

    virCommandAddArg(cmd, "-rtc");
    if (!(rtcopt = qemuBuildClockArgStr(&def->clock)))
        return -1;
    virCommandAddArg(cmd, rtcopt);
    VIR_FREE(rtcopt);

    if (def->clock.offset == VIR_DOMAIN_CLOCK_OFFSET_TIMEZONE &&
        def->clock.data.timezone) {
        virCommandAddEnvPair(cmd, "TZ", def->clock.data.timezone);
    }

    for (i = 0; i < def->clock.ntimers; i++) {
        switch ((virDomainTimerNameType)def->clock.timers[i]->name) {
        case VIR_DOMAIN_TIMER_NAME_PLATFORM:
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("unsupported timer type (name) '%s'"),
                           virDomainTimerNameTypeToString(def->clock.timers[i]->name));
            return -1;

        case VIR_DOMAIN_TIMER_NAME_TSC:
        case VIR_DOMAIN_TIMER_NAME_KVMCLOCK:
        case VIR_DOMAIN_TIMER_NAME_HYPERVCLOCK:
            /* Timers above are handled when building -cpu.  */
        case VIR_DOMAIN_TIMER_NAME_LAST:
            break;

        case VIR_DOMAIN_TIMER_NAME_RTC:
            /* Already handled in qemuBuildClockArgStr */
            break;

        case VIR_DOMAIN_TIMER_NAME_PIT:
            switch (def->clock.timers[i]->tickpolicy) {
            case -1:
            case VIR_DOMAIN_TIMER_TICKPOLICY_DELAY:
                /* delay is the default if we don't have kernel
                   (kvm-pit), otherwise, the default is catchup. */
                if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_KVM_PIT_TICK_POLICY))
                    virCommandAddArgList(cmd, "-global",
                                         "kvm-pit.lost_tick_policy=delay", NULL);
                break;
            case VIR_DOMAIN_TIMER_TICKPOLICY_CATCHUP:
                if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_KVM_PIT_TICK_POLICY)) {
                    /* do nothing - this is default for kvm-pit */
                } else {
                    /* can't catchup if we don't have kvm-pit */
                    virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                                   _("unsupported pit tickpolicy '%s'"),
                                   virDomainTimerTickpolicyTypeToString(def->clock.timers[i]->tickpolicy));
                    return -1;
                }
                break;
            case VIR_DOMAIN_TIMER_TICKPOLICY_DISCARD:
                if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_KVM_PIT_TICK_POLICY))
                    virCommandAddArgList(cmd, "-global",
                                         "kvm-pit.lost_tick_policy=discard", NULL);
                break;
            case VIR_DOMAIN_TIMER_TICKPOLICY_MERGE:
                /* no way to support this mode for pit in qemu */
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                               _("unsupported pit tickpolicy '%s'"),
                               virDomainTimerTickpolicyTypeToString(def->clock.timers[i]->tickpolicy));
                return -1;
            }
            break;

        case VIR_DOMAIN_TIMER_NAME_HPET:
            /* the only meaningful attribute for hpet is "present". If
             * present is -1, that means it wasn't specified, and
             * should be left at the default for the
             * hypervisor. "default" when -no-hpet exists is "yes",
             * and when -no-hpet doesn't exist is "no". "confusing"?
             * "yes"! */

            if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_NO_HPET)) {
                if (def->clock.timers[i]->present == 0)
                    virCommandAddArg(cmd, "-no-hpet");
            } else {
                /* no hpet timer available. The only possible action
                   is to raise an error if present="yes" */
                if (def->clock.timers[i]->present == 1) {
                    virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                                   "%s", _("hpet timer is not supported"));
                    return -1;
                }
            }
            break;
        }
    }

    return 0;
}


static int
qemuBuildPMCommandLine(virCommandPtr cmd,
                       const virDomainDef *def,
                       qemuDomainObjPrivatePtr priv)
{
    virQEMUCapsPtr qemuCaps = priv->qemuCaps;

    /* Only add -no-reboot option if each event destroys domain */
    if (priv->allowReboot == VIR_TRISTATE_BOOL_NO)
        virCommandAddArg(cmd, "-no-reboot");

    if (qemuDomainIsUsingNoShutdown(priv))
        virCommandAddArg(cmd, "-no-shutdown");

    if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_NO_ACPI)) {
        if (def->features[VIR_DOMAIN_FEATURE_ACPI] != VIR_TRISTATE_SWITCH_ON)
            virCommandAddArg(cmd, "-no-acpi");
    }

    /* We fall back to PIIX4_PM even for q35, since it's what we did
       pre-q35-pm support. QEMU starts up fine (with a warning) if
       mixing PIIX PM and -M q35. Starting to reject things here
       could mean we refuse to start existing configs in the wild.*/
    if (def->pm.s3) {
        const char *pm_object = "PIIX4_PM";

        if (qemuDomainIsQ35(def) &&
            virQEMUCapsGet(qemuCaps, QEMU_CAPS_ICH9_DISABLE_S3)) {
            pm_object = "ICH9-LPC";
        } else if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_PIIX_DISABLE_S3)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           "%s", _("setting ACPI S3 not supported"));
            return -1;
        }

        virCommandAddArg(cmd, "-global");
        virCommandAddArgFormat(cmd, "%s.disable_s3=%d",
                               pm_object, def->pm.s3 == VIR_TRISTATE_BOOL_NO);
    }

    if (def->pm.s4) {
        const char *pm_object = "PIIX4_PM";

        if (qemuDomainIsQ35(def) &&
            virQEMUCapsGet(qemuCaps, QEMU_CAPS_ICH9_DISABLE_S4)) {
            pm_object = "ICH9-LPC";
        } else if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_PIIX_DISABLE_S4)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           "%s", _("setting ACPI S4 not supported"));
            return -1;
        }

        virCommandAddArg(cmd, "-global");
        virCommandAddArgFormat(cmd, "%s.disable_s4=%d",
                               pm_object, def->pm.s4 == VIR_TRISTATE_BOOL_NO);
    }

    return 0;
}


static int
qemuBuildBootCommandLine(virCommandPtr cmd,
                         const virDomainDef *def,
                         virQEMUCapsPtr qemuCaps)
{
    virBuffer boot_buf = VIR_BUFFER_INITIALIZER;
    char *boot_opts_str = NULL;

    if (def->os.bootmenu) {
        if (def->os.bootmenu == VIR_TRISTATE_BOOL_YES)
            virBufferAddLit(&boot_buf, "menu=on,");
        else
            virBufferAddLit(&boot_buf, "menu=off,");
    }

    if (def->os.bios.rt_set) {
        if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_REBOOT_TIMEOUT)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("reboot timeout is not supported "
                             "by this QEMU binary"));
            goto error;
        }

        virBufferAsprintf(&boot_buf,
                          "reboot-timeout=%d,",
                          def->os.bios.rt_delay);
    }

    if (def->os.bm_timeout_set) {
        if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_SPLASH_TIMEOUT)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("splash timeout is not supported "
                             "by this QEMU binary"));
            goto error;
        }

        virBufferAsprintf(&boot_buf, "splash-time=%u,", def->os.bm_timeout);
    }

    if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_BOOT_STRICT))
        virBufferAddLit(&boot_buf, "strict=on,");

    virBufferTrim(&boot_buf, ",", -1);

    if (virBufferCheckError(&boot_buf) < 0)
        goto error;

    boot_opts_str = virBufferContentAndReset(&boot_buf);
    if (boot_opts_str) {
        virCommandAddArg(cmd, "-boot");
        virCommandAddArg(cmd, boot_opts_str);
    }
    VIR_FREE(boot_opts_str);

    if (def->os.kernel)
        virCommandAddArgList(cmd, "-kernel", def->os.kernel, NULL);
    if (def->os.initrd)
        virCommandAddArgList(cmd, "-initrd", def->os.initrd, NULL);
    if (def->os.cmdline)
        virCommandAddArgList(cmd, "-append", def->os.cmdline, NULL);
    if (def->os.dtb)
        virCommandAddArgList(cmd, "-dtb", def->os.dtb, NULL);
    if (def->os.slic_table) {
        virBuffer buf = VIR_BUFFER_INITIALIZER;
        virCommandAddArg(cmd, "-acpitable");
        virBufferAddLit(&buf, "sig=SLIC,file=");
        virQEMUBuildBufferEscapeComma(&buf, def->os.slic_table);
        virCommandAddArgBuffer(cmd, &buf);
    }

    return 0;

 error:
    VIR_FREE(boot_opts_str);
    virBufferFreeAndReset(&boot_buf);
    return -1;
}


static int
qemuBuildIOMMUCommandLine(virCommandPtr cmd,
                          const virDomainDef *def,
                          virQEMUCapsPtr qemuCaps)
{
    const virDomainIOMMUDef *iommu = def->iommu;

    if (!iommu)
        return 0;

    switch (iommu->model) {
    case VIR_DOMAIN_IOMMU_MODEL_INTEL: {
        VIR_AUTOCLEAN(virBuffer) opts = VIR_BUFFER_INITIALIZER;

        /* qemuDomainDeviceDefValidateIOMMU() already made sure we have
         * one of QEMU_CAPS_DEVICE_INTEL_IOMMU or QEMU_CAPS_MACHINE_IOMMU:
         * here we handle the former case, while the latter is taken care
         * of in qemuBuildMachineCommandLine() */
        if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE_INTEL_IOMMU))
            return 0;

        virBufferAddLit(&opts, "intel-iommu");
        if (iommu->intremap != VIR_TRISTATE_SWITCH_ABSENT) {
            virBufferAsprintf(&opts, ",intremap=%s",
                              virTristateSwitchTypeToString(iommu->intremap));
        }
        if (iommu->caching_mode != VIR_TRISTATE_SWITCH_ABSENT) {
            virBufferAsprintf(&opts, ",caching-mode=%s",
                              virTristateSwitchTypeToString(iommu->caching_mode));
        }
        if (iommu->eim != VIR_TRISTATE_SWITCH_ABSENT) {
            virBufferAsprintf(&opts, ",eim=%s",
                              virTristateSwitchTypeToString(iommu->eim));
        }
        if (iommu->iotlb != VIR_TRISTATE_SWITCH_ABSENT) {
            virBufferAsprintf(&opts, ",device-iotlb=%s",
                              virTristateSwitchTypeToString(iommu->iotlb));
        }

        virCommandAddArg(cmd, "-device");
        virCommandAddArgBuffer(cmd, &opts);
        break;
    }

    case VIR_DOMAIN_IOMMU_MODEL_SMMUV3:
        /* There is no -device for SMMUv3, so nothing to be done here */
        return 0;

    case VIR_DOMAIN_IOMMU_MODEL_LAST:
    default:
        virReportEnumRangeError(virDomainIOMMUModel, iommu->model);
        return -1;
    }

    return 0;
}


static int
qemuBuildGlobalControllerCommandLine(virCommandPtr cmd,
                                     const virDomainDef *def,
                                     virQEMUCapsPtr qemuCaps)
{
    size_t i;

    for (i = 0; i < def->ncontrollers; i++) {
        virDomainControllerDefPtr cont = def->controllers[i];
        if (cont->type == VIR_DOMAIN_CONTROLLER_TYPE_PCI &&
            cont->opts.pciopts.pcihole64) {
            const char *hoststr = NULL;
            bool cap = false;
            bool machine = false;

            switch (cont->model) {
            case VIR_DOMAIN_CONTROLLER_MODEL_PCI_ROOT:
                hoststr = "i440FX-pcihost";
                cap = virQEMUCapsGet(qemuCaps, QEMU_CAPS_I440FX_PCI_HOLE64_SIZE);
                machine = qemuDomainIsI440FX(def);
                break;

            case VIR_DOMAIN_CONTROLLER_MODEL_PCIE_ROOT:
                hoststr = "q35-pcihost";
                cap = virQEMUCapsGet(qemuCaps, QEMU_CAPS_Q35_PCI_HOLE64_SIZE);
                machine = qemuDomainIsQ35(def);
                break;

            default:
                virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                               _("64-bit PCI hole setting is only for root"
                                 " PCI controllers"));
                return -1;
            }

            if (!machine) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                             _("Setting the 64-bit PCI hole size is not "
                             "supported for machine '%s'"), def->os.machine);
                return -1;
            }
            if (!cap) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("64-bit PCI hole size setting is not supported "
                                 "with this QEMU binary"));
                return -1;
            }

            virCommandAddArg(cmd, "-global");
            virCommandAddArgFormat(cmd, "%s.pci-hole64-size=%luK", hoststr,
                                   cont->opts.pciopts.pcihole64size);
        }
    }

    return 0;
}


static void
qemuBuildCpuFeature(virQEMUCapsPtr qemuCaps,
                    virBufferPtr buf,
                    const char *name,
                    bool state)
{
    name = virQEMUCapsCPUFeatureToQEMU(qemuCaps, name);

    if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_QUERY_CPU_MODEL_EXPANSION))
        virBufferAsprintf(buf, ",%s=%s", name, state ? "on" : "off");
    else
        virBufferAsprintf(buf, ",%c%s", state ? '+' : '-', name);
}


static int
qemuBuildCpuModelArgStr(virQEMUDriverPtr driver,
                        const virDomainDef *def,
                        virBufferPtr buf,
                        virQEMUCapsPtr qemuCaps)
{
    int ret = -1;
    size_t i;
    virCapsPtr caps = NULL;
    virCPUDefPtr cpu = def->cpu;

    if (!(caps = virQEMUDriverGetCapabilities(driver, false)))
        goto cleanup;

    switch ((virCPUMode) cpu->mode) {
    case VIR_CPU_MODE_HOST_PASSTHROUGH:
        virBufferAddLit(buf, "host");

        if (def->os.arch == VIR_ARCH_ARMV7L &&
            caps->host.arch == VIR_ARCH_AARCH64) {
            if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_CPU_AARCH64_OFF)) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                               _("QEMU binary does not support CPU "
                                 "host-passthrough for armv7l on "
                                 "aarch64 host"));
                goto cleanup;
            }
            virBufferAddLit(buf, ",aarch64=off");
        }
        break;

    case VIR_CPU_MODE_HOST_MODEL:
        if (ARCH_IS_PPC64(def->os.arch)) {
            virBufferAddLit(buf, "host");
            if (cpu->model &&
                !(qemuDomainIsPSeries(def) &&
                  virQEMUCapsGet(qemuCaps, QEMU_CAPS_MACHINE_PSERIES_MAX_CPU_COMPAT))) {
                virBufferAsprintf(buf, ",compat=%s", cpu->model);
            }
        } else {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("unexpected host-model CPU for %s architecture"),
                           virArchToString(def->os.arch));
            goto cleanup;
        }
        break;

    case VIR_CPU_MODE_CUSTOM:
        virBufferAdd(buf, cpu->model, -1);
        break;

    case VIR_CPU_MODE_LAST:
        break;
    }

    if (ARCH_IS_S390(def->os.arch) && cpu->features &&
        !virQEMUCapsGet(qemuCaps, QEMU_CAPS_QUERY_CPU_MODEL_EXPANSION)) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("CPU features not supported by hypervisor for %s "
                         "architecture"), virArchToString(def->os.arch));
        goto cleanup;
    }

    if (cpu->vendor_id)
        virBufferAsprintf(buf, ",vendor=%s", cpu->vendor_id);

    for (i = 0; i < cpu->nfeatures; i++) {
        switch ((virCPUFeaturePolicy) cpu->features[i].policy) {
        case VIR_CPU_FEATURE_FORCE:
        case VIR_CPU_FEATURE_REQUIRE:
            qemuBuildCpuFeature(qemuCaps, buf, cpu->features[i].name, true);
            break;

        case VIR_CPU_FEATURE_DISABLE:
        case VIR_CPU_FEATURE_FORBID:
            qemuBuildCpuFeature(qemuCaps, buf, cpu->features[i].name, false);
            break;

        case VIR_CPU_FEATURE_OPTIONAL:
        case VIR_CPU_FEATURE_LAST:
            break;
        }
    }

    ret = 0;
 cleanup:
    virObjectUnref(caps);
    return ret;
}

static int
qemuBuildCpuCommandLine(virCommandPtr cmd,
                        virQEMUDriverPtr driver,
                        const virDomainDef *def,
                        virQEMUCapsPtr qemuCaps)
{
    virArch hostarch = virArchFromHost();
    char *cpu = NULL, *cpu_flags = NULL;
    int ret = -1;
    virBuffer cpu_buf = VIR_BUFFER_INITIALIZER;
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    size_t i;

    if (def->cpu &&
        (def->cpu->mode != VIR_CPU_MODE_CUSTOM || def->cpu->model)) {
        if (qemuBuildCpuModelArgStr(driver, def, &cpu_buf, qemuCaps) < 0)
            goto cleanup;
    } else {
        /*
         * Need to force a 32-bit guest CPU type if
         *
         *  1. guest OS is i686
         *  2. host OS is x86_64
         *  3. emulator is qemu-kvm or kvm
         *
         * Or
         *
         *  1. guest OS is i686
         *  2. emulator is qemu-system-x86_64
         */
        if (def->os.arch == VIR_ARCH_I686 &&
            ((hostarch == VIR_ARCH_X86_64 &&
              strstr(def->emulator, "kvm")) ||
             strstr(def->emulator, "x86_64"))) {
            virBufferAddLit(&cpu_buf, "qemu32");
        }
    }

    /* Handle paravirtual timers  */
    for (i = 0; i < def->clock.ntimers; i++) {
        virDomainTimerDefPtr timer = def->clock.timers[i];

        if (timer->name == VIR_DOMAIN_TIMER_NAME_KVMCLOCK &&
            timer->present != -1) {
            qemuBuildCpuFeature(qemuCaps, &buf, "kvmclock",
                                !!timer->present);
        } else if (timer->name == VIR_DOMAIN_TIMER_NAME_HYPERVCLOCK &&
                   timer->present == 1) {
            virBufferAddLit(&buf, ",hv_time");
        } else if (timer->name == VIR_DOMAIN_TIMER_NAME_TSC &&
                   timer->frequency > 0) {
            virBufferAsprintf(&buf, ",tsc-frequency=%lu", timer->frequency);
        }
    }

    if (def->apic_eoi) {
        qemuBuildCpuFeature(qemuCaps, &buf, "kvm_pv_eoi",
                            def->apic_eoi == VIR_TRISTATE_SWITCH_ON);
    }

    if (def->features[VIR_DOMAIN_FEATURE_PVSPINLOCK]) {
        qemuBuildCpuFeature(qemuCaps, &buf, "kvm_pv_unhalt",
                            def->features[VIR_DOMAIN_FEATURE_PVSPINLOCK] == VIR_TRISTATE_SWITCH_ON);
    }

    if (def->features[VIR_DOMAIN_FEATURE_HYPERV] == VIR_TRISTATE_SWITCH_ON) {
        for (i = 0; i < VIR_DOMAIN_HYPERV_LAST; i++) {
            switch ((virDomainHyperv) i) {
            case VIR_DOMAIN_HYPERV_RELAXED:
            case VIR_DOMAIN_HYPERV_VAPIC:
            case VIR_DOMAIN_HYPERV_VPINDEX:
            case VIR_DOMAIN_HYPERV_RUNTIME:
            case VIR_DOMAIN_HYPERV_SYNIC:
            case VIR_DOMAIN_HYPERV_STIMER:
            case VIR_DOMAIN_HYPERV_RESET:
            case VIR_DOMAIN_HYPERV_FREQUENCIES:
            case VIR_DOMAIN_HYPERV_REENLIGHTENMENT:
            case VIR_DOMAIN_HYPERV_TLBFLUSH:
            case VIR_DOMAIN_HYPERV_IPI:
            case VIR_DOMAIN_HYPERV_EVMCS:
                if (def->hyperv_features[i] == VIR_TRISTATE_SWITCH_ON)
                    virBufferAsprintf(&buf, ",hv_%s",
                                      virDomainHypervTypeToString(i));
                break;

            case VIR_DOMAIN_HYPERV_SPINLOCKS:
                if (def->hyperv_features[i] == VIR_TRISTATE_SWITCH_ON)
                    virBufferAsprintf(&buf, ",hv_spinlocks=0x%x",
                                      def->hyperv_spinlocks);
                break;

            case VIR_DOMAIN_HYPERV_VENDOR_ID:
                if (def->hyperv_features[i] == VIR_TRISTATE_SWITCH_ON)
                    virBufferAsprintf(&buf, ",hv_vendor_id=%s",
                                      def->hyperv_vendor_id);
                break;

            /* coverity[dead_error_begin] */
            case VIR_DOMAIN_HYPERV_LAST:
                break;
            }
        }
    }

    for (i = 0; i < def->npanics; i++) {
        if (def->panics[i]->model == VIR_DOMAIN_PANIC_MODEL_HYPERV) {
            virBufferAddLit(&buf, ",hv_crash");
            break;
        }
    }

    if (def->features[VIR_DOMAIN_FEATURE_KVM] == VIR_TRISTATE_SWITCH_ON) {
        for (i = 0; i < VIR_DOMAIN_KVM_LAST; i++) {
            switch ((virDomainKVM) i) {
            case VIR_DOMAIN_KVM_HIDDEN:
                if (def->kvm_features[i] == VIR_TRISTATE_SWITCH_ON)
                    virBufferAddLit(&buf, ",kvm=off");
                break;

            /* coverity[dead_error_begin] */
            case VIR_DOMAIN_KVM_LAST:
                break;
            }
        }
    }

    if (def->features[VIR_DOMAIN_FEATURE_PMU]) {
        virTristateSwitch pmu = def->features[VIR_DOMAIN_FEATURE_PMU];
        virBufferAsprintf(&buf, ",pmu=%s",
                          virTristateSwitchTypeToString(pmu));
    }

    if (def->cpu && def->cpu->cache) {
        virCPUCacheDefPtr cache = def->cpu->cache;
        bool hostOff = false;
        bool l3Off = false;

        switch (cache->mode) {
        case VIR_CPU_CACHE_MODE_EMULATE:
            virBufferAddLit(&buf, ",l3-cache=on");
            hostOff = true;
            break;

        case VIR_CPU_CACHE_MODE_PASSTHROUGH:
            virBufferAddLit(&buf, ",host-cache-info=on");
            l3Off = true;
            break;

        case VIR_CPU_CACHE_MODE_DISABLE:
            hostOff = l3Off = true;
            break;

        case VIR_CPU_CACHE_MODE_LAST:
            break;
        }

        if (hostOff &&
            def->cpu->mode == VIR_CPU_MODE_HOST_PASSTHROUGH &&
            virQEMUCapsGet(qemuCaps, QEMU_CAPS_CPU_CACHE))
            virBufferAddLit(&buf, ",host-cache-info=off");

        if (l3Off &&
            virQEMUCapsGet(qemuCaps, QEMU_CAPS_CPU_CACHE))
            virBufferAddLit(&buf, ",l3-cache=off");
    }

    if (virBufferCheckError(&cpu_buf) < 0)
        goto cleanup;
    if (virBufferCheckError(&buf) < 0)
        goto cleanup;

    cpu = virBufferContentAndReset(&cpu_buf);
    cpu_flags = virBufferContentAndReset(&buf);

    if (cpu_flags && !cpu) {
        const char *default_model;

        switch ((int)def->os.arch) {
        case VIR_ARCH_I686:
            default_model = "qemu32";
            break;
        case VIR_ARCH_X86_64:
            default_model = "qemu64";
            break;
        default:
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("CPU flags requested but can't determine "
                             "default CPU for arch %s"),
                           virArchToString(def->os.arch));
            goto cleanup;
        }

        if (VIR_STRDUP(cpu, default_model) < 0)
            goto cleanup;
    }

    if (cpu) {
        virCommandAddArg(cmd, "-cpu");
        virCommandAddArgFormat(cmd, "%s%s", cpu, NULLSTR_EMPTY(cpu_flags));
    }

    ret = 0;

 cleanup:
    VIR_FREE(cpu);
    VIR_FREE(cpu_flags);
    virBufferFreeAndReset(&buf);
    virBufferFreeAndReset(&cpu_buf);
    return ret;
}


static bool
qemuAppendKeyWrapMachineParm(virBuffer *buf, virQEMUCapsPtr qemuCaps,
                             int flag, const char *pname, int pstate)
{
    if (pstate != VIR_TRISTATE_SWITCH_ABSENT) {
        if (!virQEMUCapsGet(qemuCaps, flag)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("%s is not available with this QEMU binary"), pname);
            return false;
        }

        virBufferAsprintf(buf, ",%s=%s", pname,
                          virTristateSwitchTypeToString(pstate));
    }

    return true;
}

static bool
qemuAppendKeyWrapMachineParms(virBuffer *buf, virQEMUCapsPtr qemuCaps,
                              const virDomainKeyWrapDef *keywrap)
{
    if (!qemuAppendKeyWrapMachineParm(buf, qemuCaps, QEMU_CAPS_AES_KEY_WRAP,
                                      "aes-key-wrap", keywrap->aes))
        return false;

    if (!qemuAppendKeyWrapMachineParm(buf, qemuCaps, QEMU_CAPS_DEA_KEY_WRAP,
                                      "dea-key-wrap", keywrap->dea))
        return false;

    return true;
}


static void
qemuAppendLoadparmMachineParm(virBuffer *buf,
                              const virDomainDef *def)
{
    size_t i = 0;

    for (i = 0; i < def->ndisks; i++) {
        virDomainDiskDefPtr disk = def->disks[i];

        if (disk->info.bootIndex == 1 && disk->info.loadparm) {
            virBufferAsprintf(buf, ",loadparm=%s", disk->info.loadparm);
            return;
        }
    }

    /* Network boot device */
    for (i = 0; i < def->nnets; i++) {
        virDomainNetDefPtr net = def->nets[i];

        if (net->info.bootIndex == 1 && net->info.loadparm) {
            virBufferAsprintf(buf, ",loadparm=%s", net->info.loadparm);
            return;
        }
    }
}


static int
qemuBuildNameCommandLine(virCommandPtr cmd,
                         virQEMUDriverConfigPtr cfg,
                         const virDomainDef *def,
                         virQEMUCapsPtr qemuCaps)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;

    virCommandAddArg(cmd, "-name");

    /* The 'guest' option let's us handle a name with '=' embedded in it */
    if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_NAME_GUEST))
        virBufferAddLit(&buf, "guest=");

    virQEMUBuildBufferEscapeComma(&buf, def->name);

    if (cfg->setProcessName)
        virBufferAsprintf(&buf, ",process=qemu:%s", def->name);

    if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_NAME_DEBUG_THREADS))
        virBufferAddLit(&buf, ",debug-threads=on");

    virCommandAddArgBuffer(cmd, &buf);

    return 0;
}

static int
qemuBuildMachineCommandLine(virCommandPtr cmd,
                            virQEMUDriverConfigPtr cfg,
                            const virDomainDef *def,
                            virQEMUCapsPtr qemuCaps)
{
    virTristateSwitch vmport = def->features[VIR_DOMAIN_FEATURE_VMPORT];
    virTristateSwitch smm = def->features[VIR_DOMAIN_FEATURE_SMM];
    virCPUDefPtr cpu = def->cpu;
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    size_t i;
    int ret = -1;

    /* This should *never* be NULL, since we always provide
     * a machine in the capabilities data for QEMU. So this
     * check is just here as a safety in case the unexpected
     * happens */
    if (!def->os.machine)
        return 0;

    virCommandAddArg(cmd, "-machine");
    virBufferAdd(&buf, def->os.machine, -1);

    switch ((virDomainVirtType)def->virtType) {
    case VIR_DOMAIN_VIRT_QEMU:
        virBufferAddLit(&buf, ",accel=tcg");
        break;

    case VIR_DOMAIN_VIRT_KVM:
        virBufferAddLit(&buf, ",accel=kvm");
        break;

    case VIR_DOMAIN_VIRT_KQEMU:
    case VIR_DOMAIN_VIRT_XEN:
    case VIR_DOMAIN_VIRT_LXC:
    case VIR_DOMAIN_VIRT_UML:
    case VIR_DOMAIN_VIRT_OPENVZ:
    case VIR_DOMAIN_VIRT_TEST:
    case VIR_DOMAIN_VIRT_VMWARE:
    case VIR_DOMAIN_VIRT_HYPERV:
    case VIR_DOMAIN_VIRT_VBOX:
    case VIR_DOMAIN_VIRT_PHYP:
    case VIR_DOMAIN_VIRT_PARALLELS:
    case VIR_DOMAIN_VIRT_BHYVE:
    case VIR_DOMAIN_VIRT_VZ:
    case VIR_DOMAIN_VIRT_NONE:
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("the QEMU binary does not support %s"),
                       virDomainVirtTypeToString(def->virtType));
        return -1;

    case VIR_DOMAIN_VIRT_LAST:
    default:
        virReportEnumRangeError(virDomainVirtType, def->virtType);
        return -1;
    }

    /* To avoid the collision of creating USB controllers when calling
     * machine->init in QEMU, it needs to set usb=off
     */
    virBufferAddLit(&buf, ",usb=off");

    if (vmport) {
        if (!virQEMUCapsSupportsVmport(qemuCaps, def)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("vmport is not available "
                             "with this QEMU binary"));
            goto cleanup;
        }

        virBufferAsprintf(&buf, ",vmport=%s",
                          virTristateSwitchTypeToString(vmport));
    }

    if (smm)
        virBufferAsprintf(&buf, ",smm=%s", virTristateSwitchTypeToString(smm));

    if (def->mem.dump_core) {
        virBufferAsprintf(&buf, ",dump-guest-core=%s",
                          virTristateSwitchTypeToString(def->mem.dump_core));
    } else {
        virBufferAsprintf(&buf, ",dump-guest-core=%s",
                          cfg->dumpGuestCore ? "on" : "off");
    }

    if (def->mem.nosharepages) {
        if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_MEM_MERGE)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("disable shared memory is not available "
                             "with this QEMU binary"));
            goto cleanup;
        }

        virBufferAddLit(&buf, ",mem-merge=off");
    }

    if (def->keywrap &&
        !qemuAppendKeyWrapMachineParms(&buf, qemuCaps, def->keywrap))
        goto cleanup;

    if (def->features[VIR_DOMAIN_FEATURE_GIC] == VIR_TRISTATE_SWITCH_ON) {
        bool hasGICVersionOption = virQEMUCapsGet(qemuCaps,
                                                  QEMU_CAPS_MACH_VIRT_GIC_VERSION);

        switch ((virGICVersion) def->gic_version) {
        case VIR_GIC_VERSION_2:
            if (!hasGICVersionOption) {
                /* If the gic-version option is not available, we can't
                 * configure the GIC; however, we know that before the
                 * option was introduced the guests would always get a
                 * GICv2, so in order to maintain compatibility with
                 * those old QEMU versions all we need to do is stop
                 * early instead of erroring out */
                break;
            }
            ATTRIBUTE_FALLTHROUGH;

        case VIR_GIC_VERSION_3:
        case VIR_GIC_VERSION_HOST:
            if (!hasGICVersionOption) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("gic-version option is not available "
                                 "with this QEMU binary"));
                goto cleanup;
            }

            virBufferAsprintf(&buf, ",gic-version=%s",
                              virGICVersionTypeToString(def->gic_version));
            break;

        case VIR_GIC_VERSION_NONE:
        case VIR_GIC_VERSION_LAST:
        default:
            break;
        }
    }

    if (def->iommu) {
        switch (def->iommu->model) {
        case VIR_DOMAIN_IOMMU_MODEL_INTEL:
            /* qemuDomainDeviceDefValidateIOMMU() already made sure we have
             * one of QEMU_CAPS_DEVICE_INTEL_IOMMU or QEMU_CAPS_MACHINE_IOMMU:
             * here we handle the latter case, while the former is taken care
             * of in qemuBuildIOMMUCommandLine() */
            if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_MACHINE_IOMMU))
                virBufferAddLit(&buf, ",iommu=on");
            break;

        case VIR_DOMAIN_IOMMU_MODEL_SMMUV3:
            virBufferAddLit(&buf, ",iommu=smmuv3");
            break;

        case VIR_DOMAIN_IOMMU_MODEL_LAST:
        default:
            virReportEnumRangeError(virDomainIOMMUModel, def->iommu->model);
            return -1;
        }
    }

    for (i = 0; i < def->nmems; i++) {
        if (def->mems[i]->model == VIR_DOMAIN_MEMORY_MODEL_NVDIMM) {
            if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE_NVDIMM)) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("nvdimm isn't supported by this QEMU binary"));
                goto cleanup;
            }
            virBufferAddLit(&buf, ",nvdimm=on");
            break;
        }
    }

    if (def->features[VIR_DOMAIN_FEATURE_IOAPIC] != VIR_DOMAIN_IOAPIC_NONE) {
        if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_MACHINE_KERNEL_IRQCHIP)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("I/O APIC tuning is not supported by this "
                             "QEMU binary"));
            goto cleanup;
        }
        switch ((virDomainIOAPIC) def->features[VIR_DOMAIN_FEATURE_IOAPIC]) {
        case VIR_DOMAIN_IOAPIC_QEMU:
            if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_MACHINE_KERNEL_IRQCHIP_SPLIT)) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("split I/O APIC is not supported by this "
                                 "QEMU binary"));
                goto cleanup;
            }
            virBufferAddLit(&buf, ",kernel_irqchip=split");
            break;
        case VIR_DOMAIN_IOAPIC_KVM:
            virBufferAddLit(&buf, ",kernel_irqchip=on");
            break;
        case VIR_DOMAIN_IOAPIC_NONE:
        case VIR_DOMAIN_IOAPIC_LAST:
            break;
        }
    }

    if (def->features[VIR_DOMAIN_FEATURE_HPT] == VIR_TRISTATE_SWITCH_ON) {

        if (def->hpt_resizing != VIR_DOMAIN_HPT_RESIZING_NONE) {
            const char *str;

            if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_MACHINE_PSERIES_RESIZE_HPT)) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("HTP resizing is not supported by this "
                                 "QEMU binary"));
                goto cleanup;
            }

            str = virDomainHPTResizingTypeToString(def->hpt_resizing);
            if (!str) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("Invalid setting for HPT resizing"));
                goto cleanup;
            }

            virBufferAsprintf(&buf, ",resize-hpt=%s", str);
        }

        if (def->hpt_maxpagesize > 0) {
            if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_MACHINE_PSERIES_CAP_HPT_MAX_PAGE_SIZE)) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("Configuring the page size for HPT guests "
                                 "is not supported by this QEMU binary"));
                goto cleanup;
            }

            virBufferAsprintf(&buf, ",cap-hpt-max-page-size=%lluk",
                              def->hpt_maxpagesize);
        }
    }

    if (def->features[VIR_DOMAIN_FEATURE_HTM] != VIR_TRISTATE_SWITCH_ABSENT) {
        const char *str;

        if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_MACHINE_PSERIES_CAP_HTM)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("HTM configuration is not supported by this "
                             "QEMU binary"));
            goto cleanup;
        }

        str = virTristateSwitchTypeToString(def->features[VIR_DOMAIN_FEATURE_HTM]);
        if (!str) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("Invalid setting for HTM state"));
            goto cleanup;
        }

        virBufferAsprintf(&buf, ",cap-htm=%s", str);
    }

    if (def->features[VIR_DOMAIN_FEATURE_NESTED_HV] != VIR_TRISTATE_SWITCH_ABSENT) {
        const char *str;

        if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_MACHINE_PSERIES_CAP_NESTED_HV)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("Nested HV configuration is not supported by "
                             "this QEMU binary"));
            goto cleanup;
        }

        str = virTristateSwitchTypeToString(def->features[VIR_DOMAIN_FEATURE_NESTED_HV]);
        if (!str) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("Invalid setting for nested HV state"));
            goto cleanup;
        }

        virBufferAsprintf(&buf, ",cap-nested-hv=%s", str);
    }

    if (cpu && cpu->model &&
        cpu->mode == VIR_CPU_MODE_HOST_MODEL &&
        qemuDomainIsPSeries(def) &&
        virQEMUCapsGet(qemuCaps, QEMU_CAPS_MACHINE_PSERIES_MAX_CPU_COMPAT)) {
        virBufferAsprintf(&buf, ",max-cpu-compat=%s", cpu->model);
    }

    if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_LOADPARM))
        qemuAppendLoadparmMachineParm(&buf, def);

    if (def->sev)
        virBufferAddLit(&buf, ",memory-encryption=sev0");

    virCommandAddArgBuffer(cmd, &buf);

    ret = 0;
 cleanup:
    virBufferFreeAndReset(&buf);
    return ret;
}


static void
qemuBuildTSEGCommandLine(virCommandPtr cmd,
                         const virDomainDef *def)
{
    if (!def->tseg_specified)
        return;

    virCommandAddArg(cmd, "-global");

    /* PostParse callback guarantees that the size is divisible by 1 MiB */
    virCommandAddArgFormat(cmd, "mch.extended-tseg-mbytes=%llu",
                           def->tseg_size >> 20);
}


static int
qemuBuildSmpCommandLine(virCommandPtr cmd,
                        virDomainDefPtr def)
{
    char *smp;
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    unsigned int maxvcpus = virDomainDefGetVcpusMax(def);
    unsigned int nvcpus = 0;
    virDomainVcpuDefPtr vcpu;
    size_t i;

    /* count non-hotpluggable enabled vcpus. Hotpluggable ones will be added
     * in a different way */
    for (i = 0; i < maxvcpus; i++) {
        vcpu = virDomainDefGetVcpu(def, i);
        if (vcpu->online && vcpu->hotpluggable == VIR_TRISTATE_BOOL_NO)
            nvcpus++;
    }

    virCommandAddArg(cmd, "-smp");

    virBufferAsprintf(&buf, "%u", nvcpus);

    if (nvcpus != maxvcpus)
        virBufferAsprintf(&buf, ",maxcpus=%u", maxvcpus);
    /* sockets, cores, and threads are either all zero
     * or all non-zero, thus checking one of them is enough */
    if (def->cpu && def->cpu->sockets) {
        virBufferAsprintf(&buf, ",sockets=%u", def->cpu->sockets);
        virBufferAsprintf(&buf, ",cores=%u", def->cpu->cores);
        virBufferAsprintf(&buf, ",threads=%u", def->cpu->threads);
    } else {
        virBufferAsprintf(&buf, ",sockets=%u", virDomainDefGetVcpusMax(def));
        virBufferAsprintf(&buf, ",cores=%u", 1);
        virBufferAsprintf(&buf, ",threads=%u", 1);
    }

    if (virBufferCheckError(&buf) < 0)
        return -1;

    smp = virBufferContentAndReset(&buf);
    virCommandAddArg(cmd, smp);
    VIR_FREE(smp);

    return 0;
}


static int
qemuBuildMemPathStr(virQEMUDriverConfigPtr cfg,
                    const virDomainDef *def,
                    virCommandPtr cmd,
                    qemuDomainObjPrivatePtr priv)
{
    const long system_page_size = virGetSystemPageSizeKB();
    char *mem_path = NULL;

    /* There are two cases where we want to put -mem-path onto
     * the command line: First one is when there are no guest
     * NUMA nodes and hugepages are configured. The second one is
     * if user requested file allocation. */
    if (def->mem.nhugepages &&
        def->mem.hugepages[0].size != system_page_size) {
        unsigned long long pagesize = def->mem.hugepages[0].size;
        if (!pagesize &&
            qemuBuildMemoryGetDefaultPagesize(cfg, &pagesize) < 0)
            return -1;
        if (qemuGetDomainHupageMemPath(def, cfg, pagesize, &mem_path) < 0)
            return -1;
    } else if (def->mem.source == VIR_DOMAIN_MEMORY_SOURCE_FILE) {
        if (qemuGetMemoryBackingPath(def, cfg, "ram", &mem_path) < 0)
            return -1;
    } else {
        return 0;
    }

    if (def->mem.allocation != VIR_DOMAIN_MEMORY_ALLOCATION_IMMEDIATE) {
        virCommandAddArgList(cmd, "-mem-prealloc", NULL);
        priv->memPrealloc = true;
    }

    virCommandAddArgList(cmd, "-mem-path", mem_path, NULL);
    VIR_FREE(mem_path);

    return 0;
}


static int
qemuBuildMemCommandLine(virCommandPtr cmd,
                        virQEMUDriverConfigPtr cfg,
                        const virDomainDef *def,
                        virQEMUCapsPtr qemuCaps,
                        qemuDomainObjPrivatePtr priv)
{
    if (qemuDomainDefValidateMemoryHotplug(def, qemuCaps, NULL) < 0)
        return -1;

    virCommandAddArg(cmd, "-m");

    if (virDomainDefHasMemoryHotplug(def)) {
        /* Use the 'k' suffix to let qemu handle the units */
        virCommandAddArgFormat(cmd, "size=%lluk,slots=%u,maxmem=%lluk",
                               virDomainDefGetMemoryInitial(def),
                               def->mem.memory_slots,
                               def->mem.max_memory);

    } else {
       virCommandAddArgFormat(cmd, "%llu",
                              virDomainDefGetMemoryInitial(def) / 1024);
    }

    if (def->mem.allocation == VIR_DOMAIN_MEMORY_ALLOCATION_IMMEDIATE) {
        virCommandAddArgList(cmd, "-mem-prealloc", NULL);
        priv->memPrealloc = true;
    }

    /*
     * Add '-mem-path' (and '-mem-prealloc') parameter here if
     * the hugepages and no numa node is specified.
     */
    if (!virDomainNumaGetNodeCount(def->numa) &&
        qemuBuildMemPathStr(cfg, def, cmd, priv) < 0)
        return -1;

    if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_OVERCOMMIT)) {
        virCommandAddArg(cmd, "-overcommit");
        virCommandAddArgFormat(cmd, "mem-lock=%s", def->mem.locked ? "on" : "off");
    } else {
        virCommandAddArg(cmd, "-realtime");
        virCommandAddArgFormat(cmd, "mlock=%s",
                               def->mem.locked ? "on" : "off");
    }

    return 0;
}


static int
qemuBuildIOThreadCommandLine(virCommandPtr cmd,
                             const virDomainDef *def)
{
    size_t i;

    if (def->niothreadids == 0)
        return 0;

    /* Create iothread objects using the defined iothreadids list
     * and the defined id and name from the list. These may be used
     * by a disk definition which will associate to an iothread by
     * supplying a value of an id from the list
     */
    for (i = 0; i < def->niothreadids; i++) {
        virCommandAddArg(cmd, "-object");
        virCommandAddArgFormat(cmd, "iothread,id=iothread%u",
                               def->iothreadids[i]->iothread_id);
    }

    return 0;
}


static int
qemuBuildNumaArgStr(virQEMUDriverConfigPtr cfg,
                    virDomainDefPtr def,
                    virCommandPtr cmd,
                    qemuDomainObjPrivatePtr priv)
{
    size_t i, j;
    virQEMUCapsPtr qemuCaps = priv->qemuCaps;
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    char *cpumask = NULL, *tmpmask = NULL, *next = NULL;
    virBufferPtr nodeBackends = NULL;
    bool needBackend = false;
    int rc;
    int ret = -1;
    size_t ncells = virDomainNumaGetNodeCount(def->numa);
    const long system_page_size = virGetSystemPageSizeKB();
    bool numa_distances = false;

    if (virDomainNumatuneHasPerNodeBinding(def->numa) &&
        !(virQEMUCapsGet(qemuCaps, QEMU_CAPS_OBJECT_MEMORY_RAM) ||
          virQEMUCapsGet(qemuCaps, QEMU_CAPS_OBJECT_MEMORY_FILE) ||
          virQEMUCapsGet(qemuCaps, QEMU_CAPS_OBJECT_MEMORY_MEMFD))) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Per-node memory binding is not supported "
                         "with this QEMU"));
        goto cleanup;
    }

    if (def->mem.nhugepages &&
        def->mem.hugepages[0].size != system_page_size &&
        !virQEMUCapsGet(qemuCaps, QEMU_CAPS_OBJECT_MEMORY_FILE)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("huge pages per NUMA node are not "
                         "supported with this QEMU"));
        goto cleanup;
    }

    if (!virDomainNumatuneNodesetIsAvailable(def->numa, priv->autoNodeset))
        goto cleanup;

    if (VIR_ALLOC_N(nodeBackends, ncells) < 0)
        goto cleanup;

    /* using of -numa memdev= cannot be combined with -numa mem=, thus we
     * need to check which approach to use */
    for (i = 0; i < ncells; i++) {
        if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_OBJECT_MEMORY_RAM) ||
            virQEMUCapsGet(qemuCaps, QEMU_CAPS_OBJECT_MEMORY_FILE) ||
            virQEMUCapsGet(qemuCaps, QEMU_CAPS_OBJECT_MEMORY_MEMFD)) {

            if ((rc = qemuBuildMemoryCellBackendStr(def, cfg, i, priv,
                                                    &nodeBackends[i])) < 0)
                goto cleanup;

            if (rc == 0)
                needBackend = true;
        } else {
            if (virDomainNumaGetNodeMemoryAccessMode(def->numa, i)) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("Shared memory mapping is not supported "
                                 "with this QEMU"));
                goto cleanup;
            }
        }
    }

    if (!needBackend &&
        qemuBuildMemPathStr(cfg, def, cmd, priv) < 0)
        goto cleanup;

    for (i = 0; i < ncells; i++) {
        VIR_FREE(cpumask);
        if (!(cpumask = virBitmapFormat(virDomainNumaGetNodeCpumask(def->numa, i))))
            goto cleanup;

        if (strchr(cpumask, ',') &&
            !virQEMUCapsGet(qemuCaps, QEMU_CAPS_NUMA)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("disjoint NUMA cpu ranges are not supported "
                             "with this QEMU"));
            goto cleanup;
        }

        if (needBackend) {
            virCommandAddArg(cmd, "-object");
            virCommandAddArgBuffer(cmd, &nodeBackends[i]);
        }

        virCommandAddArg(cmd, "-numa");
        virBufferAsprintf(&buf, "node,nodeid=%zu", i);

        for (tmpmask = cpumask; tmpmask; tmpmask = next) {
            if ((next = strchr(tmpmask, ',')))
                *(next++) = '\0';
            virBufferAddLit(&buf, ",cpus=");
            virBufferAdd(&buf, tmpmask, -1);
        }

        if (needBackend)
            virBufferAsprintf(&buf, ",memdev=ram-node%zu", i);
        else
            virBufferAsprintf(&buf, ",mem=%llu",
                              virDomainNumaGetNodeMemorySize(def->numa, i) / 1024);

        virCommandAddArgBuffer(cmd, &buf);
    }

    /* If NUMA node distance is specified for at least one pair
     * of nodes, we have to specify all the distances. Even
     * though they might be the default ones. */
    for (i = 0; i < ncells; i++) {
        for (j = 0; j < ncells; j++) {
            if (virDomainNumaNodeDistanceIsUsingDefaults(def->numa, i, j))
                continue;

            numa_distances = true;
            break;
        }
        if (numa_distances)
            break;
    }

    if (numa_distances) {
        if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_NUMA_DIST)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("setting NUMA distances is not "
                             "supported with this qemu"));
            goto cleanup;
        }

        for (i = 0; i < ncells; i++) {
            for (j = 0; j < ncells; j++) {
                size_t distance = virDomainNumaGetNodeDistance(def->numa, i, j);

                virCommandAddArg(cmd, "-numa");
                virBufferAsprintf(&buf, "dist,src=%zu,dst=%zu,val=%zu", i, j, distance);

                virCommandAddArgBuffer(cmd, &buf);
            }
        }
    }

    ret = 0;

 cleanup:
    VIR_FREE(cpumask);

    if (nodeBackends) {
        for (i = 0; i < ncells; i++)
            virBufferFreeAndReset(&nodeBackends[i]);

        VIR_FREE(nodeBackends);
    }

    virBufferFreeAndReset(&buf);
    return ret;
}


static int
qemuBuildMemoryDeviceCommandLine(virCommandPtr cmd,
                                 virQEMUDriverConfigPtr cfg,
                                 virDomainDefPtr def,
                                 qemuDomainObjPrivatePtr priv)
{
    size_t i;

    /* memory hotplug requires NUMA to be enabled - we already checked
     * that memory devices are present only when NUMA is */
    for (i = 0; i < def->nmems; i++) {
        virBuffer buf = VIR_BUFFER_INITIALIZER;
        char *dimmStr;

        if (qemuBuildMemoryDimmBackendStr(&buf, def->mems[i], def, cfg, priv) < 0)
            return -1;

        virCommandAddArg(cmd, "-object");
        virCommandAddArgBuffer(cmd, &buf);

        if (!(dimmStr = qemuBuildMemoryDeviceStr(def->mems[i], priv)))
            return -1;

        virCommandAddArgList(cmd, "-device", dimmStr, NULL);

        VIR_FREE(dimmStr);
    }

    return 0;
}


static int
qemuBuildGraphicsSDLCommandLine(virQEMUDriverConfigPtr cfg ATTRIBUTE_UNUSED,
                                virCommandPtr cmd,
                                virQEMUCapsPtr qemuCaps ATTRIBUTE_UNUSED,
                                virDomainGraphicsDefPtr graphics)
{
    int ret = -1;
    virBuffer opt = VIR_BUFFER_INITIALIZER;

    if (graphics->data.sdl.xauth)
        virCommandAddEnvPair(cmd, "XAUTHORITY", graphics->data.sdl.xauth);
    if (graphics->data.sdl.display)
        virCommandAddEnvPair(cmd, "DISPLAY", graphics->data.sdl.display);
    if (graphics->data.sdl.fullscreen)
        virCommandAddArg(cmd, "-full-screen");

    /* If using SDL for video, then we should just let it
     * use QEMU's host audio drivers, possibly SDL too
     * User can set these two before starting libvirtd
     */
    virCommandAddEnvPassBlockSUID(cmd, "QEMU_AUDIO_DRV", NULL);
    virCommandAddEnvPassBlockSUID(cmd, "SDL_AUDIODRIVER", NULL);

    virCommandAddArg(cmd, "-display");
    virBufferAddLit(&opt, "sdl");

    if (graphics->data.sdl.gl != VIR_TRISTATE_BOOL_ABSENT) {
        if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_SDL_GL)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("OpenGL for SDL is not supported with this QEMU "
                             "binary"));
            goto cleanup;
        }

        virBufferAsprintf(&opt, ",gl=%s",
                          virTristateSwitchTypeToString(graphics->data.sdl.gl));

    }

    if (virBufferCheckError(&opt) < 0)
        goto cleanup;

    virCommandAddArgBuffer(cmd, &opt);

    ret = 0;
 cleanup:
    virBufferFreeAndReset(&opt);
    return ret;
}


static int
qemuBuildGraphicsVNCCommandLine(virQEMUDriverConfigPtr cfg,
                                virCommandPtr cmd,
                                virQEMUCapsPtr qemuCaps,
                                virDomainGraphicsDefPtr graphics)
{
    virBuffer opt = VIR_BUFFER_INITIALIZER;
    virDomainGraphicsListenDefPtr glisten = NULL;
    bool escapeAddr;

    if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_VNC)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("vnc graphics are not supported with this QEMU"));
        goto error;
    }

    if (!(glisten = virDomainGraphicsGetListen(graphics, 0))) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("missing listen element"));
        goto error;
    }

    switch (glisten->type) {
    case VIR_DOMAIN_GRAPHICS_LISTEN_TYPE_SOCKET:
        if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_VNC_MULTI_SERVERS))
            virBufferAddLit(&opt, "vnc=unix:");
        else
            virBufferAddLit(&opt, "unix:");
        virQEMUBuildBufferEscapeComma(&opt, glisten->socket);
        break;

    case VIR_DOMAIN_GRAPHICS_LISTEN_TYPE_ADDRESS:
    case VIR_DOMAIN_GRAPHICS_LISTEN_TYPE_NETWORK:
        if (!graphics->data.vnc.autoport &&
            (graphics->data.vnc.port < 5900 ||
             graphics->data.vnc.port > 65535)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("vnc port must be in range [5900,65535]"));
            goto error;
        }

        if (glisten->address) {
            escapeAddr = strchr(glisten->address, ':') != NULL;
            if (escapeAddr)
                virBufferAsprintf(&opt, "[%s]", glisten->address);
            else
                virBufferAdd(&opt, glisten->address, -1);
        }
        virBufferAsprintf(&opt, ":%d",
                          graphics->data.vnc.port - 5900);

        if (graphics->data.vnc.websocket)
            virBufferAsprintf(&opt, ",websocket=%d", graphics->data.vnc.websocket);
        break;

    case VIR_DOMAIN_GRAPHICS_LISTEN_TYPE_NONE:
        virBufferAddLit(&opt, "none");
        break;

    case VIR_DOMAIN_GRAPHICS_LISTEN_TYPE_LAST:
        break;
    }

    if (graphics->data.vnc.sharePolicy) {
        virBufferAsprintf(&opt, ",share=%s",
                          virDomainGraphicsVNCSharePolicyTypeToString(
                              graphics->data.vnc.sharePolicy));
    }

    if (graphics->data.vnc.auth.passwd || cfg->vncPassword)
        virBufferAddLit(&opt, ",password");

    if (cfg->vncTLS) {
        qemuDomainGraphicsPrivatePtr gfxPriv = QEMU_DOMAIN_GRAPHICS_PRIVATE(graphics);
        if (gfxPriv->tlsAlias) {
            const char *secretAlias = NULL;

            if (gfxPriv->secinfo) {
                if (qemuBuildObjectSecretCommandLine(cmd,
                                                     gfxPriv->secinfo) < 0)
                    goto error;
                secretAlias = gfxPriv->secinfo->s.aes.alias;
            }

            if (qemuBuildTLSx509CommandLine(cmd,
                                            cfg->vncTLSx509certdir,
                                            true,
                                            cfg->vncTLSx509verify,
                                            secretAlias,
                                            gfxPriv->tlsAlias,
                                            qemuCaps) < 0)
                goto error;

            virBufferAsprintf(&opt, ",tls-creds=%s", gfxPriv->tlsAlias);
        } else {
            virBufferAddLit(&opt, ",tls");
            if (cfg->vncTLSx509verify) {
                virBufferAddLit(&opt, ",x509verify=");
                virQEMUBuildBufferEscapeComma(&opt, cfg->vncTLSx509certdir);
            } else {
                virBufferAddLit(&opt, ",x509=");
                virQEMUBuildBufferEscapeComma(&opt, cfg->vncTLSx509certdir);
            }
        }
    }

    if (cfg->vncSASL) {
        virBufferAddLit(&opt, ",sasl");

        if (cfg->vncSASLdir)
            virCommandAddEnvPair(cmd, "SASL_CONF_PATH", cfg->vncSASLdir);

        /* TODO: Support ACLs later */
    }

    virCommandAddArg(cmd, "-vnc");
    virCommandAddArgBuffer(cmd, &opt);
    if (graphics->data.vnc.keymap)
        virCommandAddArgList(cmd, "-k", graphics->data.vnc.keymap, NULL);

    /* Unless user requested it, set the audio backend to none, to
     * prevent it opening the host OS audio devices, since that causes
     * security issues and might not work when using VNC.
     */
    if (cfg->vncAllowHostAudio)
        virCommandAddEnvPassBlockSUID(cmd, "QEMU_AUDIO_DRV", NULL);
    else
        virCommandAddEnvString(cmd, "QEMU_AUDIO_DRV=none");

    return 0;

 error:
    virBufferFreeAndReset(&opt);
    return -1;
}


static int
qemuBuildGraphicsSPICECommandLine(virQEMUDriverConfigPtr cfg,
                                  virCommandPtr cmd,
                                  virQEMUCapsPtr qemuCaps,
                                  virDomainGraphicsDefPtr graphics)
{
    virBuffer opt = VIR_BUFFER_INITIALIZER;
    virDomainGraphicsListenDefPtr glisten = NULL;
    int port = graphics->data.spice.port;
    int tlsPort = graphics->data.spice.tlsPort;
    size_t i;
    bool hasSecure = false;
    bool hasInsecure = false;

    if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_SPICE)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("spice graphics are not supported with this QEMU"));
        goto error;
    }

    if (!(glisten = virDomainGraphicsGetListen(graphics, 0))) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("missing listen element"));
        goto error;
    }

    switch (glisten->type) {
    case VIR_DOMAIN_GRAPHICS_LISTEN_TYPE_SOCKET:
        if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_SPICE_UNIX)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("unix socket for spice graphics are not supported "
                             "with this QEMU"));
            goto error;
        }

        virBufferAddLit(&opt, "unix,addr=");
        virQEMUBuildBufferEscapeComma(&opt, glisten->socket);
        virBufferAddLit(&opt, ",");
        hasInsecure = true;
        break;

    case VIR_DOMAIN_GRAPHICS_LISTEN_TYPE_ADDRESS:
    case VIR_DOMAIN_GRAPHICS_LISTEN_TYPE_NETWORK:
        if (port > 0) {
            virBufferAsprintf(&opt, "port=%u,", port);
            hasInsecure = true;
        }

        if (tlsPort > 0) {
            if (!cfg->spiceTLS) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("spice TLS port set in XML configuration, "
                                 "but TLS is disabled in qemu.conf"));
                goto error;
            }
            virBufferAsprintf(&opt, "tls-port=%u,", tlsPort);
            hasSecure = true;
        }

        if (port > 0 || tlsPort > 0) {
            if (glisten->address)
                virBufferAsprintf(&opt, "addr=%s,", glisten->address);
        }

        break;

    case VIR_DOMAIN_GRAPHICS_LISTEN_TYPE_NONE:
        /* QEMU requires either port or tls-port to be specified if there is no
         * other argument. Use a dummy port=0. */
        virBufferAddLit(&opt, "port=0,");
        hasInsecure = true;
        break;
    case VIR_DOMAIN_GRAPHICS_LISTEN_TYPE_LAST:
        break;
    }

    if (cfg->spiceSASL) {
        virBufferAddLit(&opt, "sasl,");

        if (cfg->spiceSASLdir)
            virCommandAddEnvPair(cmd, "SASL_CONF_PATH",
                                 cfg->spiceSASLdir);

        /* TODO: Support ACLs later */
    }

    if (graphics->data.spice.mousemode) {
        switch (graphics->data.spice.mousemode) {
        case VIR_DOMAIN_GRAPHICS_SPICE_MOUSE_MODE_SERVER:
            virBufferAddLit(&opt, "agent-mouse=off,");
            break;
        case VIR_DOMAIN_GRAPHICS_SPICE_MOUSE_MODE_CLIENT:
            virBufferAddLit(&opt, "agent-mouse=on,");
            break;
        case VIR_DOMAIN_GRAPHICS_SPICE_MOUSE_MODE_DEFAULT:
            break;
        case VIR_DOMAIN_GRAPHICS_SPICE_MOUSE_MODE_LAST:
        default:
            virReportEnumRangeError(virDomainGraphicsSpiceMouseMode,
                                    graphics->data.spice.mousemode);
            goto error;
        }
    }

    /* In the password case we set it via monitor command, to avoid
     * making it visible on CLI, so there's no use of password=XXX
     * in this bit of the code */
    if (!graphics->data.spice.auth.passwd &&
        !cfg->spicePassword)
        virBufferAddLit(&opt, "disable-ticketing,");

    if (hasSecure) {
        virBufferAddLit(&opt, "x509-dir=");
        virQEMUBuildBufferEscapeComma(&opt, cfg->spiceTLSx509certdir);
        virBufferAddLit(&opt, ",");
    }

    switch (graphics->data.spice.defaultMode) {
    case VIR_DOMAIN_GRAPHICS_SPICE_CHANNEL_MODE_SECURE:
        if (!hasSecure) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("spice defaultMode secure requested in XML "
                             "configuration, but TLS connection is not "
                             "available"));
            goto error;
        }
        virBufferAddLit(&opt, "tls-channel=default,");
        break;
    case VIR_DOMAIN_GRAPHICS_SPICE_CHANNEL_MODE_INSECURE:
        if (!hasInsecure) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("spice defaultMode insecure requested in XML "
                             "configuration, but plaintext connection is not "
                             "available"));
            goto error;
        }
        virBufferAddLit(&opt, "plaintext-channel=default,");
        break;
    case VIR_DOMAIN_GRAPHICS_SPICE_CHANNEL_MODE_ANY:
    case VIR_DOMAIN_GRAPHICS_SPICE_CHANNEL_MODE_LAST:
        /* nothing */
        break;
    }

    for (i = 0; i < VIR_DOMAIN_GRAPHICS_SPICE_CHANNEL_LAST; i++) {
        switch (graphics->data.spice.channels[i]) {
        case VIR_DOMAIN_GRAPHICS_SPICE_CHANNEL_MODE_SECURE:
            if (!hasSecure) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("spice secure channels set in XML "
                                 "configuration, but TLS connection is not "
                                 "available"));
                goto error;
            }
            virBufferAsprintf(&opt, "tls-channel=%s,",
                              virDomainGraphicsSpiceChannelNameTypeToString(i));
            break;

        case VIR_DOMAIN_GRAPHICS_SPICE_CHANNEL_MODE_INSECURE:
            if (!hasInsecure) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("spice insecure channels set in XML "
                                 "configuration, but plaintext connection "
                                 "is not available"));
                goto error;
            }
            virBufferAsprintf(&opt, "plaintext-channel=%s,",
                              virDomainGraphicsSpiceChannelNameTypeToString(i));
            break;

        case VIR_DOMAIN_GRAPHICS_SPICE_CHANNEL_MODE_ANY:
            break;
        }
    }

    if (graphics->data.spice.image)
        virBufferAsprintf(&opt, "image-compression=%s,",
                          virDomainGraphicsSpiceImageCompressionTypeToString(graphics->data.spice.image));
    if (graphics->data.spice.jpeg)
        virBufferAsprintf(&opt, "jpeg-wan-compression=%s,",
                          virDomainGraphicsSpiceJpegCompressionTypeToString(graphics->data.spice.jpeg));
    if (graphics->data.spice.zlib)
        virBufferAsprintf(&opt, "zlib-glz-wan-compression=%s,",
                          virDomainGraphicsSpiceZlibCompressionTypeToString(graphics->data.spice.zlib));
    if (graphics->data.spice.playback)
        virBufferAsprintf(&opt, "playback-compression=%s,",
                          virTristateSwitchTypeToString(graphics->data.spice.playback));
    if (graphics->data.spice.streaming)
        virBufferAsprintf(&opt, "streaming-video=%s,",
                          virDomainGraphicsSpiceStreamingModeTypeToString(graphics->data.spice.streaming));
    if (graphics->data.spice.copypaste == VIR_TRISTATE_BOOL_NO)
        virBufferAddLit(&opt, "disable-copy-paste,");

    if (graphics->data.spice.filetransfer == VIR_TRISTATE_BOOL_NO) {
        if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_SPICE_FILE_XFER_DISABLE)) {
           virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                          _("This QEMU can't disable file transfers through spice"));
            goto error;
        } else {
            virBufferAddLit(&opt, "disable-agent-file-xfer,");
        }
    }

    if (graphics->data.spice.gl == VIR_TRISTATE_BOOL_YES) {
        if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_SPICE_GL)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("This QEMU doesn't support spice OpenGL"));
            goto error;
        }

        /* spice.gl is a TristateBool, but qemu expects on/off: use
         * TristateSwitch helper */
        virBufferAsprintf(&opt, "gl=%s,",
                          virTristateSwitchTypeToString(graphics->data.spice.gl));

        if (graphics->data.spice.rendernode) {
            if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_SPICE_RENDERNODE)) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("This QEMU doesn't support spice OpenGL rendernode"));
                goto error;
            }

            virBufferAddLit(&opt, "rendernode=");
            virQEMUBuildBufferEscapeComma(&opt, graphics->data.spice.rendernode);
            virBufferAddLit(&opt, ",");
        }
    }

    /* Turn on seamless migration unconditionally. If migration destination
     * doesn't support it, it fallbacks to previous migration algorithm silently. */
    virBufferAddLit(&opt, "seamless-migration=on,");

    virBufferTrim(&opt, ",", -1);

    virCommandAddArg(cmd, "-spice");
    virCommandAddArgBuffer(cmd, &opt);
    if (graphics->data.spice.keymap)
        virCommandAddArgList(cmd, "-k",
                             graphics->data.spice.keymap, NULL);
    /* SPICE includes native support for tunnelling audio, so we
     * set the audio backend to point at SPICE's own driver
     */
    virCommandAddEnvString(cmd, "QEMU_AUDIO_DRV=spice");

    return 0;

 error:
    virBufferFreeAndReset(&opt);
    return -1;
}


static int
qemuBuildGraphicsEGLHeadlessCommandLine(virQEMUDriverConfigPtr cfg ATTRIBUTE_UNUSED,
                                        virCommandPtr cmd,
                                        virQEMUCapsPtr qemuCaps,
                                        virDomainGraphicsDefPtr graphics)
{
    int ret = -1;
    virBuffer opt = VIR_BUFFER_INITIALIZER;

    virBufferAddLit(&opt, "egl-headless");

    if (graphics->data.egl_headless.rendernode) {
        if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_EGL_HEADLESS_RENDERNODE)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("This QEMU doesn't support OpenGL rendernode "
                             "with egl-headless graphics type"));
            goto cleanup;
        }

        virBufferAddLit(&opt, ",rendernode=");
        virQEMUBuildBufferEscapeComma(&opt,
                                      graphics->data.egl_headless.rendernode);
    }

    if (virBufferCheckError(&opt) < 0)
        goto cleanup;

    virCommandAddArg(cmd, "-display");
    virCommandAddArgBuffer(cmd, &opt);

    ret = 0;
 cleanup:
    virBufferFreeAndReset(&opt);
    return ret;
}


static int
qemuBuildGraphicsCommandLine(virQEMUDriverConfigPtr cfg,
                             virCommandPtr cmd,
                             virDomainDefPtr def,
                             virQEMUCapsPtr qemuCaps)
{
    size_t i;

    for (i = 0; i < def->ngraphics; i++) {
        virDomainGraphicsDefPtr graphics = def->graphics[i];

        switch (graphics->type) {
        case VIR_DOMAIN_GRAPHICS_TYPE_SDL:
            if (qemuBuildGraphicsSDLCommandLine(cfg, cmd,
                                                qemuCaps, graphics) < 0)
                return -1;

            break;
        case VIR_DOMAIN_GRAPHICS_TYPE_VNC:
            if (qemuBuildGraphicsVNCCommandLine(cfg, cmd,
                                                qemuCaps, graphics) < 0)
                return -1;

            break;
        case VIR_DOMAIN_GRAPHICS_TYPE_SPICE:
            if (qemuBuildGraphicsSPICECommandLine(cfg, cmd,
                                                  qemuCaps, graphics) < 0)
                return -1;

            break;
        case VIR_DOMAIN_GRAPHICS_TYPE_EGL_HEADLESS:
            if (qemuBuildGraphicsEGLHeadlessCommandLine(cfg, cmd,
                                                        qemuCaps, graphics) < 0)
                return -1;

            break;
        case VIR_DOMAIN_GRAPHICS_TYPE_RDP:
        case VIR_DOMAIN_GRAPHICS_TYPE_DESKTOP:
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("unsupported graphics type '%s'"),
                           virDomainGraphicsTypeToString(graphics->type));
            return -1;
        case VIR_DOMAIN_GRAPHICS_TYPE_LAST:
        default:
            virReportEnumRangeError(virDomainGraphicsType, graphics->type);
            return -1;
        }
    }

    return 0;
}

static int
qemuInterfaceVhostuserConnect(virQEMUDriverPtr driver,
                              virLogManagerPtr logManager,
                              virSecurityManagerPtr secManager,
                              virCommandPtr cmd,
                              virDomainDefPtr def,
                              virDomainNetDefPtr net,
                              virQEMUCapsPtr qemuCaps,
                              char **chardev)
{
    virQEMUDriverConfigPtr cfg = virQEMUDriverGetConfig(driver);
    int ret = -1;

    switch ((virDomainChrType)net->data.vhostuser->type) {
    case VIR_DOMAIN_CHR_TYPE_UNIX:
        if (!(*chardev = qemuBuildChrChardevStr(logManager, secManager,
                                                cmd, cfg, def,
                                                net->data.vhostuser,
                                                net->info.alias, qemuCaps, 0)))
            goto cleanup;
        break;

    case VIR_DOMAIN_CHR_TYPE_NULL:
    case VIR_DOMAIN_CHR_TYPE_VC:
    case VIR_DOMAIN_CHR_TYPE_PTY:
    case VIR_DOMAIN_CHR_TYPE_DEV:
    case VIR_DOMAIN_CHR_TYPE_FILE:
    case VIR_DOMAIN_CHR_TYPE_PIPE:
    case VIR_DOMAIN_CHR_TYPE_STDIO:
    case VIR_DOMAIN_CHR_TYPE_UDP:
    case VIR_DOMAIN_CHR_TYPE_TCP:
    case VIR_DOMAIN_CHR_TYPE_SPICEVMC:
    case VIR_DOMAIN_CHR_TYPE_SPICEPORT:
    case VIR_DOMAIN_CHR_TYPE_NMDM:
    case VIR_DOMAIN_CHR_TYPE_LAST:
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("vhost-user type '%s' not supported"),
                       virDomainChrTypeToString(net->data.vhostuser->type));
        goto cleanup;
    }

    ret = 0;
 cleanup:
    virObjectUnref(cfg);
    return ret;
}

static int
qemuBuildInterfaceCommandLine(virQEMUDriverPtr driver,
                              virLogManagerPtr logManager,
                              virSecurityManagerPtr secManager,
                              virCommandPtr cmd,
                              virDomainDefPtr def,
                              virDomainNetDefPtr net,
                              virQEMUCapsPtr qemuCaps,
                              unsigned int bootindex,
                              virNetDevVPortProfileOp vmop,
                              bool standalone,
                              size_t *nnicindexes,
                              int **nicindexes)
{
    int ret = -1;
    char *nic = NULL;
    char *host = NULL;
    char *chardev = NULL;
    int *tapfd = NULL;
    size_t tapfdSize = 0;
    int *vhostfd = NULL;
    size_t vhostfdSize = 0;
    char **tapfdName = NULL;
    char **vhostfdName = NULL;
    virDomainNetType actualType = virDomainNetGetActualType(net);
    virNetDevBandwidthPtr actualBandwidth;
    bool requireNicdev = false;
    size_t i;


    if (!bootindex)
        bootindex = net->info.bootIndex;

    /* Currently nothing besides TAP devices supports multiqueue. */
    if (net->driver.virtio.queues > 0 &&
        !(actualType == VIR_DOMAIN_NET_TYPE_NETWORK ||
          actualType == VIR_DOMAIN_NET_TYPE_BRIDGE ||
          actualType == VIR_DOMAIN_NET_TYPE_DIRECT ||
          actualType == VIR_DOMAIN_NET_TYPE_ETHERNET ||
          actualType == VIR_DOMAIN_NET_TYPE_VHOSTUSER)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Multiqueue network is not supported for: %s"),
                       virDomainNetTypeToString(actualType));
        return -1;
    }

    /* and only TAP devices support nwfilter rules */
    if (net->filter) {
        virNetDevVPortProfilePtr vport = virDomainNetGetActualVirtPortProfile(net);
        if (!(actualType == VIR_DOMAIN_NET_TYPE_NETWORK ||
              actualType == VIR_DOMAIN_NET_TYPE_BRIDGE ||
              actualType == VIR_DOMAIN_NET_TYPE_ETHERNET)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("filterref is not supported for "
                             "network interfaces of type %s"),
                           virDomainNetTypeToString(actualType));
            return -1;
        }
        if (vport && vport->virtPortType != VIR_NETDEV_VPORT_PROFILE_NONE) {
            /* currently none of the defined virtualport types support iptables */
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("filterref is not supported for "
                             "network interfaces with virtualport type %s"),
                           virNetDevVPortTypeToString(vport->virtPortType));
            return -1;
        }
    }

    if (net->backend.tap &&
        !(actualType == VIR_DOMAIN_NET_TYPE_NETWORK ||
          actualType == VIR_DOMAIN_NET_TYPE_BRIDGE ||
          actualType == VIR_DOMAIN_NET_TYPE_ETHERNET)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Custom tap device path is not supported for: %s"),
                       virDomainNetTypeToString(actualType));
        return -1;
    }

    switch (actualType) {
    case VIR_DOMAIN_NET_TYPE_NETWORK:
    case VIR_DOMAIN_NET_TYPE_BRIDGE:
        tapfdSize = net->driver.virtio.queues;
        if (!tapfdSize)
            tapfdSize = 1;

        if (VIR_ALLOC_N(tapfd, tapfdSize) < 0 ||
            VIR_ALLOC_N(tapfdName, tapfdSize) < 0)
            goto cleanup;

        memset(tapfd, -1, tapfdSize * sizeof(tapfd[0]));

        if (qemuInterfaceBridgeConnect(def, driver, net,
                                       tapfd, &tapfdSize) < 0)
            goto cleanup;
        break;

    case VIR_DOMAIN_NET_TYPE_DIRECT:
        tapfdSize = net->driver.virtio.queues;
        if (!tapfdSize)
            tapfdSize = 1;

        if (VIR_ALLOC_N(tapfd, tapfdSize) < 0 ||
            VIR_ALLOC_N(tapfdName, tapfdSize) < 0)
            goto cleanup;

        memset(tapfd, -1, tapfdSize * sizeof(tapfd[0]));

        if (qemuInterfaceDirectConnect(def, driver, net,
                                       tapfd, tapfdSize, vmop) < 0)
            goto cleanup;
        break;

    case VIR_DOMAIN_NET_TYPE_ETHERNET:
        tapfdSize = net->driver.virtio.queues;
        if (!tapfdSize)
            tapfdSize = 1;

        if (VIR_ALLOC_N(tapfd, tapfdSize) < 0 ||
            VIR_ALLOC_N(tapfdName, tapfdSize) < 0)
            goto cleanup;

        memset(tapfd, -1, tapfdSize * sizeof(tapfd[0]));

        if (qemuInterfaceEthernetConnect(def, driver, net,
                                         tapfd, tapfdSize) < 0)
            goto cleanup;
        break;

    case VIR_DOMAIN_NET_TYPE_HOSTDEV:
        /* NET_TYPE_HOSTDEV devices are really hostdev devices, so
         * their commandlines are constructed with other hostdevs.
         */
        ret = 0;
        goto cleanup;
        break;

    case VIR_DOMAIN_NET_TYPE_VHOSTUSER:
        requireNicdev = true;

        if (net->driver.virtio.queues > 1 &&
            !virQEMUCapsGet(qemuCaps, QEMU_CAPS_VHOSTUSER_MULTIQUEUE)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("multi-queue is not supported for vhost-user "
                             "with this QEMU binary"));
            goto cleanup;
        }

        if (qemuInterfaceVhostuserConnect(driver, logManager, secManager,
                                          cmd, def, net, qemuCaps, &chardev) < 0)
            goto cleanup;

        if (virNetDevOpenvswitchGetVhostuserIfname(net->data.vhostuser->data.nix.path,
                                                   &net->ifname) < 0)
            goto cleanup;

        break;

    case VIR_DOMAIN_NET_TYPE_USER:
    case VIR_DOMAIN_NET_TYPE_SERVER:
    case VIR_DOMAIN_NET_TYPE_CLIENT:
    case VIR_DOMAIN_NET_TYPE_MCAST:
    case VIR_DOMAIN_NET_TYPE_INTERNAL:
    case VIR_DOMAIN_NET_TYPE_UDP:
    case VIR_DOMAIN_NET_TYPE_LAST:
        /* nada */
        break;
    }

    /* For types whose implementations use a netdev on the host, add
     * an entry to nicindexes for passing on to systemd.
    */
    switch ((virDomainNetType)actualType) {
    case VIR_DOMAIN_NET_TYPE_ETHERNET:
    case VIR_DOMAIN_NET_TYPE_NETWORK:
    case VIR_DOMAIN_NET_TYPE_BRIDGE:
    case VIR_DOMAIN_NET_TYPE_DIRECT:
    {
        int nicindex;

        /* network and bridge use a tap device, and direct uses a
         * macvtap device
         */
        if (virQEMUDriverIsPrivileged(driver) && nicindexes && nnicindexes &&
            net->ifname) {
            if (virNetDevGetIndex(net->ifname, &nicindex) < 0 ||
                VIR_APPEND_ELEMENT(*nicindexes, *nnicindexes, nicindex) < 0)
                goto cleanup;
        }
        break;
    }

    case VIR_DOMAIN_NET_TYPE_USER:
    case VIR_DOMAIN_NET_TYPE_VHOSTUSER:
    case VIR_DOMAIN_NET_TYPE_SERVER:
    case VIR_DOMAIN_NET_TYPE_CLIENT:
    case VIR_DOMAIN_NET_TYPE_MCAST:
    case VIR_DOMAIN_NET_TYPE_UDP:
    case VIR_DOMAIN_NET_TYPE_INTERNAL:
    case VIR_DOMAIN_NET_TYPE_HOSTDEV:
    case VIR_DOMAIN_NET_TYPE_LAST:
       /* These types don't use a network device on the host, but
        * instead use some other type of connection to the emulated
        * device in the qemu process.
        *
        * (Note that hostdev can't be considered as "using a network
        * device", because by the time it is being used, it has been
        * detached from the hostside network driver so it doesn't show
        * up in the list of interfaces on the host - it's just some
        * PCI device.)
        */
       break;
    }

    /* Set bandwidth or warn if requested and not supported. */
    actualBandwidth = virDomainNetGetActualBandwidth(net);
    if (actualBandwidth) {
        if (virNetDevSupportBandwidth(actualType)) {
            if (virNetDevBandwidthSet(net->ifname, actualBandwidth, false,
                                      !virDomainNetTypeSharesHostView(net)) < 0)
                goto cleanup;
        } else {
            VIR_WARN("setting bandwidth on interfaces of "
                     "type '%s' is not implemented yet",
                     virDomainNetTypeToString(actualType));
        }
    }

    if (net->mtu &&
        virNetDevSetMTU(net->ifname, net->mtu) < 0)
        goto cleanup;

    if ((actualType == VIR_DOMAIN_NET_TYPE_NETWORK ||
         actualType == VIR_DOMAIN_NET_TYPE_BRIDGE ||
         actualType == VIR_DOMAIN_NET_TYPE_ETHERNET ||
         actualType == VIR_DOMAIN_NET_TYPE_DIRECT) &&
        !standalone) {
        /* Attempt to use vhost-net mode for these types of
           network device */
        vhostfdSize = net->driver.virtio.queues;
        if (!vhostfdSize)
            vhostfdSize = 1;

        if (VIR_ALLOC_N(vhostfd, vhostfdSize) < 0 ||
            VIR_ALLOC_N(vhostfdName, vhostfdSize))
            goto cleanup;

        memset(vhostfd, -1, vhostfdSize * sizeof(vhostfd[0]));

        if (qemuInterfaceOpenVhostNet(def, net, vhostfd, &vhostfdSize) < 0)
            goto cleanup;
    }

    for (i = 0; i < tapfdSize; i++) {
        if (qemuSecuritySetTapFDLabel(driver->securityManager,
                                      def, tapfd[i]) < 0)
            goto cleanup;
        if (virAsprintf(&tapfdName[i], "%d", tapfd[i]) < 0)
            goto cleanup;
        virCommandPassFD(cmd, tapfd[i],
                         VIR_COMMAND_PASS_FD_CLOSE_PARENT);
        tapfd[i] = -1;
    }

    for (i = 0; i < vhostfdSize; i++) {
        if (virAsprintf(&vhostfdName[i], "%d", vhostfd[i]) < 0)
            goto cleanup;
        virCommandPassFD(cmd, vhostfd[i],
                         VIR_COMMAND_PASS_FD_CLOSE_PARENT);
        vhostfd[i] = -1;
    }

    if (chardev)
        virCommandAddArgList(cmd, "-chardev", chardev, NULL);

    if (!(host = qemuBuildHostNetStr(net, driver,
                                     tapfdName, tapfdSize,
                                     vhostfdName, vhostfdSize)))
        goto cleanup;
    virCommandAddArgList(cmd, "-netdev", host, NULL);

    /* Possible combinations:
     *
     *   Old way: -netdev type=tap,id=netdev1 \
     *              -net nic,model=e1000,netdev=netdev1
     *   New way: -netdev type=tap,id=netdev1 -device e1000,id=netdev1
     */
    if (qemuDomainSupportsNicdev(def, net)) {
        if (qemuCommandAddExtDevice(cmd, &net->info) < 0)
            goto cleanup;

        if (!(nic = qemuBuildNicDevStr(def, net, bootindex,
                                       net->driver.virtio.queues, qemuCaps)))
            goto cleanup;
        virCommandAddArgList(cmd, "-device", nic, NULL);
    } else if (!requireNicdev) {
        if (qemuCommandAddExtDevice(cmd, &net->info) < 0)
            goto cleanup;

        if (!(nic = qemuBuildLegacyNicStr(net)))
            goto cleanup;
        virCommandAddArgList(cmd, "-net", nic, NULL);
    } else {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("Nicdev support unavailable"));
        goto cleanup;
    }

    ret = 0;
 cleanup:
    if (ret < 0) {
        virErrorPtr saved_err = virSaveLastError();
        virDomainConfNWFilterTeardown(net);
        virSetError(saved_err);
        virFreeError(saved_err);
    }
    for (i = 0; vhostfd && i < vhostfdSize; i++) {
        if (ret < 0)
            VIR_FORCE_CLOSE(vhostfd[i]);
        if (vhostfdName)
            VIR_FREE(vhostfdName[i]);
    }
    VIR_FREE(vhostfdName);
    for (i = 0; tapfd && i < tapfdSize; i++) {
        if (ret < 0)
            VIR_FORCE_CLOSE(tapfd[i]);
        if (tapfdName)
            VIR_FREE(tapfdName[i]);
    }
    VIR_FREE(tapfdName);
    VIR_FREE(vhostfd);
    VIR_FREE(tapfd);
    VIR_FREE(chardev);
    VIR_FREE(host);
    VIR_FREE(nic);
    return ret;
}


/* NOTE: Not using const virDomainDef here since eventually a call is made
 *       into qemuSecuritySetTapFDLabel which calls it's driver
 *       API domainSetSecurityTapFDLabel that doesn't use the const format.
 */
static int
qemuBuildNetCommandLine(virQEMUDriverPtr driver,
                        virLogManagerPtr logManager,
                        virSecurityManagerPtr secManager,
                        virCommandPtr cmd,
                        virDomainDefPtr def,
                        virQEMUCapsPtr qemuCaps,
                        virNetDevVPortProfileOp vmop,
                        bool standalone,
                        size_t *nnicindexes,
                        int **nicindexes,
                        unsigned int *bootHostdevNet)
{
    size_t i;
    int last_good_net = -1;
    virErrorPtr originalError = NULL;

    if (def->nnets) {
        unsigned int bootNet = 0;

        /* convert <boot dev='network'/> to bootindex since we didn't emit -boot n */
        for (i = 0; i < def->os.nBootDevs; i++) {
            if (def->os.bootDevs[i] == VIR_DOMAIN_BOOT_NET) {
                bootNet = i + 1;
                break;
            }
        }

        for (i = 0; i < def->nnets; i++) {
            virDomainNetDefPtr net = def->nets[i];

            if (qemuBuildInterfaceCommandLine(driver, logManager, secManager, cmd, def, net,
                                              qemuCaps, bootNet, vmop,
                                              standalone, nnicindexes,
                                              nicindexes) < 0)
                goto error;

            last_good_net = i;
            /* if this interface is a type='hostdev' interface and we
             * haven't yet added a "bootindex" parameter to an
             * emulated network device, save the bootindex - hostdev
             * interface commandlines will be built later on when we
             * cycle through all the hostdevs, and we'll use it then.
             */
            if (virDomainNetGetActualType(net) == VIR_DOMAIN_NET_TYPE_HOSTDEV &&
                *bootHostdevNet == 0) {
                *bootHostdevNet = bootNet;
            }
            bootNet = 0;
        }
    }
    return 0;

 error:
    /* free up any resources in the network driver
     * but don't overwrite the original error */
    originalError = virSaveLastError();
    for (i = 0; last_good_net != -1 && i <= last_good_net; i++)
        virDomainConfNWFilterTeardown(def->nets[i]);
    virSetError(originalError);
    virFreeError(originalError);
    return -1;
}


static const char *
qemuBuildSmartcardFindCCIDController(const virDomainDef *def,
                                     const virDomainSmartcardDef *smartcard)
{
    size_t i;

    /* Should never happen. But doesn't hurt to check. */
    if (smartcard->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_CCID)
        return NULL;

    for (i = 0; i < def->ncontrollers; i++) {
        const virDomainControllerDef *tmp = def->controllers[i];

        if (tmp->type != VIR_DOMAIN_CONTROLLER_TYPE_CCID)
            continue;

        if (tmp->idx != smartcard->info.addr.ccid.controller)
            continue;

        return tmp->info.alias;
    }

    return NULL;
}


static int
qemuBuildSmartcardCommandLine(virLogManagerPtr logManager,
                              virSecurityManagerPtr secManager,
                              virCommandPtr cmd,
                              virQEMUDriverConfigPtr cfg,
                              const virDomainDef *def,
                              virQEMUCapsPtr qemuCaps,
                              bool chardevStdioLogd)
{
    size_t i;
    virDomainSmartcardDefPtr smartcard;
    char *devstr;
    virBuffer opt = VIR_BUFFER_INITIALIZER;
    const char *database;
    const char *contAlias = NULL;
    unsigned int cdevflags = QEMU_BUILD_CHARDEV_TCP_NOWAIT |
        QEMU_BUILD_CHARDEV_UNIX_FD_PASS;
    if (chardevStdioLogd)
        cdevflags |= QEMU_BUILD_CHARDEV_FILE_LOGD;

    if (!def->nsmartcards)
        return 0;

    smartcard = def->smartcards[0];

    /* -device usb-ccid was already emitted along with other
     * controllers.  For now, qemu handles only one smartcard.  */
    if (def->nsmartcards > 1 ||
        smartcard->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_CCID ||
        smartcard->info.addr.ccid.controller != 0 ||
        smartcard->info.addr.ccid.slot != 0) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("this QEMU binary lacks multiple smartcard "
                         "support"));
        virBufferFreeAndReset(&opt);
        return -1;
    }

    switch (smartcard->type) {
    case VIR_DOMAIN_SMARTCARD_TYPE_HOST:
        if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_CCID_EMULATED)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("this QEMU binary lacks smartcard host "
                             "mode support"));
            return -1;
        }

        virBufferAddLit(&opt, "ccid-card-emulated,backend=nss-emulated");
        break;

    case VIR_DOMAIN_SMARTCARD_TYPE_HOST_CERTIFICATES:
        if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_CCID_EMULATED)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("this QEMU binary lacks smartcard host "
                             "mode support"));
            return -1;
        }

        virBufferAddLit(&opt, "ccid-card-emulated,backend=certificates");
        for (i = 0; i < VIR_DOMAIN_SMARTCARD_NUM_CERTIFICATES; i++) {
            virBufferAsprintf(&opt, ",cert%zu=", i + 1);
            virQEMUBuildBufferEscapeComma(&opt, smartcard->data.cert.file[i]);
        }
        if (smartcard->data.cert.database) {
            database = smartcard->data.cert.database;
        } else {
            database = VIR_DOMAIN_SMARTCARD_DEFAULT_DATABASE;
        }
        virBufferAddLit(&opt, ",db=");
        virQEMUBuildBufferEscapeComma(&opt, database);
        break;

    case VIR_DOMAIN_SMARTCARD_TYPE_PASSTHROUGH:
        if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_CCID_PASSTHRU)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("this QEMU binary lacks smartcard "
                             "passthrough mode support"));
            return -1;
        }

        if (!(devstr = qemuBuildChrChardevStr(logManager, secManager,
                                              cmd, cfg, def,
                                              smartcard->data.passthru,
                                              smartcard->info.alias,
                                              qemuCaps, cdevflags))) {
            virBufferFreeAndReset(&opt);
            return -1;
        }
        virCommandAddArg(cmd, "-chardev");
        virCommandAddArg(cmd, devstr);
        VIR_FREE(devstr);

        virBufferAsprintf(&opt, "ccid-card-passthru,chardev=char%s",
                          smartcard->info.alias);
        break;

    default:
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unexpected smartcard type %d"),
                       smartcard->type);
        virBufferFreeAndReset(&opt);
        return -1;
    }

    if (!(contAlias = qemuBuildSmartcardFindCCIDController(def,
                                                           smartcard))) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Unable to find controller for %s"),
                       smartcard->info.alias);
        return -1;
    }

    virCommandAddArg(cmd, "-device");
    virBufferAsprintf(&opt, ",id=%s,bus=%s.0", smartcard->info.alias, contAlias);
    virCommandAddArgBuffer(cmd, &opt);

    return 0;
}


static char *
qemuBuildShmemDevLegacyStr(virDomainDefPtr def,
                           virDomainShmemDefPtr shmem,
                           virQEMUCapsPtr qemuCaps)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;

    if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE_IVSHMEM)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("ivshmem device is not supported "
                         "with this QEMU binary"));
        return NULL;
    }

    virBufferAddLit(&buf, "ivshmem");
    virBufferAsprintf(&buf, ",id=%s", shmem->info.alias);

    if (shmem->size)
        virBufferAsprintf(&buf, ",size=%llum", shmem->size >> 20);

    if (!shmem->server.enabled) {
        virBufferAsprintf(&buf, ",shm=%s", shmem->name);
    } else {
        virBufferAsprintf(&buf, ",chardev=char%s", shmem->info.alias);
        if (shmem->msi.enabled) {
            virBufferAddLit(&buf, ",msi=on");
            if (shmem->msi.vectors)
                virBufferAsprintf(&buf, ",vectors=%u", shmem->msi.vectors);
            if (shmem->msi.ioeventfd)
                virBufferAsprintf(&buf, ",ioeventfd=%s",
                                  virTristateSwitchTypeToString(shmem->msi.ioeventfd));
        }
    }

    if (qemuBuildDeviceAddressStr(&buf, def, &shmem->info, qemuCaps) < 0)
        goto error;

    if (virBufferCheckError(&buf) < 0)
        goto error;

    return virBufferContentAndReset(&buf);

 error:
    virBufferFreeAndReset(&buf);
    return NULL;
}

char *
qemuBuildShmemDevStr(virDomainDefPtr def,
                     virDomainShmemDefPtr shmem,
                     virQEMUCapsPtr qemuCaps)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;

    if ((shmem->model == VIR_DOMAIN_SHMEM_MODEL_IVSHMEM_PLAIN &&
         !virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE_IVSHMEM_PLAIN)) ||
        (shmem->model == VIR_DOMAIN_SHMEM_MODEL_IVSHMEM_DOORBELL &&
         !virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE_IVSHMEM_DOORBELL))) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("shmem model '%s' is not supported "
                             "by this QEMU binary"),
                           virDomainShmemModelTypeToString(shmem->model));
        return NULL;
    }

    virBufferAdd(&buf, virDomainShmemModelTypeToString(shmem->model), -1);
    virBufferAsprintf(&buf, ",id=%s", shmem->info.alias);

    if (shmem->server.enabled)
        virBufferAsprintf(&buf, ",chardev=char%s", shmem->info.alias);
    else
        virBufferAsprintf(&buf, ",memdev=shmmem-%s", shmem->info.alias);

    if (shmem->msi.vectors)
        virBufferAsprintf(&buf, ",vectors=%u", shmem->msi.vectors);
    if (shmem->msi.ioeventfd) {
        virBufferAsprintf(&buf, ",ioeventfd=%s",
                          virTristateSwitchTypeToString(shmem->msi.ioeventfd));
    }

    if (qemuBuildDeviceAddressStr(&buf, def, &shmem->info, qemuCaps) < 0) {
        virBufferFreeAndReset(&buf);
        return NULL;
    }

    if (virBufferCheckError(&buf) < 0)
        return NULL;

    return virBufferContentAndReset(&buf);
}


virJSONValuePtr
qemuBuildShmemBackendMemProps(virDomainShmemDefPtr shmem)
{
    char *mem_alias = NULL;
    char *mem_path = NULL;
    virJSONValuePtr ret = NULL;

    if (virAsprintf(&mem_path, "/dev/shm/%s", shmem->name) < 0)
        return NULL;

    if (virAsprintf(&mem_alias, "shmmem-%s", shmem->info.alias) < 0)
        goto cleanup;

    qemuMonitorCreateObjectProps(&ret, "memory-backend-file", mem_alias,
                                 "s:mem-path", mem_path,
                                 "U:size", shmem->size,
                                 "b:share", true,
                                 NULL);

 cleanup:
    VIR_FREE(mem_alias);
    VIR_FREE(mem_path);
    return ret;
}


static int
qemuBuildShmemCommandLine(virLogManagerPtr logManager,
                          virSecurityManagerPtr secManager,
                          virCommandPtr cmd,
                          virQEMUDriverConfigPtr cfg,
                          virDomainDefPtr def,
                          virDomainShmemDefPtr shmem,
                          virQEMUCapsPtr qemuCaps,
                          bool chardevStdioLogd)
{
    virJSONValuePtr memProps = NULL;
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    char *devstr = NULL;
    int rc;
    unsigned int cdevflags = QEMU_BUILD_CHARDEV_TCP_NOWAIT |
        QEMU_BUILD_CHARDEV_UNIX_FD_PASS;
    if (chardevStdioLogd)
        cdevflags |= QEMU_BUILD_CHARDEV_FILE_LOGD;

    if (shmem->size) {
        /*
         * Thanks to our parsing code, we have a guarantee that the
         * size is power of two and is at least a mebibyte in size.
         * But because it may change in the future, the checks are
         * doubled in here.
         */
        if (shmem->size & (shmem->size - 1)) {
            virReportError(VIR_ERR_XML_ERROR, "%s",
                           _("shmem size must be a power of two"));
            return -1;
        }
        if (shmem->size < 1024 * 1024) {
            virReportError(VIR_ERR_XML_ERROR, "%s",
                           _("shmem size must be at least 1 MiB (1024 KiB)"));
            return -1;
        }
    }

    if (shmem->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("only 'pci' addresses are supported for the "
                         "shared memory device"));
        return -1;
    }

    switch ((virDomainShmemModel)shmem->model) {
    case VIR_DOMAIN_SHMEM_MODEL_IVSHMEM:
        devstr = qemuBuildShmemDevLegacyStr(def, shmem, qemuCaps);
        break;

    case VIR_DOMAIN_SHMEM_MODEL_IVSHMEM_PLAIN:
        if (!(memProps = qemuBuildShmemBackendMemProps(shmem)))
            return -1;

        rc = virQEMUBuildObjectCommandlineFromJSON(&buf, memProps);
        virJSONValueFree(memProps);

        if (rc < 0)
            return -1;

        virCommandAddArg(cmd, "-object");
        virCommandAddArgBuffer(cmd, &buf);

        ATTRIBUTE_FALLTHROUGH;
    case VIR_DOMAIN_SHMEM_MODEL_IVSHMEM_DOORBELL:
        devstr = qemuBuildShmemDevStr(def, shmem, qemuCaps);
        break;

    case VIR_DOMAIN_SHMEM_MODEL_LAST:
        break;
    }

    if (!devstr)
        return -1;

    if (qemuCommandAddExtDevice(cmd, &shmem->info) < 0) {
        VIR_FREE(devstr);
        return -1;
    }

    virCommandAddArgList(cmd, "-device", devstr, NULL);
    VIR_FREE(devstr);

    if (shmem->server.enabled) {
        devstr = qemuBuildChrChardevStr(logManager, secManager,
                                        cmd, cfg, def,
                                        &shmem->server.chr,
                                        shmem->info.alias, qemuCaps,
                                        cdevflags);
        if (!devstr)
            return -1;

        virCommandAddArgList(cmd, "-chardev", devstr, NULL);
        VIR_FREE(devstr);
    }

    return 0;
}


static virQEMUCapsFlags
qemuChrSerialTargetModelToCaps(virDomainChrSerialTargetModel targetModel)
{
    switch (targetModel) {
    case VIR_DOMAIN_CHR_SERIAL_TARGET_MODEL_ISA_SERIAL:
        return QEMU_CAPS_DEVICE_ISA_SERIAL;
    case VIR_DOMAIN_CHR_SERIAL_TARGET_MODEL_USB_SERIAL:
        return QEMU_CAPS_DEVICE_USB_SERIAL;
    case VIR_DOMAIN_CHR_SERIAL_TARGET_MODEL_PCI_SERIAL:
        return QEMU_CAPS_DEVICE_PCI_SERIAL;
    case VIR_DOMAIN_CHR_SERIAL_TARGET_MODEL_SPAPR_VTY:
        return QEMU_CAPS_DEVICE_SPAPR_VTY;
    case VIR_DOMAIN_CHR_SERIAL_TARGET_MODEL_SCLPCONSOLE:
        return QEMU_CAPS_DEVICE_SCLPCONSOLE;
    case VIR_DOMAIN_CHR_SERIAL_TARGET_MODEL_SCLPLMCONSOLE:
        return QEMU_CAPS_DEVICE_SCLPLMCONSOLE;
    case VIR_DOMAIN_CHR_SERIAL_TARGET_MODEL_PL011:
        return QEMU_CAPS_DEVICE_PL011;
    case VIR_DOMAIN_CHR_SERIAL_TARGET_MODEL_16550A:
    case VIR_DOMAIN_CHR_SERIAL_TARGET_MODEL_NONE:
    case VIR_DOMAIN_CHR_SERIAL_TARGET_MODEL_LAST:
        break;
    }

    return 0;
}


static int
qemuBuildChrDeviceCommandLine(virCommandPtr cmd,
                              const virDomainDef *def,
                              virDomainChrDefPtr chr,
                              virQEMUCapsPtr qemuCaps)
{
    char *devstr = NULL;

    if (qemuBuildChrDeviceStr(&devstr, def, chr, qemuCaps) < 0)
        return -1;

    virCommandAddArgList(cmd, "-device", devstr, NULL);
    VIR_FREE(devstr);
    return 0;
}


static bool
qemuChrIsPlatformDevice(const virDomainDef *def,
                        virDomainChrDefPtr chr)
{
    if (def->os.arch == VIR_ARCH_ARMV6L ||
        def->os.arch == VIR_ARCH_ARMV7L ||
        def->os.arch == VIR_ARCH_AARCH64) {

        /* pl011 (used on mach-virt) is a platform device */
        if (chr->deviceType == VIR_DOMAIN_CHR_DEVICE_TYPE_SERIAL &&
            chr->targetType == VIR_DOMAIN_CHR_SERIAL_TARGET_TYPE_SYSTEM &&
            chr->targetModel == VIR_DOMAIN_CHR_SERIAL_TARGET_MODEL_PL011) {
            return true;
        }
    }

    if (ARCH_IS_RISCV(def->os.arch)) {

        /* 16550a (used by riscv/virt guests) is a platform device */
        if (chr->deviceType == VIR_DOMAIN_CHR_DEVICE_TYPE_SERIAL &&
            chr->targetType == VIR_DOMAIN_CHR_SERIAL_TARGET_TYPE_SYSTEM &&
            chr->targetModel == VIR_DOMAIN_CHR_SERIAL_TARGET_MODEL_16550A) {
            return true;
        }
    }

    /* If we got all the way here and we're still stuck with the default
     * target type for a serial device, it means we have no clue what kind of
     * device we're talking about and we must treat it as a platform device. */
    if (chr->deviceType == VIR_DOMAIN_CHR_DEVICE_TYPE_SERIAL &&
        chr->targetType == VIR_DOMAIN_CHR_SERIAL_TARGET_TYPE_NONE) {
        return true;
    }

    return false;
}


static int
qemuBuildSerialCommandLine(virLogManagerPtr logManager,
                           virSecurityManagerPtr secManager,
                           virCommandPtr cmd,
                           virQEMUDriverConfigPtr cfg,
                           const virDomainDef *def,
                           virQEMUCapsPtr qemuCaps,
                           bool chardevStdioLogd)
{
    size_t i;
    bool havespice = false;
    unsigned int cdevflags = QEMU_BUILD_CHARDEV_TCP_NOWAIT |
        QEMU_BUILD_CHARDEV_UNIX_FD_PASS;
    if (chardevStdioLogd)
        cdevflags |= QEMU_BUILD_CHARDEV_FILE_LOGD;

    if (def->nserials) {
        for (i = 0; i < def->ngraphics && !havespice; i++) {
            if (def->graphics[i]->type == VIR_DOMAIN_GRAPHICS_TYPE_SPICE)
                havespice = true;
        }
    }

    for (i = 0; i < def->nserials; i++) {
        virDomainChrDefPtr serial = def->serials[i];
        char *devstr;

        if (serial->source->type == VIR_DOMAIN_CHR_TYPE_SPICEPORT && !havespice)
            continue;

        if (!(devstr = qemuBuildChrChardevStr(logManager, secManager,
                                              cmd, cfg, def,
                                              serial->source,
                                              serial->info.alias,
                                              qemuCaps, cdevflags)))
            return -1;
        virCommandAddArg(cmd, "-chardev");
        virCommandAddArg(cmd, devstr);
        VIR_FREE(devstr);

        /* If the device is not a platform device, build the devstr */
        if (!qemuChrIsPlatformDevice(def, serial)) {
            if (qemuBuildChrDeviceCommandLine(cmd, def, serial, qemuCaps) < 0)
                return -1;
        } else {
            virQEMUCapsFlags caps;

            caps = qemuChrSerialTargetModelToCaps(serial->targetModel);

            if (caps && !virQEMUCapsGet(qemuCaps, caps)) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                               _("'%s' is not supported in this QEMU binary"),
                               virDomainChrSerialTargetModelTypeToString(serial->targetModel));
                return -1;
            }

            virCommandAddArg(cmd, "-serial");
            virCommandAddArgFormat(cmd, "chardev:char%s", serial->info.alias);
        }
    }

    return 0;
}


static int
qemuBuildParallelsCommandLine(virLogManagerPtr logManager,
                              virSecurityManagerPtr secManager,
                              virCommandPtr cmd,
                              virQEMUDriverConfigPtr cfg,
                              const virDomainDef *def,
                              virQEMUCapsPtr qemuCaps,
                              bool chardevStdioLogd)
{
    size_t i;
    unsigned int cdevflags = QEMU_BUILD_CHARDEV_TCP_NOWAIT |
        QEMU_BUILD_CHARDEV_UNIX_FD_PASS;
    if (chardevStdioLogd)
        cdevflags |= QEMU_BUILD_CHARDEV_FILE_LOGD;

    for (i = 0; i < def->nparallels; i++) {
        virDomainChrDefPtr parallel = def->parallels[i];
        char *devstr;

        if (!(devstr = qemuBuildChrChardevStr(logManager, secManager,
                                              cmd, cfg, def,
                                              parallel->source,
                                              parallel->info.alias,
                                              qemuCaps, cdevflags)))
            return -1;
        virCommandAddArg(cmd, "-chardev");
        virCommandAddArg(cmd, devstr);
        VIR_FREE(devstr);

        if (qemuBuildChrDeviceCommandLine(cmd, def, parallel,
                                          qemuCaps) < 0)
            return -1;
    }

    return 0;
}


static int
qemuBuildChannelsCommandLine(virLogManagerPtr logManager,
                             virSecurityManagerPtr secManager,
                             virCommandPtr cmd,
                             virQEMUDriverConfigPtr cfg,
                             const virDomainDef *def,
                             virQEMUCapsPtr qemuCaps,
                             bool chardevStdioLogd)
{
    size_t i;
    unsigned int cdevflags = QEMU_BUILD_CHARDEV_TCP_NOWAIT |
        QEMU_BUILD_CHARDEV_UNIX_FD_PASS;
    if (chardevStdioLogd)
        cdevflags |= QEMU_BUILD_CHARDEV_FILE_LOGD;

    for (i = 0; i < def->nchannels; i++) {
        virDomainChrDefPtr channel = def->channels[i];
        char *devstr;

        switch (channel->targetType) {
        case VIR_DOMAIN_CHR_CHANNEL_TARGET_TYPE_GUESTFWD:
            if (!(devstr = qemuBuildChrChardevStr(logManager, secManager,
                                                  cmd, cfg, def,
                                                  channel->source,
                                                  channel->info.alias,
                                                  qemuCaps, cdevflags)))
                return -1;
            virCommandAddArg(cmd, "-chardev");
            virCommandAddArg(cmd, devstr);
            VIR_FREE(devstr);

            if (qemuBuildChrDeviceStr(&devstr, def, channel, qemuCaps) < 0)
                return -1;
            virCommandAddArgList(cmd, "-netdev", devstr, NULL);
            VIR_FREE(devstr);
            break;

        case VIR_DOMAIN_CHR_CHANNEL_TARGET_TYPE_VIRTIO:
            if (!(devstr = qemuBuildChrChardevStr(logManager, secManager,
                                                  cmd, cfg, def,
                                                  channel->source,
                                                  channel->info.alias,
                                                  qemuCaps, cdevflags)))
                return -1;
            virCommandAddArg(cmd, "-chardev");
            virCommandAddArg(cmd, devstr);
            VIR_FREE(devstr);

            if (qemuBuildChrDeviceCommandLine(cmd, def, channel, qemuCaps) < 0)
                return -1;
            break;
        }
    }

    return 0;
}


static int
qemuBuildConsoleCommandLine(virLogManagerPtr logManager,
                            virSecurityManagerPtr secManager,
                            virCommandPtr cmd,
                            virQEMUDriverConfigPtr cfg,
                            const virDomainDef *def,
                            virQEMUCapsPtr qemuCaps,
                            bool chardevStdioLogd)
{
    size_t i;
    unsigned int cdevflags = QEMU_BUILD_CHARDEV_TCP_NOWAIT |
        QEMU_BUILD_CHARDEV_UNIX_FD_PASS;
    if (chardevStdioLogd)
        cdevflags |= QEMU_BUILD_CHARDEV_FILE_LOGD;

    /* Explicit console devices */
    for (i = 0; i < def->nconsoles; i++) {
        virDomainChrDefPtr console = def->consoles[i];
        char *devstr;

        switch (console->targetType) {
        case VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_SCLP:
            if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE_SCLPCONSOLE)) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("sclpconsole is not supported in this QEMU binary"));
                return -1;
            }

            if (!(devstr = qemuBuildChrChardevStr(logManager, secManager,
                                                  cmd, cfg, def,
                                                  console->source,
                                                  console->info.alias,
                                                  qemuCaps, cdevflags)))
                return -1;
            virCommandAddArg(cmd, "-chardev");
            virCommandAddArg(cmd, devstr);
            VIR_FREE(devstr);

            if (qemuBuildChrDeviceCommandLine(cmd, def, console, qemuCaps) < 0)
                return -1;
            break;

        case VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_SCLPLM:
            if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE_SCLPLMCONSOLE)) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("sclplmconsole is not supported in this QEMU binary"));
                return -1;
            }

            if (!(devstr = qemuBuildChrChardevStr(logManager, secManager,
                                                  cmd, cfg, def,
                                                  console->source,
                                                  console->info.alias,
                                                  qemuCaps, cdevflags)))
                return -1;
            virCommandAddArg(cmd, "-chardev");
            virCommandAddArg(cmd, devstr);
            VIR_FREE(devstr);

            if (qemuBuildChrDeviceCommandLine(cmd, def, console, qemuCaps) < 0)
                return -1;
            break;

        case VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_VIRTIO:
            if (!(devstr = qemuBuildChrChardevStr(logManager, secManager,
                                                  cmd, cfg, def,
                                                  console->source,
                                                  console->info.alias,
                                                  qemuCaps, cdevflags)))
                return -1;
            virCommandAddArg(cmd, "-chardev");
            virCommandAddArg(cmd, devstr);
            VIR_FREE(devstr);

            if (qemuBuildChrDeviceCommandLine(cmd, def, console, qemuCaps) < 0)
                return -1;
            break;

        case VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_SERIAL:
            break;

        default:
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("unsupported console target type %s"),
                           NULLSTR(virDomainChrConsoleTargetTypeToString(console->targetType)));
            return -1;
        }
    }

    return 0;
}


char *
qemuBuildRedirdevDevStr(const virDomainDef *def,
                        virDomainRedirdevDefPtr dev,
                        virQEMUCapsPtr qemuCaps)
{
    size_t i;
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    virDomainRedirFilterDefPtr redirfilter = def->redirfilter;

    if (dev->bus != VIR_DOMAIN_REDIRDEV_BUS_USB) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Redirection bus %s is not supported by QEMU"),
                       virDomainRedirdevBusTypeToString(dev->bus));
        goto error;
    }

    if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_USB_REDIR)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("USB redirection is not supported "
                         "by this version of QEMU"));
        goto error;
    }

    virBufferAsprintf(&buf, "usb-redir,chardev=char%s,id=%s",
                      dev->info.alias, dev->info.alias);

    if (redirfilter && redirfilter->nusbdevs) {
        if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_USB_REDIR_FILTER)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("USB redirection filter is not "
                             "supported by this version of QEMU"));
            goto error;
        }

        virBufferAddLit(&buf, ",filter=");

        for (i = 0; i < redirfilter->nusbdevs; i++) {
            virDomainRedirFilterUSBDevDefPtr usbdev = redirfilter->usbdevs[i];
            if (usbdev->usbClass >= 0)
                virBufferAsprintf(&buf, "0x%02X:", usbdev->usbClass);
            else
                virBufferAddLit(&buf, "-1:");

            if (usbdev->vendor >= 0)
                virBufferAsprintf(&buf, "0x%04X:", usbdev->vendor);
            else
                virBufferAddLit(&buf, "-1:");

            if (usbdev->product >= 0)
                virBufferAsprintf(&buf, "0x%04X:", usbdev->product);
            else
                virBufferAddLit(&buf, "-1:");

            if (usbdev->version >= 0)
                virBufferAsprintf(&buf, "0x%04X:", usbdev->version);
            else
                virBufferAddLit(&buf, "-1:");

            virBufferAsprintf(&buf, "%u", usbdev->allow);
            if (i < redirfilter->nusbdevs -1)
                virBufferAddLit(&buf, "|");
        }
    }

    if (dev->info.bootIndex)
        virBufferAsprintf(&buf, ",bootindex=%u", dev->info.bootIndex);

    if (qemuBuildDeviceAddressStr(&buf, def, &dev->info, qemuCaps) < 0)
        goto error;

    if (virBufferCheckError(&buf) < 0)
        goto error;

    return virBufferContentAndReset(&buf);

 error:
    virBufferFreeAndReset(&buf);
    return NULL;
}


static int
qemuBuildRedirdevCommandLine(virLogManagerPtr logManager,
                             virSecurityManagerPtr secManager,
                             virCommandPtr cmd,
                             virQEMUDriverConfigPtr cfg,
                             const virDomainDef *def,
                             virQEMUCapsPtr qemuCaps,
                             bool chardevStdioLogd)
{
    size_t i;
    unsigned int cdevflags = QEMU_BUILD_CHARDEV_TCP_NOWAIT |
        QEMU_BUILD_CHARDEV_UNIX_FD_PASS;
    if (chardevStdioLogd)
        cdevflags |= QEMU_BUILD_CHARDEV_FILE_LOGD;

    for (i = 0; i < def->nredirdevs; i++) {
        virDomainRedirdevDefPtr redirdev = def->redirdevs[i];
        char *devstr;

        if (!(devstr = qemuBuildChrChardevStr(logManager, secManager,
                                              cmd, cfg, def,
                                              redirdev->source,
                                              redirdev->info.alias,
                                              qemuCaps, cdevflags))) {
            return -1;
        }

        virCommandAddArg(cmd, "-chardev");
        virCommandAddArg(cmd, devstr);
        VIR_FREE(devstr);

        virCommandAddArg(cmd, "-device");
        if (!(devstr = qemuBuildRedirdevDevStr(def, redirdev, qemuCaps)))
            return -1;
        virCommandAddArg(cmd, devstr);
        VIR_FREE(devstr);
    }

    return 0;
}


static void
qemuBuildDomainLoaderCommandLine(virCommandPtr cmd,
                                 virDomainDefPtr def)
{
    virDomainLoaderDefPtr loader = def->os.loader;
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    int unit = 0;

    if (!loader)
        return;

    switch ((virDomainLoader) loader->type) {
    case VIR_DOMAIN_LOADER_TYPE_ROM:
        virCommandAddArg(cmd, "-bios");
        virCommandAddArg(cmd, loader->path);
        break;

    case VIR_DOMAIN_LOADER_TYPE_PFLASH:

        if (loader->secure == VIR_TRISTATE_BOOL_YES) {
            virCommandAddArgList(cmd,
                                 "-global",
                                 "driver=cfi.pflash01,property=secure,value=on",
                                 NULL);
        }

        virBufferAddLit(&buf, "file=");
        virQEMUBuildBufferEscapeComma(&buf, loader->path);
        virBufferAsprintf(&buf, ",if=pflash,format=raw,unit=%d", unit);
        unit++;

        if (loader->readonly) {
            virBufferAsprintf(&buf, ",readonly=%s",
                              virTristateSwitchTypeToString(loader->readonly));
        }

        virCommandAddArg(cmd, "-drive");
        virCommandAddArgBuffer(cmd, &buf);

        if (loader->nvram) {
            virBufferFreeAndReset(&buf);
            virBufferAddLit(&buf, "file=");
            virQEMUBuildBufferEscapeComma(&buf, loader->nvram);
            virBufferAsprintf(&buf, ",if=pflash,format=raw,unit=%d", unit);

            virCommandAddArg(cmd, "-drive");
            virCommandAddArgBuffer(cmd, &buf);
        }
        break;

    case VIR_DOMAIN_LOADER_TYPE_NONE:
    case VIR_DOMAIN_LOADER_TYPE_LAST:
        /* nada */
        break;
    }

    virBufferFreeAndReset(&buf);
}


static char *
qemuBuildTPMDevStr(const virDomainDef *def,
                   virQEMUCapsPtr qemuCaps)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    const virDomainTPMDef *tpm = def->tpm;
    const char *model = virDomainTPMModelTypeToString(tpm->model);
    virQEMUCapsFlags flag;

    switch (tpm->model) {
    case VIR_DOMAIN_TPM_MODEL_TIS:
        flag = QEMU_CAPS_DEVICE_TPM_TIS;
        break;
    case VIR_DOMAIN_TPM_MODEL_CRB:
        flag = QEMU_CAPS_DEVICE_TPM_CRB;
        break;
    case VIR_DOMAIN_TPM_MODEL_LAST:
    default:
        virReportEnumRangeError(virDomainTPMModel, tpm->model);
        goto error;
    }

    if (!virQEMUCapsGet(qemuCaps, flag)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("The QEMU executable %s does not support TPM "
                       "model %s"),
                       def->emulator, model);
        goto error;
    }

    virBufferAsprintf(&buf, "%s,tpmdev=tpm-%s,id=%s",
                      model, tpm->info.alias, tpm->info.alias);

    if (virBufferCheckError(&buf) < 0)
        goto error;

    return virBufferContentAndReset(&buf);

 error:
    virBufferFreeAndReset(&buf);
    return NULL;
}


/* this function is exported so that tests can mock the FDs */
int
qemuBuildTPMOpenBackendFDs(const char *tpmdev,
                           const char *cancel_path,
                           int *tpmfd,
                           int *cancelfd)
{
    if ((*tpmfd = open(tpmdev, O_RDWR)) < 0) {
        virReportSystemError(errno, _("Could not open TPM device %s"),
                             tpmdev);
        return -1;
    }

    if ((*cancelfd = open(cancel_path, O_WRONLY)) < 0) {
        virReportSystemError(errno,
                             _("Could not open TPM device's cancel "
                               "path %s"), cancel_path);
        VIR_FORCE_CLOSE(*tpmfd);
        return -1;
    }

    return 0;
}


static char *
qemuBuildTPMBackendStr(const virDomainDef *def,
                       virCommandPtr cmd,
                       virQEMUCapsPtr qemuCaps,
                       int *tpmfd,
                       int *cancelfd,
                       char **chardev)
{
    const virDomainTPMDef *tpm = def->tpm;
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    const char *type = NULL;
    char *cancel_path = NULL;
    char *devset = NULL;
    char *cancelset = NULL;
    const char *tpmdev;

    *tpmfd = -1;
    *cancelfd = -1;

    switch (tpm->type) {
    case VIR_DOMAIN_TPM_TYPE_PASSTHROUGH:
    case VIR_DOMAIN_TPM_TYPE_EMULATOR:
        type = virDomainTPMBackendTypeToString(tpm->type);
        break;
    case VIR_DOMAIN_TPM_TYPE_LAST:
        goto error;
    }

    virBufferAsprintf(&buf, "%s,id=tpm-%s", type, tpm->info.alias);

    switch (tpm->type) {
    case VIR_DOMAIN_TPM_TYPE_PASSTHROUGH:
        if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE_TPM_PASSTHROUGH))
            goto no_support;

        tpmdev = tpm->data.passthrough.source.data.file.path;
        if (!(cancel_path = virTPMCreateCancelPath(tpmdev)))
            goto error;

        if (qemuBuildTPMOpenBackendFDs(tpmdev, cancel_path, tpmfd, cancelfd) < 0)
            goto error;

        virCommandPassFD(cmd, *tpmfd, VIR_COMMAND_PASS_FD_CLOSE_PARENT);
        virCommandPassFD(cmd, *cancelfd, VIR_COMMAND_PASS_FD_CLOSE_PARENT);

        if (!(devset = qemuVirCommandGetDevSet(cmd, *tpmfd)) ||
            !(cancelset = qemuVirCommandGetDevSet(cmd, *cancelfd)))
            goto error;

        virBufferAddLit(&buf, ",path=");
        virQEMUBuildBufferEscapeComma(&buf, devset);

        virBufferAddLit(&buf, ",cancel-path=");
        virQEMUBuildBufferEscapeComma(&buf, cancelset);

        VIR_FREE(devset);
        VIR_FREE(cancelset);
        VIR_FREE(cancel_path);

        break;
    case VIR_DOMAIN_TPM_TYPE_EMULATOR:
        if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE_TPM_EMULATOR))
            goto no_support;

        virBufferAddLit(&buf, ",chardev=chrtpm");

        if (virAsprintf(chardev, "socket,id=chrtpm,path=%s",
                        tpm->data.emulator.source.data.nix.path) < 0)
            goto error;

        break;
    case VIR_DOMAIN_TPM_TYPE_LAST:
        goto error;
    }

    if (virBufferCheckError(&buf) < 0)
        goto error;

    return virBufferContentAndReset(&buf);

 no_support:
    virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                   _("The QEMU executable %s does not support TPM "
                     "backend type %s"),
                   def->emulator, type);

 error:
    VIR_FREE(devset);
    VIR_FREE(cancel_path);

    virBufferFreeAndReset(&buf);
    return NULL;
}


static int
qemuBuildTPMCommandLine(virCommandPtr cmd,
                        const virDomainDef *def,
                        virQEMUCapsPtr qemuCaps)
{
    char *optstr;
    char *chardev = NULL;
    int tpmfd = -1;
    int cancelfd = -1;
    char *fdset;

    if (!def->tpm)
        return 0;

    if (!(optstr = qemuBuildTPMBackendStr(def, cmd, qemuCaps,
                                          &tpmfd, &cancelfd,
                                          &chardev)))
        return -1;

    virCommandAddArgList(cmd, "-tpmdev", optstr, NULL);
    VIR_FREE(optstr);

    if (chardev) {
        virCommandAddArgList(cmd, "-chardev", chardev, NULL);
        VIR_FREE(chardev);
    }

    if (tpmfd >= 0) {
        fdset = qemuVirCommandGetFDSet(cmd, tpmfd);
        if (!fdset)
            return -1;

        virCommandAddArgList(cmd, "-add-fd", fdset, NULL);
        VIR_FREE(fdset);
    }

    if (cancelfd >= 0) {
        fdset = qemuVirCommandGetFDSet(cmd, cancelfd);
        if (!fdset)
            return -1;

        virCommandAddArgList(cmd, "-add-fd", fdset, NULL);
        VIR_FREE(fdset);
    }

    if (!(optstr = qemuBuildTPMDevStr(def, qemuCaps)))
        return -1;

    virCommandAddArgList(cmd, "-device", optstr, NULL);
    VIR_FREE(optstr);

    return 0;
}

static int
qemuBuildSEVCommandLine(virDomainObjPtr vm, virCommandPtr cmd,
                        virDomainSEVDefPtr sev)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    qemuDomainObjPrivatePtr priv = vm->privateData;
    char *path = NULL;
    int ret = -1;

    if (!sev)
        return 0;

    VIR_DEBUG("policy=0x%x cbitpos=%d reduced_phys_bits=%d",
              sev->policy, sev->cbitpos, sev->reduced_phys_bits);

    virBufferAsprintf(&buf, "sev-guest,id=sev0,cbitpos=%d", sev->cbitpos);
    virBufferAsprintf(&buf, ",reduced-phys-bits=%d", sev->reduced_phys_bits);
    virBufferAsprintf(&buf, ",policy=0x%x", sev->policy);

    if (sev->dh_cert) {
        if (virAsprintf(&path, "%s/dh_cert.base64", priv->libDir) < 0)
            goto cleanup;
        virBufferAsprintf(&buf, ",dh-cert-file=%s", path);
        VIR_FREE(path);
    }

    if (sev->session) {
        if (virAsprintf(&path, "%s/session.base64", priv->libDir) < 0)
            goto cleanup;
        virBufferAsprintf(&buf, ",session-file=%s", path);
        VIR_FREE(path);
    }

    virCommandAddArg(cmd, "-object");
    virCommandAddArgBuffer(cmd, &buf);
    ret = 0;
 cleanup:
    virBufferFreeAndReset(&buf);
    return ret;
}

static int
qemuBuildVMCoreInfoCommandLine(virCommandPtr cmd,
                               const virDomainDef *def,
                               virQEMUCapsPtr qemuCaps)
{
    virTristateSwitch vmci = def->features[VIR_DOMAIN_FEATURE_VMCOREINFO];

    if (vmci != VIR_TRISTATE_SWITCH_ON)
        return 0;

    if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE_VMCOREINFO)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("vmcoreinfo is not available "
                         "with this QEMU binary"));
        return -1;
    }

    virCommandAddArgList(cmd, "-device", "vmcoreinfo", NULL);
    return 0;
}


static int
qemuBuildPanicCommandLine(virCommandPtr cmd,
                          const virDomainDef *def,
                          virQEMUCapsPtr qemuCaps)
{
    size_t i;

    for (i = 0; i < def->npanics; i++) {
        switch ((virDomainPanicModel) def->panics[i]->model) {
        case VIR_DOMAIN_PANIC_MODEL_S390:
            /* For s390 guests, the hardware provides the same
             * functionality as the pvpanic device. The address
             * cannot be configured by the user */
            if (!ARCH_IS_S390(def->os.arch)) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("only S390 guests support "
                                 "panic device of model 's390'"));
                return -1;
            }
            if (def->panics[i]->info.type !=
                VIR_DOMAIN_DEVICE_ADDRESS_TYPE_NONE) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("setting the panic device address is not "
                                 "supported for model 's390'"));
                return -1;
            }
            break;

        case VIR_DOMAIN_PANIC_MODEL_HYPERV:
            /* Panic with model 'hyperv' is not a device, it should
             * be configured in cpu commandline. The address
             * cannot be configured by the user */
            if (!ARCH_IS_X86(def->os.arch)) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("only i686 and x86_64 guests support "
                                 "panic device of model 'hyperv'"));
                return -1;
            }
            if (def->panics[i]->info.type !=
                VIR_DOMAIN_DEVICE_ADDRESS_TYPE_NONE) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("setting the panic device address is not "
                                 "supported for model 'hyperv'"));
                return -1;
            }
            break;

        case VIR_DOMAIN_PANIC_MODEL_PSERIES:
            /* For pSeries guests, the firmware provides the same
             * functionality as the pvpanic device. The address
             * cannot be configured by the user */
            if (!qemuDomainIsPSeries(def)) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("only pSeries guests support panic device "
                                 "of model 'pseries'"));
                return -1;
            }
            if (def->panics[i]->info.type !=
                VIR_DOMAIN_DEVICE_ADDRESS_TYPE_NONE) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("setting the panic device address is not "
                                 "supported for model 'pseries'"));
                return -1;
            }
            break;

        case VIR_DOMAIN_PANIC_MODEL_ISA:
            if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE_PANIC)) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("the QEMU binary does not support the "
                                 "ISA panic device"));
                return -1;
            }

            switch (def->panics[i]->info.type) {
            case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_ISA:
                virCommandAddArg(cmd, "-device");
                virCommandAddArgFormat(cmd, "pvpanic,ioport=%d",
                                       def->panics[i]->info.addr.isa.iobase);
                break;

            case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_NONE:
                virCommandAddArgList(cmd, "-device", "pvpanic", NULL);
                break;

            default:
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("panic is supported only "
                                 "with ISA address type"));
                return -1;
            }

        /* default model value was changed before in post parse */
        case VIR_DOMAIN_PANIC_MODEL_DEFAULT:
        case VIR_DOMAIN_PANIC_MODEL_LAST:
            break;
        }
    }

    return 0;
}


static virJSONValuePtr
qemuBuildPRManagerInfoPropsInternal(const char *alias,
                                    const char *path)
{
    virJSONValuePtr ret = NULL;

    if (qemuMonitorCreateObjectProps(&ret,
                                     "pr-manager-helper", alias,
                                     "s:path", path, NULL) < 0)
        return NULL;

    return ret;
}


/**
 * qemuBuildPRManagedManagerInfoProps:
 *
 * Build the JSON properties for the pr-manager object corresponding to the PR
 * daemon managed by libvirt.
 */
virJSONValuePtr
qemuBuildPRManagedManagerInfoProps(qemuDomainObjPrivatePtr priv)
{
    char *path = NULL;
    virJSONValuePtr ret = NULL;

    if (!(path = qemuDomainGetManagedPRSocketPath(priv)))
        return NULL;

    ret = qemuBuildPRManagerInfoPropsInternal(qemuDomainGetManagedPRAlias(),
                                              path);

    VIR_FREE(path);
    return ret;
}


/**
 * qemuBuildPRManagerInfoProps:
 * @src: storage source
 *
 * Build the JSON properties for the pr-manager object.
 */
virJSONValuePtr
qemuBuildPRManagerInfoProps(virStorageSourcePtr src)
{
    return qemuBuildPRManagerInfoPropsInternal(src->pr->mgralias, src->pr->path);
}


static int
qemuBuildManagedPRCommandLine(virCommandPtr cmd,
                              const virDomainDef *def,
                              qemuDomainObjPrivatePtr priv)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    virJSONValuePtr props = NULL;
    int ret = -1;

    if (!virDomainDefHasManagedPR(def))
        return 0;

    if (!(props = qemuBuildPRManagedManagerInfoProps(priv)))
        return -1;

    if (virQEMUBuildObjectCommandlineFromJSON(&buf, props) < 0)
        goto cleanup;

    virCommandAddArg(cmd, "-object");
    virCommandAddArgBuffer(cmd, &buf);

    ret = 0;
 cleanup:
    virBufferFreeAndReset(&buf);
    virJSONValueFree(props);
    return ret;
}


/**
 * qemuBuildCommandLineValidate:
 *
 * Prior to taking the plunge and building a long command line only
 * to find some configuration option isn't valid, let's do a couple
 * of checks and fail early.
 *
 * Returns 0 on success, returns -1 and messages what the issue is.
 */
static int
qemuBuildCommandLineValidate(virQEMUDriverPtr driver,
                             const virDomainDef *def)
{
    size_t i;
    int sdl = 0;
    int vnc = 0;
    int spice = 0;
    int egl_headless = 0;

    if (!virQEMUDriverIsPrivileged(driver)) {
        /* If we have no cgroups then we can have no tunings that
         * require them */

        if (virMemoryLimitIsSet(def->mem.hard_limit) ||
            virMemoryLimitIsSet(def->mem.soft_limit) ||
            virMemoryLimitIsSet(def->mem.swap_hard_limit)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("Memory tuning is not available in session mode"));
            return -1;
        }

        if (def->blkio.weight) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("Block I/O tuning is not available in session mode"));
            return -1;
        }

        if (def->cputune.sharesSpecified || def->cputune.period ||
            def->cputune.quota || def->cputune.global_period ||
            def->cputune.global_quota || def->cputune.emulator_period ||
            def->cputune.emulator_quota || def->cputune.iothread_period ||
            def->cputune.iothread_quota) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("CPU tuning is not available in session mode"));
            return -1;
        }
    }

    for (i = 0; i < def->ngraphics; ++i) {
        switch (def->graphics[i]->type) {
        case VIR_DOMAIN_GRAPHICS_TYPE_SDL:
            ++sdl;
            break;
        case VIR_DOMAIN_GRAPHICS_TYPE_VNC:
            ++vnc;
            break;
        case VIR_DOMAIN_GRAPHICS_TYPE_SPICE:
            ++spice;
            break;
        case VIR_DOMAIN_GRAPHICS_TYPE_EGL_HEADLESS:
            ++egl_headless;
            break;
        case VIR_DOMAIN_GRAPHICS_TYPE_RDP:
        case VIR_DOMAIN_GRAPHICS_TYPE_DESKTOP:
        case VIR_DOMAIN_GRAPHICS_TYPE_LAST:
            break;
        }
    }

    if (sdl > 1 || vnc > 1 || spice > 1 || egl_headless > 1) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("only 1 graphics device of each type "
                         "(sdl, vnc, spice, headless) is supported"));
        return -1;
    }

    if (def->virtType == VIR_DOMAIN_VIRT_XEN ||
        def->os.type == VIR_DOMAIN_OSTYPE_XEN ||
        def->os.type == VIR_DOMAIN_OSTYPE_LINUX) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("qemu emulator '%s' does not support xen"),
                       def->emulator);
        return -1;
    }

    return 0;
}


static int
qemuBuildSeccompSandboxCommandLine(virCommandPtr cmd,
                                   virQEMUDriverConfigPtr cfg,
                                   virQEMUCapsPtr qemuCaps ATTRIBUTE_UNUSED)
{
    if (cfg->seccompSandbox == 0) {
        if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_SECCOMP_SANDBOX))
            virCommandAddArgList(cmd, "-sandbox", "off", NULL);
        return 0;
    }

    /* Use blacklist by default if supported */
    if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_SECCOMP_BLACKLIST)) {
        virCommandAddArgList(cmd, "-sandbox",
                             "on,obsolete=deny,elevateprivileges=deny,"
                             "spawn=deny,resourcecontrol=deny",
                             NULL);
        return 0;
    }

    /* Seccomp whitelist is opt-in */
    if (cfg->seccompSandbox > 0)
        virCommandAddArgList(cmd, "-sandbox", "on", NULL);

    return 0;

}


char *
qemuBuildVsockDevStr(virDomainDefPtr def,
                     virDomainVsockDefPtr vsock,
                     virQEMUCapsPtr qemuCaps,
                     const char *fdprefix)
{
    qemuDomainVsockPrivatePtr priv = (qemuDomainVsockPrivatePtr)vsock->privateData;
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    char *ret = NULL;


    if (qemuBuildVirtioDevStr(&buf, "vhost-vsock", qemuCaps,
                              VIR_DOMAIN_DEVICE_VSOCK, vsock) < 0) {
        goto cleanup;
    }

    virBufferAsprintf(&buf, ",id=%s", vsock->info.alias);
    virBufferAsprintf(&buf, ",guest-cid=%u", vsock->guest_cid);
    virBufferAsprintf(&buf, ",vhostfd=%s%u", fdprefix, priv->vhostfd);
    if (qemuBuildDeviceAddressStr(&buf, def, &vsock->info, qemuCaps) < 0)
        goto cleanup;

    if (virBufferCheckError(&buf) < 0)
        goto cleanup;

    ret = virBufferContentAndReset(&buf);

 cleanup:
    virBufferFreeAndReset(&buf);
    return ret;
}


static int
qemuBuildVsockCommandLine(virCommandPtr cmd,
                          virDomainDefPtr def,
                          virDomainVsockDefPtr vsock,
                          virQEMUCapsPtr qemuCaps)
{
    qemuDomainVsockPrivatePtr priv = (qemuDomainVsockPrivatePtr)vsock->privateData;
    char *devstr = NULL;
    int ret = -1;

    if (!(devstr = qemuBuildVsockDevStr(def, vsock, qemuCaps, "")))
        goto cleanup;

    virCommandPassFD(cmd, priv->vhostfd, VIR_COMMAND_PASS_FD_CLOSE_PARENT);
    priv->vhostfd = -1;

    if (qemuCommandAddExtDevice(cmd, &vsock->info) < 0)
        goto cleanup;

    virCommandAddArgList(cmd, "-device", devstr, NULL);

    ret = 0;
 cleanup:
    VIR_FREE(devstr);
    return ret;
}


/*
 * Constructs a argv suitable for launching qemu with config defined
 * for a given virtual machine.
 */
virCommandPtr
qemuBuildCommandLine(virQEMUDriverPtr driver,
                     virLogManagerPtr logManager,
                     virSecurityManagerPtr secManager,
                     virDomainObjPtr vm,
                     const char *migrateURI,
                     virDomainMomentObjPtr snapshot,
                     virNetDevVPortProfileOp vmop,
                     bool standalone,
                     bool enableFips,
                     size_t *nnicindexes,
                     int **nicindexes)
{
    size_t i;
    char uuid[VIR_UUID_STRING_BUFLEN];
    virCommandPtr cmd = NULL;
    virQEMUDriverConfigPtr cfg = virQEMUDriverGetConfig(driver);
    unsigned int bootHostdevNet = 0;
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virDomainDefPtr def = vm->def;
    virQEMUCapsPtr qemuCaps = priv->qemuCaps;
    bool chardevStdioLogd = priv->chardevStdioLogd;

    VIR_DEBUG("driver=%p def=%p mon=%p "
              "qemuCaps=%p migrateURI=%s snapshot=%p vmop=%d",
              driver, def, priv->monConfig,
              qemuCaps, migrateURI, snapshot, vmop);

    if (qemuBuildCommandLineValidate(driver, def) < 0)
        goto error;

    cmd = virCommandNew(def->emulator);

    virCommandAddEnvPassCommon(cmd);

    /* For system QEMU we want to set both HOME and all the XDG variables to
     * libDir/qemu otherwise apps QEMU links to might try to access the default
     * home dir '/' which would always result in a permission issue.
     *
     * For session QEMU, we only want to set XDG_CACHE_HOME as cache data
     * may be purged at any time and that should not affect any app. We
     * do want VMs to integrate with services in user's session so we're
     * not re-setting any other env variables
     */
    if (!driver->privileged) {
        virCommandAddEnvFormat(cmd, "XDG_CACHE_HOME=%s/%s",
                               priv->libDir, ".cache");
    } else {
        virCommandAddEnvPair(cmd, "HOME", priv->libDir);
        virCommandAddEnvXDG(cmd, priv->libDir);
    }

    if (qemuBuildNameCommandLine(cmd, cfg, def, qemuCaps) < 0)
        goto error;

    if (!standalone)
        virCommandAddArg(cmd, "-S"); /* freeze CPU */

    if (qemuBuildMasterKeyCommandLine(cmd, priv) < 0)
        goto error;

    if (qemuBuildManagedPRCommandLine(cmd, def, priv) < 0)
        goto error;

    if (enableFips)
        virCommandAddArg(cmd, "-enable-fips");

    if (qemuBuildMachineCommandLine(cmd, cfg, def, qemuCaps) < 0)
        goto error;

    qemuBuildTSEGCommandLine(cmd, def);

    if (qemuBuildCpuCommandLine(cmd, driver, def, qemuCaps) < 0)
        goto error;

    qemuBuildDomainLoaderCommandLine(cmd, def);

    if (!migrateURI && !snapshot && qemuDomainAlignMemorySizes(def) < 0)
        goto error;

    if (qemuBuildMemCommandLine(cmd, cfg, def, qemuCaps, priv) < 0)
        goto error;

    if (qemuBuildSmpCommandLine(cmd, def) < 0)
        goto error;

    if (qemuBuildIOThreadCommandLine(cmd, def) < 0)
        goto error;

    if (virDomainNumaGetNodeCount(def->numa) &&
        qemuBuildNumaArgStr(cfg, def, cmd, priv) < 0)
        goto error;

    if (qemuBuildMemoryDeviceCommandLine(cmd, cfg, def, priv) < 0)
        goto error;

    virUUIDFormat(def->uuid, uuid);
    virCommandAddArgList(cmd, "-uuid", uuid, NULL);

    if (qemuBuildSmbiosCommandLine(cmd, driver, def) < 0)
        goto error;

    if (qemuBuildVMGenIDCommandLine(cmd, def, qemuCaps) < 0)
        goto error;

    /*
     * NB, -nographic *MUST* come before any serial, or monitor
     * or parallel port flags due to QEMU craziness, where it
     * decides to change the serial port & monitor to be on stdout
     * if you ask for nographic. So we have to make sure we override
     * these defaults ourselves...
     */
    if (!def->ngraphics) {
        virCommandAddArg(cmd, "-display");
        virCommandAddArg(cmd, "none");

        if (cfg->nogfxAllowHostAudio)
            virCommandAddEnvPassBlockSUID(cmd, "QEMU_AUDIO_DRV", NULL);
        else
            virCommandAddEnvString(cmd, "QEMU_AUDIO_DRV=none");
    }

    /* Disable global config files and default devices */
    virCommandAddArg(cmd, "-no-user-config");
    virCommandAddArg(cmd, "-nodefaults");

    if (qemuBuildSgaCommandLine(cmd, def, qemuCaps) < 0)
        goto error;

    if (qemuBuildMonitorCommandLine(logManager, secManager, cmd, cfg, def, priv) < 0)
        goto error;

    if (qemuBuildClockCommandLine(cmd, def, qemuCaps) < 0)
        goto error;

    if (qemuBuildPMCommandLine(cmd, def, priv) < 0)
        goto error;

    if (qemuBuildBootCommandLine(cmd, def, qemuCaps) < 0)
        goto error;

    if (qemuBuildIOMMUCommandLine(cmd, def, qemuCaps) < 0)
        goto error;

    if (qemuBuildGlobalControllerCommandLine(cmd, def, qemuCaps) < 0)
        goto error;

    if (qemuBuildControllersCommandLine(cmd, def, qemuCaps) < 0)
        goto error;

    if (qemuBuildHubCommandLine(cmd, def, qemuCaps) < 0)
        goto error;

    if (qemuBuildControllersByTypeCommandLine(cmd, def, qemuCaps,
                                              VIR_DOMAIN_CONTROLLER_TYPE_CCID) < 0)
        goto error;

    if (qemuBuildDisksCommandLine(cmd, def, qemuCaps) < 0)
        goto error;

    if (qemuBuildFSDevCommandLine(cmd, def, qemuCaps) < 0)
        goto error;

    if (qemuBuildNetCommandLine(driver, logManager, secManager, cmd, def,
                                qemuCaps, vmop, standalone,
                                nnicindexes, nicindexes, &bootHostdevNet) < 0)
        goto error;

    if (qemuBuildSmartcardCommandLine(logManager, secManager, cmd, cfg, def, qemuCaps,
                                      chardevStdioLogd) < 0)
        goto error;

    if (qemuBuildSerialCommandLine(logManager, secManager, cmd, cfg, def, qemuCaps,
                                   chardevStdioLogd) < 0)
        goto error;

    if (qemuBuildParallelsCommandLine(logManager, secManager, cmd, cfg, def, qemuCaps,
                                      chardevStdioLogd) < 0)
        goto error;

    if (qemuBuildChannelsCommandLine(logManager, secManager, cmd, cfg, def, qemuCaps,
                                     chardevStdioLogd) < 0)
        goto error;

    if (qemuBuildConsoleCommandLine(logManager, secManager, cmd, cfg, def, qemuCaps,
                                    chardevStdioLogd) < 0)
        goto error;

    if (qemuBuildTPMCommandLine(cmd, def, qemuCaps) < 0)
        goto error;

    if (qemuBuildInputCommandLine(cmd, def, qemuCaps) < 0)
        goto error;

    if (qemuBuildGraphicsCommandLine(cfg, cmd, def, qemuCaps) < 0)
        goto error;

    if (qemuBuildVideoCommandLine(cmd, def, qemuCaps) < 0)
        goto error;

    if (qemuBuildSoundCommandLine(cmd, def, qemuCaps) < 0)
        goto error;

    if (qemuBuildWatchdogCommandLine(cmd, def, qemuCaps) < 0)
        goto error;

    if (qemuBuildRedirdevCommandLine(logManager, secManager, cmd, cfg, def, qemuCaps,
                                     chardevStdioLogd) < 0)
        goto error;

    if (qemuBuildHostdevCommandLine(cmd, def, qemuCaps, &bootHostdevNet) < 0)
        goto error;

    if (migrateURI)
        virCommandAddArgList(cmd, "-incoming", migrateURI, NULL);

    if (qemuBuildMemballoonCommandLine(cmd, def, qemuCaps) < 0)
        goto error;

    if (qemuBuildRNGCommandLine(logManager, secManager, cmd, cfg, def, qemuCaps,
                                chardevStdioLogd) < 0)
        goto error;

    if (qemuBuildNVRAMCommandLine(cmd, def, qemuCaps) < 0)
        goto error;

    if (qemuBuildVMCoreInfoCommandLine(cmd, def, qemuCaps) < 0)
        goto error;

    if (qemuBuildSEVCommandLine(vm, cmd, def->sev) < 0)
        goto error;

    if (snapshot)
        virCommandAddArgList(cmd, "-loadvm", snapshot->def->name, NULL);

    if (def->namespaceData) {
        qemuDomainXmlNsDefPtr qemuxmlns;

        qemuxmlns = def->namespaceData;
        for (i = 0; i < qemuxmlns->num_args; i++)
            virCommandAddArg(cmd, qemuxmlns->args[i]);
        for (i = 0; i < qemuxmlns->num_env; i++)
            virCommandAddEnvPair(cmd, qemuxmlns->env_name[i],
                                 NULLSTR_EMPTY(qemuxmlns->env_value[i]));
    }

    if (qemuBuildSeccompSandboxCommandLine(cmd, cfg, qemuCaps) < 0)
        goto error;

    if (qemuBuildPanicCommandLine(cmd, def, qemuCaps) < 0)
        goto error;

    for (i = 0; i < def->nshmems; i++) {
        if (qemuBuildShmemCommandLine(logManager, secManager, cmd, cfg,
                                      def, def->shmems[i], qemuCaps,
                                      chardevStdioLogd))
            goto error;
    }

    if (def->vsock &&
        qemuBuildVsockCommandLine(cmd, def, def->vsock, qemuCaps) < 0)
        goto error;

    /* In some situations, eg. VFIO passthrough, QEMU might need to lock a
     * significant amount of memory, so we need to set the limit accordingly */
    virCommandSetMaxMemLock(cmd, qemuDomainGetMemLockLimitBytes(def));

    if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_MSG_TIMESTAMP) &&
        cfg->logTimestamp)
        virCommandAddArgList(cmd, "-msg", "timestamp=on", NULL);

    virObjectUnref(cfg);
    return cmd;

 error:
    virObjectUnref(cfg);
    virCommandFree(cmd);
    return NULL;
}


/* This function generates the correct '-device' string for character
 * devices of each architecture.
 */
static int
qemuBuildSerialChrDeviceStr(char **deviceStr,
                            const virDomainDef *def,
                            virDomainChrDefPtr serial,
                            virQEMUCapsPtr qemuCaps)
{
    virBuffer cmd = VIR_BUFFER_INITIALIZER;
    virQEMUCapsFlags caps;

    switch ((virDomainChrSerialTargetModel) serial->targetModel) {
    case VIR_DOMAIN_CHR_SERIAL_TARGET_MODEL_ISA_SERIAL:
    case VIR_DOMAIN_CHR_SERIAL_TARGET_MODEL_USB_SERIAL:
    case VIR_DOMAIN_CHR_SERIAL_TARGET_MODEL_PCI_SERIAL:
    case VIR_DOMAIN_CHR_SERIAL_TARGET_MODEL_SPAPR_VTY:
    case VIR_DOMAIN_CHR_SERIAL_TARGET_MODEL_SCLPCONSOLE:
    case VIR_DOMAIN_CHR_SERIAL_TARGET_MODEL_SCLPLMCONSOLE:

        caps = qemuChrSerialTargetModelToCaps(serial->targetModel);

        if (caps && !virQEMUCapsGet(qemuCaps, caps)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("'%s' is not supported in this QEMU binary"),
                           virDomainChrSerialTargetModelTypeToString(serial->targetModel));
            goto error;
        }
        break;

    case VIR_DOMAIN_CHR_SERIAL_TARGET_MODEL_PL011:
    case VIR_DOMAIN_CHR_SERIAL_TARGET_MODEL_16550A:
    case VIR_DOMAIN_CHR_SERIAL_TARGET_MODEL_NONE:
    case VIR_DOMAIN_CHR_SERIAL_TARGET_MODEL_LAST:
        /* Except from _LAST, which is just a guard value and will never
         * be used, all of the above are platform devices, which means
         * qemuBuildSerialCommandLine() will have taken the appropriate
         * branch and we will not have ended up here. */
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Invalid target model for serial device"));
        goto error;
    }

    virBufferAsprintf(&cmd, "%s,chardev=char%s,id=%s",
                      virDomainChrSerialTargetModelTypeToString(serial->targetModel),
                      serial->info.alias, serial->info.alias);

    if (qemuBuildDeviceAddressStr(&cmd, def, &serial->info, qemuCaps) < 0)
        goto error;

    if (virBufferCheckError(&cmd) < 0)
        goto error;

    *deviceStr = virBufferContentAndReset(&cmd);
    return 0;

 error:
    virBufferFreeAndReset(&cmd);
    return -1;
}

static int
qemuBuildParallelChrDeviceStr(char **deviceStr,
                              virDomainChrDefPtr chr)
{
    if (virAsprintf(deviceStr, "isa-parallel,chardev=char%s,id=%s",
                    chr->info.alias, chr->info.alias) < 0)
        return -1;
    return 0;
}

static int
qemuBuildChannelChrDeviceStr(char **deviceStr,
                             const virDomainDef *def,
                             virDomainChrDefPtr chr)
{
    int ret = -1;
    char *addr = NULL;
    int port;

    switch ((virDomainChrChannelTargetType)chr->targetType) {
    case VIR_DOMAIN_CHR_CHANNEL_TARGET_TYPE_GUESTFWD:

        addr = virSocketAddrFormat(chr->target.addr);
        if (!addr)
            return ret;
        port = virSocketAddrGetPort(chr->target.addr);

        if (virAsprintf(deviceStr,
                        "user,guestfwd=tcp:%s:%i-chardev:char%s,id=%s",
                        addr, port, chr->info.alias, chr->info.alias) < 0)
            goto cleanup;
        break;

    case VIR_DOMAIN_CHR_CHANNEL_TARGET_TYPE_VIRTIO:
        if (!(*deviceStr = qemuBuildVirtioSerialPortDevStr(def, chr)))
            goto cleanup;
        break;

    case VIR_DOMAIN_CHR_CHANNEL_TARGET_TYPE_XEN:
    case VIR_DOMAIN_CHR_CHANNEL_TARGET_TYPE_NONE:
    case VIR_DOMAIN_CHR_CHANNEL_TARGET_TYPE_LAST:
        return ret;
    }

    ret = 0;
 cleanup:
    VIR_FREE(addr);
    return ret;
}

static int
qemuBuildConsoleChrDeviceStr(char **deviceStr,
                             const virDomainDef *def,
                             virDomainChrDefPtr chr)
{
    int ret = -1;

    switch ((virDomainChrConsoleTargetType)chr->targetType) {
    case VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_SCLP:
    case VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_SCLPLM:
        if (!(*deviceStr = qemuBuildSclpDevStr(chr)))
            goto cleanup;
        break;

    case VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_VIRTIO:
        if (!(*deviceStr = qemuBuildVirtioSerialPortDevStr(def, chr)))
            goto cleanup;
        break;

    case VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_SERIAL:
        break;

    case VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_NONE:
    case VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_XEN:
    case VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_UML:
    case VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_LXC:
    case VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_OPENVZ:
    case VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_LAST:
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("unsupported console target type %s"),
                       NULLSTR(virDomainChrConsoleTargetTypeToString(chr->targetType)));
        goto cleanup;
    }

    ret = 0;
 cleanup:
    return ret;
}

int
qemuBuildChrDeviceStr(char **deviceStr,
                      const virDomainDef *vmdef,
                      virDomainChrDefPtr chr,
                      virQEMUCapsPtr qemuCaps)
{
    int ret = -1;

    switch ((virDomainChrDeviceType)chr->deviceType) {
    case VIR_DOMAIN_CHR_DEVICE_TYPE_SERIAL:
        ret = qemuBuildSerialChrDeviceStr(deviceStr, vmdef, chr, qemuCaps);
        break;

    case VIR_DOMAIN_CHR_DEVICE_TYPE_PARALLEL:
        ret = qemuBuildParallelChrDeviceStr(deviceStr, chr);
        break;

    case VIR_DOMAIN_CHR_DEVICE_TYPE_CHANNEL:
        ret = qemuBuildChannelChrDeviceStr(deviceStr, vmdef, chr);
        break;

    case VIR_DOMAIN_CHR_DEVICE_TYPE_CONSOLE:
        ret = qemuBuildConsoleChrDeviceStr(deviceStr, vmdef, chr);
        break;

    case VIR_DOMAIN_CHR_DEVICE_TYPE_LAST:
        return ret;
    }

    return ret;
}


virJSONValuePtr
qemuBuildHotpluggableCPUProps(const virDomainVcpuDef *vcpu)
{
    qemuDomainVcpuPrivatePtr vcpupriv = QEMU_DOMAIN_VCPU_PRIVATE(vcpu);
    virJSONValuePtr ret = NULL;

    if (virJSONValueObjectCreate(&ret, "s:driver", vcpupriv->type,
                                       "s:id", vcpupriv->alias, NULL) < 0)
        goto error;

    if (vcpupriv->socket_id != -1 &&
        virJSONValueObjectAdd(ret, "i:socket-id", vcpupriv->socket_id, NULL) < 0)
        goto error;

    if (vcpupriv->core_id != -1 &&
        virJSONValueObjectAdd(ret, "i:core-id", vcpupriv->core_id, NULL) < 0)
        goto error;

    if (vcpupriv->thread_id != -1 &&
        virJSONValueObjectAdd(ret, "i:thread-id", vcpupriv->thread_id, NULL) < 0)
        goto error;

    if (vcpupriv->node_id != -1 &&
        virJSONValueObjectAdd(ret, "i:node-id", vcpupriv->node_id, NULL) < 0)
        goto error;

    return ret;

 error:
    virJSONValueFree(ret);
    return NULL;
}


/**
 * qemuBuildStorageSourceAttachPrepareDrive:
 * @disk: disk object to prepare
 * @qemuCaps: qemu capabilities object
 * @driveBoot: bootable flag for disks which don't have -device part
 *
 * Prepare qemuBlockStorageSourceAttachDataPtr for use with the old approach
 * using -drive/drive_add. See qemuBlockStorageSourceAttachPrepareBlockdev.
 */
qemuBlockStorageSourceAttachDataPtr
qemuBuildStorageSourceAttachPrepareDrive(virDomainDiskDefPtr disk,
                                         virQEMUCapsPtr qemuCaps)
{
    VIR_AUTOPTR(qemuBlockStorageSourceAttachData) data = NULL;
    qemuBlockStorageSourceAttachDataPtr ret = NULL;

    if (VIR_ALLOC(data) < 0)
        return NULL;

    if (!(data->driveCmd = qemuBuildDriveStr(disk, qemuCaps)) ||
        !(data->driveAlias = qemuAliasDiskDriveFromDisk(disk)))
        return NULL;

    VIR_STEAL_PTR(ret, data);

    return ret;
}


/**
 * qemuBuildStorageSourceAttachPrepareCommon:
 * @src: storage source
 * @data: already initialized data for disk source addition
 * @qemuCaps: qemu capabilities object
 *
 * Prepare data for configuration associated with the disk source such as
 * secrets/TLS/pr objects etc ...
 */
int
qemuBuildStorageSourceAttachPrepareCommon(virStorageSourcePtr src,
                                          qemuBlockStorageSourceAttachDataPtr data,
                                          virQEMUCapsPtr qemuCaps)
{
    qemuDomainStorageSourcePrivatePtr srcpriv = QEMU_DOMAIN_STORAGE_SOURCE_PRIVATE(src);

    if (src->pr &&
        !virStoragePRDefIsManaged(src->pr) &&
        !(data->prmgrProps = qemuBuildPRManagerInfoProps(src)))
        return -1;

    if (srcpriv) {
        if (srcpriv->secinfo &&
            srcpriv->secinfo->type == VIR_DOMAIN_SECRET_INFO_TYPE_AES &&
            qemuBuildSecretInfoProps(srcpriv->secinfo, &data->authsecretProps) < 0)
            return -1;

        if (srcpriv->encinfo &&
            qemuBuildSecretInfoProps(srcpriv->encinfo, &data->encryptsecretProps) < 0)
            return -1;
    }

    if (src->haveTLS == VIR_TRISTATE_BOOL_YES &&
        qemuBuildTLSx509BackendProps(src->tlsCertdir, false, true, src->tlsAlias,
                                     NULL, qemuCaps, &data->tlsProps) < 0)
        return -1;

    return 0;
}


/**
 * qemuBuildStorageSourceChainAttachPrepareDrive:
 * @disk: disk definition
 * @qemuCaps: qemu capabilities object
 *
 * Prepares qemuBlockStorageSourceChainDataPtr for attaching @disk via -drive.
 */
qemuBlockStorageSourceChainDataPtr
qemuBuildStorageSourceChainAttachPrepareDrive(virDomainDiskDefPtr disk,
                                              virQEMUCapsPtr qemuCaps)
{
    VIR_AUTOPTR(qemuBlockStorageSourceAttachData) elem = NULL;
    VIR_AUTOPTR(qemuBlockStorageSourceChainData) data = NULL;

    if (VIR_ALLOC(data) < 0)
        return NULL;

    if (!(elem = qemuBuildStorageSourceAttachPrepareDrive(disk, qemuCaps)))
        return NULL;

    if (qemuBuildStorageSourceAttachPrepareCommon(disk->src, elem, qemuCaps) < 0)
        return NULL;

    if (VIR_APPEND_ELEMENT(data->srcdata, data->nsrcdata, elem) < 0)
        return NULL;

    VIR_RETURN_PTR(data);
}


/**
 * qemuBuildStorageSourceChainAttachPrepareDrive:
 * @top: storage source chain
 * @qemuCaps: qemu capabilities object
 *
 * Prepares qemuBlockStorageSourceChainDataPtr for attaching @top via -blockdev.
 */
qemuBlockStorageSourceChainDataPtr
qemuBuildStorageSourceChainAttachPrepareBlockdev(virStorageSourcePtr top,
                                                 virQEMUCapsPtr qemuCaps)
{
    VIR_AUTOPTR(qemuBlockStorageSourceAttachData) elem = NULL;
    VIR_AUTOPTR(qemuBlockStorageSourceChainData) data = NULL;
    virStorageSourcePtr n;

    if (VIR_ALLOC(data) < 0)
        return NULL;

    for (n = top; virStorageSourceIsBacking(n); n = n->backingStore) {
        if (!(elem = qemuBlockStorageSourceAttachPrepareBlockdev(n)))
            return NULL;

        if (qemuBuildStorageSourceAttachPrepareCommon(n, elem, qemuCaps) < 0)
            return NULL;

        if (VIR_APPEND_ELEMENT(data->srcdata, data->nsrcdata, elem) < 0)
            return NULL;
    }

    VIR_RETURN_PTR(data);
}
