// SPDX-License-Identifier: MIT
// SPDX-CopyrightText: Copyright Collabora 2024

use std::fmt::Write;
use std::io::Cursor;

use compiler::memstream::MemStream;
use panthor_uapi_bindings::drm_panthor_csif_info;
use panthor_uapi_bindings::drm_panthor_dump_gpuva;
use panthor_uapi_bindings::drm_panthor_dump_header;
use panthor_uapi_bindings::drm_panthor_dump_preamble;
use panthor_uapi_bindings::drm_panthor_fw_info;
use panthor_uapi_bindings::drm_panthor_gpu_info;
use panthor_uapi_bindings::DRM_PANTHOR_DUMP_HEADER_TYPE_CSIF_INFO;
use panthor_uapi_bindings::DRM_PANTHOR_DUMP_HEADER_TYPE_FW_INFO;
use panthor_uapi_bindings::DRM_PANTHOR_DUMP_HEADER_TYPE_GPU_INFO;
use panthor_uapi_bindings::DRM_PANTHOR_DUMP_HEADER_TYPE_GROUP_INFO;
use panthor_uapi_bindings::DRM_PANTHOR_DUMP_HEADER_TYPE_PREAMBLE;
use panthor_uapi_bindings::DRM_PANTHOR_DUMP_HEADER_TYPE_QUEUE_INFO;
use panthor_uapi_bindings::DRM_PANTHOR_DUMP_HEADER_TYPE_VM;

use crate::decode_drm_panthor_csif_info;
use crate::decode_drm_panthor_dump_gpuva;
use crate::decode_drm_panthor_dump_group_info;
use crate::decode_drm_panthor_dump_header;
use crate::decode_drm_panthor_dump_preamble;
use crate::decode_drm_panthor_dump_queue_info;
use crate::decode_drm_panthor_fw_info;
use crate::decode_drm_panthor_gpu_info;
use crate::CoredumpError;
use crate::Result;

pub(crate) struct GpuVa {
    inner: drm_panthor_dump_gpuva,
    data: Vec<u8>,
}

impl GpuVa {
    pub(crate) fn new(inner: drm_panthor_dump_gpuva, data: Vec<u8>) -> Self {
        Self { inner, data }
    }
}

fn header_type_to_string(header_type: u32) -> String {
    match header_type {
        panthor_uapi_bindings::DRM_PANTHOR_DUMP_HEADER_TYPE_PREAMBLE => "Preamble".to_string(),
        panthor_uapi_bindings::DRM_PANTHOR_DUMP_HEADER_TYPE_GPU_INFO => "GpuInfo".to_string(),
        panthor_uapi_bindings::DRM_PANTHOR_DUMP_HEADER_TYPE_CSIF_INFO => "CsifInfo".to_string(),
        panthor_uapi_bindings::DRM_PANTHOR_DUMP_HEADER_TYPE_FW_INFO => "FwInfo".to_string(),
        panthor_uapi_bindings::DRM_PANTHOR_DUMP_HEADER_TYPE_VM => "Vm".to_string(),
        panthor_uapi_bindings::DRM_PANTHOR_DUMP_HEADER_TYPE_GROUP_INFO => "GroupInfo".to_string(),
        panthor_uapi_bindings::DRM_PANTHOR_DUMP_HEADER_TYPE_QUEUE_INFO => "QueueInfo".to_string(),
        _ => "Unknown".to_string(),
    }
}

// We're still deciding what we need to keep track of.
#[allow(dead_code)]
pub(crate) enum GpuInfoState {
    Unparsed,
    Parsed {
        preamble: drm_panthor_dump_preamble,
        gpu_info: drm_panthor_gpu_info,
        csif_info: drm_panthor_csif_info,
        fw_info: drm_panthor_fw_info,
    },
}

pub(crate) struct DecodeCtx<T> {
    pandecode_ctx: *mut libpanfrost_bindings::pandecode_context,
    raw_data: Cursor<T>,
    stream: MemStream,
    gpu_info: GpuInfoState,
    gpu_va_list: Vec<GpuVa>,
    output: String,
}

impl<T: AsRef<[u8]>> DecodeCtx<T> {
    pub(crate) fn new(raw_data: Cursor<T>) -> Result<Self> {
        let stream = MemStream::new()?;
        let pandecode_ctx = unsafe {
            let pandecode_ctx = libpanfrost_bindings::pandecode_create_context(false);
            assert!(!pandecode_ctx.is_null());

            // This is the exact same type
            (&mut *pandecode_ctx).dump_stream =
                stream.c_file() as *mut std::ffi::c_void as *mut libpanfrost_bindings::_IO_FILE;

            pandecode_ctx
        };

        Ok(Self {
            pandecode_ctx,
            stream,
            gpu_info: GpuInfoState::Unparsed,
            gpu_va_list: Vec::new(),
            raw_data,
            output: Default::default(),
        })
    }

