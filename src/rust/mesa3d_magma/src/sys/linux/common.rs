// Copyright 2024 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use std::fs;
use std::fs::File;
use std::fs::OpenOptions;
use std::io::Read;
use std::path::Path;
use std::path::PathBuf;
use std::sync::Arc;

use mesa3d_util::AsRawDescriptor;
use mesa3d_util::MappedRegion;
use mesa3d_util::MesaError;
use mesa3d_util::MesaHandle;
use mesa3d_util::MesaResult;
use mesa3d_util::OwnedDescriptor;
use mesa3d_util::RawDescriptor;

use nix::dir::Dir;
use nix::fcntl::readlink;
use nix::fcntl::OFlag;
use nix::sys::stat::major;
use nix::sys::stat::minor;
use nix::sys::stat::stat;
use nix::sys::stat::Mode;

use crate::magma::MagmaPhysicalDevice;
use crate::magma_defines::MagmaCreateBufferInfo;
use crate::magma_defines::MagmaHeapBudget;
use crate::magma_defines::MagmaMemoryProperties;
use crate::magma_defines::MagmaPciBusInfo;
use crate::magma_defines::MagmaPciInfo;
use crate::magma_defines::MAGMA_VENDOR_ID_AMD;
use crate::magma_defines::MAGMA_VENDOR_ID_INTEL;
use crate::magma_defines::MAGMA_VENDOR_ID_MALI;
use crate::magma_defines::MAGMA_VENDOR_ID_QCOM;
use crate::sys::linux::get_drm_device_name;
use crate::sys::linux::AmdGpu;
use crate::sys::linux::Msm;
use crate::sys::linux::DRM_DIR_NAME;
use crate::sys::linux::DRM_RENDER_MINOR_NAME;
use crate::traits::AsVirtGpu;
use crate::traits::Buffer;
use crate::traits::Context;
use crate::traits::Device;
use crate::traits::GenericPhysicalDevice;
use crate::traits::PhysicalDevice;

const PCI_ATTRS: [&str; 5] = [
    "revision",
    "vendor",
    "device",
    "subsystem_vendor",
    "subsystem_device",
];

#[derive(Debug)]
pub struct LinuxPhysicalDevice {
    descriptor: OwnedDescriptor,
    vendor_id: u16,
    name: String,
}

pub trait PlatformPhysicalDevice {
    fn as_raw_descriptor(&self) -> RawDescriptor {
        -1
    }
}

impl GenericPhysicalDevice for LinuxPhysicalDevice {
    fn create_device(
        &self,
        physical_device: &Arc<dyn PhysicalDevice>,
    ) -> MesaResult<Arc<dyn Device>> {
        let device: Arc<dyn Device> = match self.vendor_id {
            MAGMA_VENDOR_ID_AMD => Arc::new(AmdGpu::new(physical_device.clone())?),
            MAGMA_VENDOR_ID_QCOM => Arc::new(Msm::new(physical_device.clone())),
            _ => todo!(),
        };

        Ok(device)
    }
}

pub trait PlatformDevice {}

impl LinuxPhysicalDevice {
    pub fn new(device_node: PathBuf, vendor_id: u16) -> MesaResult<LinuxPhysicalDevice> {
        let descriptor: OwnedDescriptor = OpenOptions::new()
            .read(true)
            .write(true)
            .open(device_node.clone())?
            .into();

        // TODO: confirm if necessary if everything has PCI-ID
        let name = get_drm_device_name(&descriptor)?;

        Ok(LinuxPhysicalDevice {
            descriptor,
            vendor_id,
            name,
        })
    }
}

impl PlatformPhysicalDevice for LinuxPhysicalDevice {
    fn as_raw_descriptor(&self) -> RawDescriptor {
        self.descriptor.as_raw_descriptor()
    }
}

impl AsVirtGpu for LinuxPhysicalDevice {}
impl PhysicalDevice for LinuxPhysicalDevice {}

pub fn enumerate_devices() -> MesaResult<Vec<MagmaPhysicalDevice>> {
    let mut devices: Vec<MagmaPhysicalDevice> = Vec::new();
    let mut dir = Dir::open(
        DRM_DIR_NAME,
        OFlag::O_RDONLY | OFlag::O_DIRECTORY | OFlag::O_CLOEXEC,
        Mode::empty(),
    )?;

    for item in dir.iter() {
        if let Ok(entry) = item {
            let filename = entry.file_name().to_str()?;
            if filename.contains(DRM_RENDER_MINOR_NAME) {
                let path = Path::new(DRM_DIR_NAME).join(filename);
                let statbuf = stat(&path)?;

                let maj = major(statbuf.st_rdev);
                let min = minor(statbuf.st_rdev);

                let pci_device_dir = format!("/sys/dev/char/{}:{}/device", maj, min);
                let pci_subsystem_dir = format!("{}/subsystem", pci_device_dir);
                let subsystem_path = Path::new(&pci_subsystem_dir);
                let subsystem = readlink(subsystem_path)?;

                // Should not panic: Linux directories are always valid UTF-8
                if !subsystem.into_string().unwrap().contains("/pci") {
                    continue;
                }

                let mut pci_info: MagmaPciInfo = Default::default();
                let mut pci_bus_info: MagmaPciBusInfo = Default::default();
                let mut buffer = [0; 2];
                for attr in PCI_ATTRS {
                    let attr_path = format!("{}/{}", pci_device_dir, attr);
                    let mut file = File::open(attr_path)?;
                    file.read_exact(&mut buffer[..])?;
                    match attr {
                        "revision" => pci_info.revision_id = buffer[0],
                        "vendor" => pci_info.vendor_id = u16::from_be_bytes(buffer),
                        "device" => pci_info.device_id = u16::from_be_bytes(buffer),
                        "subsystem_vendor" => pci_info.subvendor_id = u16::from_be_bytes(buffer),
                        "subsystem_device" => pci_info.subdevice_id = u16::from_be_bytes(buffer),
                        _ => unimplemented!(),
                    }
                }

                let uevent_path = format!("{}/uevent", pci_device_dir);
                let text: String = fs::read_to_string(uevent_path)?;
                for line in text.lines() {
                    if line.contains("PCI_SLOT_NAME") {
                        let v: Vec<&str> = line.split(&['=', ':', '.'][..]).collect();

                        // TODO: remove unwraps(..)
                        pci_bus_info.domain = v[1].parse::<u16>().unwrap();
                        pci_bus_info.bus = v[2].parse::<u8>().unwrap();
                        pci_bus_info.device = v[3].parse::<u8>().unwrap();
                        pci_bus_info.function = v[4].parse::<u8>().unwrap();
                    }
                }

                devices.push(MagmaPhysicalDevice::new(
                    Arc::new(LinuxPhysicalDevice::new(
                        path.to_path_buf(),
                        pci_info.vendor_id,
                    )?),
                    pci_info,
                    pci_bus_info,
                ));
            }
        }
    }

    Ok(devices)
}
