import json
import shlex
import unittest
import xml.etree.ElementTree as ET
from typing import TYPE_CHECKING

try:
    from ..test_helper.test_helper import (  # type: ignore
        LibvirtTestsBase,
        assert_domain_domstate,
        hotplug,
        initialComputeVMSetup,
        initialControllerVMSetup,
        number_of_storage_devices,
        wait_for_ssh,
    )
except Exception:
    if not TYPE_CHECKING:
        from test_helper import (
            LibvirtTestsBase,
            assert_domain_domstate,
            hotplug,
            initialComputeVMSetup,
            initialControllerVMSetup,
            number_of_storage_devices,
            wait_for_ssh,
        )

# pyright: reportPossiblyUnboundVariable=false

if "start_all" not in globals():
    from ..test_helper.test_helper.nixos_test_stubs import (  # type: ignore
        computeVM,
        controllerVM,
        start_all,
    )


class LibvirtTests(LibvirtTestsBase):  # type: ignore
    def __init__(self, methodName):
        super().__init__(methodName, controllerVM, computeVM)

    @classmethod
    def setUpClass(cls):
        start_all()
        initialControllerVMSetup(controllerVM)
        initialComputeVMSetup(computeVM)

    def test_live_migration_rejects_changed_disk_alias(self):
        """Replacement XML must not rename a disk that CH restores by its old ID."""
        controllerVM.succeed("virsh define /etc/domain-chv.xml")
        controllerVM.succeed("virsh start testvm")
        wait_for_ssh(controllerVM)
        controllerVM.succeed("qemu-img create -f raw /nfs-root/disk.img 100M")
        controllerVM.succeed("chmod 0666 /nfs-root/disk.img")
        hotplug(
            controllerVM,
            "virsh attach-disk testvm /var/lib/libvirt/storage-pools/nfs-share/disk.img vdb",
        )

        xml = ET.fromstring(controllerVM.succeed("virsh dumpxml testvm"))
        disk = xml.find("./devices/disk/target[@dev='vdb']/..")
        assert disk is not None
        alias = disk.find("alias")
        assert alias is not None
        self.assertEqual(alias.get("name"), "virtio-disk1")
        alias.set("name", "ua-migration-volume")
        controllerVM.succeed(
            f"printf %s {shlex.quote(ET.tostring(xml, encoding='unicode'))} > /tmp/alias-migration.xml"
        )

        output = controllerVM.fail(
            "virsh migrate testvm ch+tcp://computeVM/session --live --p2p "
            "--xml /tmp/alias-migration.xml 2>&1"
        )
        self.assertIn(
            "Cannot change disk 'vdb' alias during migration from 'virtio-disk1' to 'ua-migration-volume'",
            output,
        )
        assert_domain_domstate(controllerVM, "running")
        computeVM.fail("virsh list --all | grep -q testvm")
        wait_for_ssh(controllerVM)

        # Rejection must release the migration job and leave the source usable.
        controllerVM.succeed(
            "virsh migrate testvm ch+tcp://computeVM/session --persistent --live --p2p"
        )
        wait_for_ssh(computeVM)
        hotplug(computeVM, "virsh detach-disk testvm vdb")
        self.assertEqual(number_of_storage_devices(computeVM), 1)

    def test_live_migration_preserves_omitted_disk_alias(self):
        """Restore omitted user aliases by target, then detach after daemon restart."""
        controllerVM.succeed("virsh define /etc/domain-chv.xml")
        controllerVM.succeed("virsh start testvm")
        wait_for_ssh(controllerVM)
        controllerVM.succeed("qemu-img create -f raw /nfs-root/disk.img 100M")
        controllerVM.succeed("chmod 0666 /nfs-root/disk.img")
        hotplug(
            controllerVM,
            "virsh attach-disk testvm /var/lib/libvirt/storage-pools/nfs-share/disk.img vdb "
            "--alias ua-migration-volume",
        )

        xml = ET.fromstring(controllerVM.succeed("virsh dumpxml testvm"))
        devices = xml.find("devices")
        assert devices is not None
        disks = devices.findall("disk")
        for disk in disks:
            alias = disk.find("alias")
            assert alias is not None
            disk.remove(alias)
            devices.remove(disk)
        # The cookie must identify disks by target, not XML ordering.
        devices.extend(reversed(disks))
        controllerVM.succeed(
            f"printf %s {shlex.quote(ET.tostring(xml, encoding='unicode'))} > /tmp/alias-migration.xml"
        )
        controllerVM.succeed(
            "virsh migrate testvm ch+tcp://computeVM/session --persistent --live --p2p "
            "--xml /tmp/alias-migration.xml"
        )
        wait_for_ssh(computeVM)
        computeVM.succeed("systemctl restart virtchd")
        xml = ET.fromstring(computeVM.succeed("virsh dumpxml testvm"))
        info = json.loads(
            computeVM.succeed(
                "ch-remote --api-socket /run/libvirt/ch/testvm-socket info"
            )
        )
        ch_disks = {disk["path"]: disk["id"] for disk in info["config"]["disks"]}
        for disk in xml.findall("./devices/disk"):
            source = disk.find("source")
            alias = disk.find("alias")
            assert source is not None and alias is not None
            self.assertEqual(alias.get("name"), ch_disks[source.get("file")])
        self.assertIn("ua-migration-volume", ch_disks.values())
        hotplug(computeVM, "virsh detach-disk testvm vdb")
        self.assertEqual(number_of_storage_devices(computeVM), 1)
        computeVM.succeed(
            "ch-remote --api-socket /run/libvirt/ch/testvm-socket info "
            "| jq -e 'all(.config.disks[]; .id != \"ua-migration-volume\")'"
        )


def suite():
    testcases = [
        LibvirtTests.test_live_migration_preserves_omitted_disk_alias,
        LibvirtTests.test_live_migration_rejects_changed_disk_alias,
    ]
    suite = unittest.TestSuite()
    for testcaseMethod in testcases:
        suite.addTest(LibvirtTests(testcaseMethod.__name__))
    return suite


runner = unittest.TextTestRunner()
if not runner.run(suite()).wasSuccessful():
    raise Exception("Test Run unsuccessful")