    pub(crate) fn output(&self) -> String {
        self.output.clone()
    }

    pub(crate) fn position(&self) -> u64 {
        self.raw_data.position()
    }

    fn decode_header_with_type(&mut self, header_type: u32) -> Result<drm_panthor_dump_header> {
        let position = self.raw_data.position();
        let header = decode_drm_panthor_dump_header(&mut self.raw_data)?;

        if header.header_type != header_type {
            self.raw_data.set_position(position);
            return Err(CoredumpError::UnexpectedHeaderType {
                expected: header_type,
                found: header.header_type,
                position,
            });
        }

        Ok(header)
    }

    fn add_va(&mut self, mut va: GpuVa) {
        unsafe {
            libpanfrost_bindings::pandecode_inject_mmap(
                self.pandecode_ctx,
                va.inner.addr,
                va.data.as_mut_ptr() as *mut _,
                va.data.len().try_into().unwrap(),
                std::ptr::null_mut(),
            );
        }

        self.gpu_va_list.push(va);
    }

    fn decode_metadata(&mut self) -> Result<()> {
        let header = self.decode_header_with_type(DRM_PANTHOR_DUMP_HEADER_TYPE_PREAMBLE)?;
        let preamble =
            decode_drm_panthor_dump_preamble(&mut self.raw_data, header.data_size as usize)?;

        writeln!(
            &mut self.output,
            "/* Dump version: {}.{} */",
            preamble.version_major, preamble.version_minor
        )?;
        writeln!(&mut self.output)?;

        let header = self.decode_header_with_type(DRM_PANTHOR_DUMP_HEADER_TYPE_GPU_INFO)?;
        let gpu_info = decode_drm_panthor_gpu_info(&mut self.raw_data, header.data_size as usize)?;

        writeln!(&mut self.output, "GPU info: {:#?}", &gpu_info)?;
        writeln!(&mut self.output)?;

        let header = self.decode_header_with_type(DRM_PANTHOR_DUMP_HEADER_TYPE_CSIF_INFO)?;
        let csif_info =
            decode_drm_panthor_csif_info(&mut self.raw_data, header.data_size as usize)?;

        writeln!(&mut self.output, "CSIF info: {:#?}", &csif_info)?;
        writeln!(&mut self.output)?;

        let header = self.decode_header_with_type(DRM_PANTHOR_DUMP_HEADER_TYPE_FW_INFO)?;
        let fw_info = decode_drm_panthor_fw_info(&mut self.raw_data, header.data_size as usize)?;

        writeln!(&mut self.output, "FW info: {:#?}", &fw_info)?;
        writeln!(&mut self.output)?;

        self.gpu_info = GpuInfoState::Parsed {
            preamble,
            gpu_info,
            csif_info,
            fw_info,
        };

        Ok(())
    }

    /// Decode the BOs. Map the GPU memory so that we can use it when decoding jobs.
    fn decode_mem(&mut self) -> Result<()> {
        loop {
            match self.decode_header_with_type(DRM_PANTHOR_DUMP_HEADER_TYPE_VM) {
                Ok(header) => {
                    let (gpuva, inline_data) = decode_drm_panthor_dump_gpuva(
                        &mut self.raw_data,
                        header.data_size as usize,
                    )?;

                    self.add_va(GpuVa::new(gpuva, inline_data));
                }
                Err(CoredumpError::UnexpectedHeaderType { .. }) => break,
                Err(e) => return Err(e),
            }
        }

        writeln!(&mut self.output, "/* Section: Mappings */")?;

        for va in &self.gpu_va_list {
            writeln!(
                &mut self.output,
                "GPU VA address: {:#x}, range: {:#x}, i.e.: ({:#x} - {:#x})",
                va.inner.addr,
                va.inner.range,
                va.inner.addr,
                va.inner.addr + va.inner.range,
            )?;
        }

        writeln!(&mut self.output)?;
        unsafe { libpanfrost_bindings::pandecode_dump_mappings(self.pandecode_ctx) };

        self.output.push_str(&self.stream.take_utf8_string_lossy()?);
        writeln!(&mut self.output,)?;

        Ok(())
    }

    fn decode_queue(&mut self) -> Result<()> {
        loop {
            match self.decode_header_with_type(DRM_PANTHOR_DUMP_HEADER_TYPE_QUEUE_INFO) {
                Ok(header) => {
                    writeln!(&mut self.output, "/* Section: Queue entry */")?;

                    let queue_info = decode_drm_panthor_dump_queue_info(
                        &mut self.raw_data,
                        header.data_size as usize,
                    )?;

                    writeln!(
                        &mut self.output,
                        "Queue Info:\n\
                        Flags: {}\n\
                        Command Stream ID: {}\n\
                        Ring Buffer GPU VA: {:#x}\n\
                        Ring Buffer Insert Point: {:#x}\n\
                        Ring Buffer Extract Point: {:#x}\n\
                        Ring Buffer Size: {:#x}",
                        queue_info.flags,
                        queue_info.cs_id,
                        queue_info.ringbuf_gpuva,
                        queue_info.ringbuf_insert,
                        queue_info.ringbuf_extract,
                        queue_info.ringbuf_size,
                    )?;

                    unsafe {
                        let gpu_id = match self.gpu_info {
                            GpuInfoState::Parsed { gpu_info, .. } => gpu_info.gpu_id,
                            _ => return Err(CoredumpError::InvalidDump("GPU info not parsed yet")),
                        };

                        // assert!(
                        //     self.gpu_va_list
                        //         .iter()
                        //         .find(|v| v.inner.addr == queue_info.ringbuf_gpuva)
                        //         .is_some(),
                        //     "Ringbuf must be mapped in libpanfrost_decode before decoding"
                        // );

                        // XXX: This is a placeholder for now...
                        let mut regs = [0u32; 256];
                        libpanfrost_bindings::pandecode_cs(
                            self.pandecode_ctx,
                            queue_info.ringbuf_gpuva,
                            queue_info.ringbuf_size.try_into().unwrap(),
                            gpu_id >> 16,
                            regs.as_mut_ptr(),
                        );

                        self.output.push_str(&self.stream.take_utf8_string_lossy()?);
                        writeln!(&mut self.output)?;
                    }
                }

                Err(CoredumpError::UnexpectedHeaderType { .. }) => break,
                Err(e) => return Err(e),
            }
        }

        Ok(())
    }

    fn decode_groups(&mut self) -> Result<()> {
        writeln!(&mut self.output, "/* Section: Groups */")?;

        loop {
            match self.decode_header_with_type(DRM_PANTHOR_DUMP_HEADER_TYPE_GROUP_INFO) {
                Ok(header) => {
                    writeln!(&mut self.output, "/* Section: Group entry */")?;

                    let group_info = decode_drm_panthor_dump_group_info(
                        &mut self.raw_data,
                        header.data_size as usize,
                    )?;
                    writeln!(&mut self.output, "Group info: {:#?}", &group_info)?;

                    for i in 0..group_info.queue_count {
                        // let is_faulty = (group_info.faulty_bitmask & (1 << i)) != 0;
                        // if is_faulty {
                        self.decode_queue()?;
                        // }
                    }
                }

                Err(CoredumpError::UnexpectedHeaderType { .. }) => break,
                Err(e) => return Err(e),
            }
        }

        writeln!(&mut self.output)?;
        Ok(())
    }

    fn decode_job(&mut self) -> Result<()> {
        writeln!(
            &mut self.output,
            "/* Section: Job at offset {:#x} */",
            self.raw_data.position()
        )?;

        self.decode_mem()?;
        self.decode_groups()?;
        writeln!(&mut self.output)?;

        while let Some(va) = self.gpu_va_list.pop() {
            unsafe {
                libpanfrost_bindings::pandecode_inject_free(
                    self.pandecode_ctx,
                    va.inner.addr,
                    va.inner.range.try_into().unwrap(),
                );
            }
        }

        Ok(())
    }

    pub(crate) fn decode(&mut self) -> Result<&String> {
        self.summary()?;
        self.decode_metadata()?;

        self.decode_job()?;

        Ok(&self.output)
    }

    pub(crate) fn summary(&mut self) -> Result<()> {
        writeln!(&mut self.output, "/* Section: Summary */")?;
        loop {
            match decode_drm_panthor_dump_header(&mut self.raw_data) {
                Ok(header) => {
                    writeln!(
                        &mut self.output,
                        "Header: magic: {}, header_type: {}, header_size: {}, data_size: {}",
                        header.magic,
                        header_type_to_string(header.header_type),
                        header.header_size,
                        header.data_size
                    )?;

                    self.raw_data
                        .set_position(self.raw_data.position() + header.data_size as u64);
                }
                Err(CoredumpError::InvalidHeaderMagic { .. }) => break,
                Err(e) => return Err(e),
            }
        }

        self.raw_data.set_position(0);
        writeln!(&mut self.output)?;
        Ok(())
    }
}

impl<T> Drop for DecodeCtx<T> {
    fn drop(&mut self) {
        unsafe {
            // pandecode_destroy_context will attempt to close the stream, but
            // the Memstream has ownership of the underlying FD.
            (*self.pandecode_ctx).dump_stream = std::ptr::null_mut();
            libpanfrost_bindings::pandecode_destroy_context(self.pandecode_ctx);
        }
    }
}
