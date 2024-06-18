/*
 * Copyright © 2024 Qualcomm Innovation Center, Inc. All Rights Reserved.
 * SPDX-License-Identifier: MIT
 */

use crate::api::icd::CLResult;
use crate::dev::debug::VclDebugFlags;
use crate::dev::renderer::*;
use crate::log;
use crate::protocol::*;

use vcl_opencl_gen::CL_VIRTGPU_IOCTL_FAILED_MESA;
use vcl_sys_gen::*;
use vcl_virglrenderer_gen::*;
use vcl_vtest_gen::*;

use std::ffi::*;
use std::fs::File;
use std::io;
use std::io::*;
use std::mem;
use std::os::fd::AsRawFd;
use std::os::fd::FromRawFd;
use std::os::unix::net::UnixStream;
use std::ptr;
use std::sync::Mutex;

pub struct Vtest {
    sock: Mutex<UnixStream>,
    protocol_version: u32,
    max_timeline_count: u32,
    capset: VirglRendererCapset,
}

impl Vtest {
    pub fn new() -> io::Result<Self> {
        let path = CStr::from_bytes_with_nul(VTEST_DEFAULT_SOCKET_NAME)
            .expect("Failed to create vtest socket name");
        let sock = UnixStream::connect(path.to_str().expect("Failed to convert CStr to str"))
            .expect("Failed to connect to vtest socket");
        let mut ret = Self {
            sock: Mutex::new(sock),
            protocol_version: 0,
            max_timeline_count: 0,
            capset: VirglRendererCapset::default(),
        };
        ret.vcmd_create_renderer("vcl")?;
        ret.init_protocol_version()?;
        ret.init_params()?;
        ret.init_capset()?;
        ret.vcmd_context_init(ret.capset.id)?;

        Ok(ret)
    }

    fn read<T: AsBytes + ?Sized>(&self, val: &mut T) -> io::Result<usize> {
        self.sock.lock().unwrap().read(val.as_mut_bytes())
    }

    fn write<T: AsBytes + ?Sized>(&self, val: &T) -> io::Result<usize> {
        self.sock.lock().unwrap().write(val.as_bytes())
    }

    fn init_protocol_version(&mut self) -> io::Result<()> {
        let min_protocol_version = 3;

        let ver = if self.vcmd_ping_protocol_version()? {
            self.vcmd_protocol_version()?
        } else {
            0
        };

        if ver < min_protocol_version {
            let msg = format!("vtest protocol version ({}) too old", ver);
            return Err(io::Error::new(io::ErrorKind::Unsupported, msg));
        }

        self.protocol_version = ver;
        Ok(())
    }

    fn init_params(&mut self) -> io::Result<()> {
        let val = self.vcmd_get_param(vcmd_param::VCMD_PARAM_MAX_TIMELINE_COUNT)?;
        self.max_timeline_count = val;
        Ok(())
    }

    fn init_capset(&mut self) -> io::Result<()> {
        let mut capset = VirglRendererCapsetVcl::default();
        let capset_available =
            self.vcmd_get_capset(self.capset.id, self.capset.version, &mut capset)?;
        if !capset_available {
            return Err(io::Error::new(io::ErrorKind::Unsupported, "no vcl capset"));
        }
        self.capset.data = capset;
        Ok(())
    }

    fn vcmd_create_renderer(&self, name: &str) -> io::Result<()> {
        let name_c = CString::new(name).expect("Failed to create vtest renderer name");
        let name_bytes = name_c.as_bytes_with_nul();
        let mut vtest_hdr = [0u32; VTEST_HDR_SIZE as usize];
        vtest_hdr[VTEST_CMD_LEN as usize] = name_bytes.len() as u32;
        vtest_hdr[VTEST_CMD_ID as usize] = VCMD_CREATE_RENDERER;

        self.write(&vtest_hdr)?;
        self.write(name_bytes)?;
        Ok(())
    }

    fn vcmd_ping_protocol_version(&self) -> io::Result<bool> {
        let mut vtest_hdr = [0u32; VTEST_HDR_SIZE as usize];
        vtest_hdr[VTEST_CMD_LEN as usize] = VCMD_PING_PROTOCOL_VERSION_SIZE;
        vtest_hdr[VTEST_CMD_ID as usize] = VCMD_PING_PROTOCOL_VERSION;

        self.write(&vtest_hdr)?;

        // Send a dummy busy wait to avoid blocking in self.read in case ping
        // protocol version is not supported
        let mut vcmd_busy_wait = [0u32; VCMD_BUSY_WAIT_SIZE as usize];
        vtest_hdr[VTEST_CMD_LEN as usize] = VCMD_BUSY_WAIT_SIZE;
        vtest_hdr[VTEST_CMD_ID as usize] = VCMD_RESOURCE_BUSY_WAIT;
        vcmd_busy_wait[VCMD_BUSY_WAIT_HANDLE as usize] = 0;
        vcmd_busy_wait[VCMD_BUSY_WAIT_FLAGS as usize] = 0;

        self.write(&vtest_hdr)?;
        self.write(&vcmd_busy_wait)?;

        let mut dummy = 0u32;
        self.read(&mut vtest_hdr)?;
        if vtest_hdr[VTEST_CMD_ID as usize] == VCMD_PING_PROTOCOL_VERSION {
            // Consume the dummy busy wait result
            self.read(&mut vtest_hdr)?;
            assert_eq!(vtest_hdr[VTEST_CMD_ID as usize], VCMD_RESOURCE_BUSY_WAIT);
            self.read(&mut dummy)?;
            return Ok(true);
        } else {
            // No ping protocol version support
            assert_eq!(vtest_hdr[VTEST_CMD_ID as usize], VCMD_RESOURCE_BUSY_WAIT);
            self.read(&mut dummy)?;
            return Ok(false);
        }
    }

    fn vcmd_protocol_version(&self) -> io::Result<u32> {
        let mut vtest_hdr = [0u32; VTEST_HDR_SIZE as usize];
        let mut vcmd_protocol_version = [0u32; VCMD_PROTOCOL_VERSION_SIZE as usize];
        vtest_hdr[VTEST_CMD_LEN as usize] = VCMD_PROTOCOL_VERSION_SIZE;
        vtest_hdr[VTEST_CMD_ID as usize] = VCMD_PROTOCOL_VERSION;
        vcmd_protocol_version[VCMD_PROTOCOL_VERSION_VERSION as usize] = VTEST_PROTOCOL_VERSION;

        self.write(&vtest_hdr)?;
        self.write(&vcmd_protocol_version)?;

        self.read(&mut vtest_hdr)?;
        assert_eq!(
            vtest_hdr[VTEST_CMD_LEN as usize],
            VCMD_PROTOCOL_VERSION_SIZE
        );
        assert_eq!(vtest_hdr[VTEST_CMD_ID as usize], VCMD_PROTOCOL_VERSION);
        self.read(&mut vcmd_protocol_version)?;

        Ok(vcmd_protocol_version[VCMD_PROTOCOL_VERSION_VERSION as usize])
    }

    fn vcmd_get_param(&self, param: vcmd_param) -> io::Result<u32> {
        let mut vtest_hdr = [0u32; VTEST_HDR_SIZE as usize];
        let mut vcmd_get_param = [0u32; VCMD_GET_PARAM_SIZE as usize];
        vtest_hdr[VTEST_CMD_LEN as usize] = VCMD_GET_PARAM_SIZE;
        vtest_hdr[VTEST_CMD_ID as usize] = VCMD_GET_PARAM;
        vcmd_get_param[VCMD_GET_PARAM_PARAM as usize] = param as _;

        self.write(&vtest_hdr)?;
        self.write(&vcmd_get_param)?;

        self.read(&mut vtest_hdr)?;
        assert_eq!(vtest_hdr[VTEST_CMD_LEN as usize], 2);
        assert_eq!(vtest_hdr[VTEST_CMD_ID as usize], VCMD_GET_PARAM);

        let mut resp = [0u32; 2];
        self.read(&mut resp)?;

        Ok(if resp[0] != 0 { resp[1] } else { 0 })
    }

    fn vcmd_get_capset<C: AsBytes>(
        &self,
        id: virgl_renderer_capset,
        version: u32,
        capset: &mut C,
    ) -> io::Result<bool> {
        let mut vtest_hdr = [0u32; VTEST_HDR_SIZE as usize];
        let mut vcmd_get_capset = [0u32; VCMD_GET_CAPSET_SIZE as usize];
        vtest_hdr[VTEST_CMD_LEN as usize] = VCMD_GET_CAPSET_SIZE;
        vtest_hdr[VTEST_CMD_ID as usize] = VCMD_GET_CAPSET;
        vcmd_get_capset[VCMD_GET_CAPSET_ID as usize] = id as _;
        vcmd_get_capset[VCMD_GET_CAPSET_VERSION as usize] = version;

        self.write(&vtest_hdr)?;
        self.write(&vcmd_get_capset)?;

        self.read(&mut vtest_hdr)?;
        assert_eq!(vtest_hdr[VTEST_CMD_ID as usize], VCMD_GET_CAPSET);

        let mut valid = 0u32;
        self.read(&mut valid)?;
        if valid == 0 {
            return Ok(false);
        }

        let capset_size = mem::size_of_val(capset);
        let mut read_size = (vtest_hdr[VTEST_CMD_LEN as usize] as usize - 1) * 4;
        if capset_size > read_size {
            let capset_bytes: &mut [u8] = capset.as_mut_bytes();
            self.read(&mut capset_bytes[..read_size])?;
            capset_bytes[read_size..capset_size - read_size].fill(0);
        } else {
            self.read(capset)?;

            let mut temp = [0u8; 256];
            read_size -= capset_size;
            while read_size > 0 {
                let temp_size = read_size.min(temp.len());
                self.read(&mut temp)?;
                read_size -= temp_size;
            }
        }

        Ok(true)
    }

    fn vcmd_context_init(&self, capset_id: virgl_renderer_capset) -> io::Result<()> {
        let mut vtest_hdr = [0u32; VTEST_HDR_SIZE as usize];
        let mut vcmd_context_init = [0u32; VCMD_CONTEXT_INIT_SIZE as usize];
        vtest_hdr[VTEST_CMD_LEN as usize] = VCMD_CONTEXT_INIT_SIZE;
        vtest_hdr[VTEST_CMD_ID as usize] = VCMD_CONTEXT_INIT;
        vcmd_context_init[VCMD_CONTEXT_INIT_CAPSET_ID as usize] = capset_id as _;

        self.write(&vtest_hdr)?;
        self.write(&vcmd_context_init)?;
        Ok(())
    }

    fn receive_fd(&self) -> i32 {
        let mut iovec = iovec {
            iov_base: ptr::null_mut(),
            iov_len: 0,
        };

        let mut control = [0u8; CMSG_SPACE_SIZEOF_INT];

        let mut message = msghdr::default();

        message.msg_name = ptr::null_mut();
        message.msg_namelen = 0;
        message.msg_iov = &mut iovec;
        message.msg_iovlen = 1;
        message.msg_control = control.as_mut_ptr() as _;
        message.msg_controllen = control.len() as _;
        message.msg_flags = 0;

        let sock = self.sock.lock().unwrap();
        let size = unsafe { recvmsg(sock.as_raw_fd(), &mut message, 0) };
        if size < 0 {
            log!(
                VclDebugFlags::Vtest,
                "Failed to receive message {}",
                io::Error::last_os_error()
            );
            return -1;
        }

        let cmsgh = unsafe { cmsg_firsthdr(&mut message) };
        if cmsgh.is_null() {
            log!(
                VclDebugFlags::Vtest,
                "Failed to get message header {}",
                io::Error::last_os_error()
            );
            return -1;
        }

        let cmsg_level = unsafe { (*cmsgh).cmsg_level };
        if cmsg_level != SOL_SOCKET as i32 {
            log!(VclDebugFlags::Vtest, "Invalid cmsg_level {}", cmsg_level);
            return -1;
        }

        let cmsg_type = unsafe { (*cmsgh).cmsg_type };
        if cmsg_type != SCM_RIGHTS as i32 {
            log!(VclDebugFlags::Vtest, "Invalid cmsg_type {}", unsafe {
                (*cmsgh).cmsg_type
            });
            return -1;
        }

        let data_ptr = unsafe { cmsg_data(cmsgh) };
        if data_ptr.is_null() {
            log!(VclDebugFlags::Vtest, "Failed to get cmsg data",);
            return -1;
        }
        let data_i32_ptr: *mut i32 = data_ptr as _;
        unsafe { *data_i32_ptr }
    }

    fn vcmd_resource_create(&self, size: usize) -> io::Result<(i32, i32)> {
        let mut vtest_hdr = [0u32; VTEST_HDR_SIZE as usize];
        let mut vcmd_res_create = [0u32; VCMD_RES_CREATE2_SIZE as usize];
        vtest_hdr[VTEST_CMD_LEN as usize] = VCMD_RES_CREATE2_SIZE;
        vtest_hdr[VTEST_CMD_ID as usize] = VCMD_RESOURCE_CREATE2;

        vcmd_res_create[VCMD_RES_CREATE2_RES_HANDLE as usize] = 0;
        vcmd_res_create[VCMD_RES_CREATE2_TARGET as usize] = 0; // PIPE_BUFFER
        vcmd_res_create[VCMD_RES_CREATE2_FORMAT as usize] = 0;
        vcmd_res_create[VCMD_RES_CREATE2_BIND as usize] = VIRGL_BIND_CUSTOM;
        vcmd_res_create[VCMD_RES_CREATE2_WIDTH as usize] = size as u32;
        vcmd_res_create[VCMD_RES_CREATE2_HEIGHT as usize] = 1;
        vcmd_res_create[VCMD_RES_CREATE2_DEPTH as usize] = 1;
        vcmd_res_create[VCMD_RES_CREATE2_ARRAY_SIZE as usize] = 1;
        vcmd_res_create[VCMD_RES_CREATE2_LAST_LEVEL as usize] = 0;
        vcmd_res_create[VCMD_RES_CREATE2_NR_SAMPLES as usize] = 0;
        vcmd_res_create[VCMD_RES_CREATE2_DATA_SIZE as usize] = size as u32;

        self.write(&vtest_hdr)?;
        self.write(&vcmd_res_create)?;

        self.read(&mut vtest_hdr)?;
        assert_eq!(vtest_hdr[VTEST_CMD_ID as usize], VCMD_RESOURCE_CREATE2);
        assert_eq!(vtest_hdr[VTEST_CMD_LEN as usize], 1);
        let mut handle = 0i32;
        self.read(&mut handle)?;

        Ok((handle, self.receive_fd()))
    }

    fn vcmd_transfer(&self, resource: &dyn VclResource, vcmd: u32) -> io::Result<()> {
        let size = resource.len();

        let mut vtest_hdr = [0u32; VTEST_HDR_SIZE as usize];
        vtest_hdr[VTEST_CMD_LEN as usize] = VCMD_TRANSFER2_HDR_SIZE;
        if vcmd == VCMD_TRANSFER_PUT2 {
            // host expects size in dwords, so calculate rounded up value
            vtest_hdr[VTEST_CMD_LEN as usize] += (size as u32 + 3) / 4;
        }
        vtest_hdr[VTEST_CMD_ID as usize] = vcmd;

        let mut vtest_get = [0u32; VCMD_TRANSFER2_HDR_SIZE as usize];
        vtest_get[VCMD_TRANSFER2_RES_HANDLE as usize] = resource.get_handle() as u32;
        vtest_get[VCMD_TRANSFER2_LEVEL as usize] = 0;
        vtest_get[VCMD_TRANSFER2_X as usize] = 0;
        vtest_get[VCMD_TRANSFER2_Y as usize] = 0;
        vtest_get[VCMD_TRANSFER2_Z as usize] = 0;
        vtest_get[VCMD_TRANSFER2_WIDTH as usize] = size as u32;
        vtest_get[VCMD_TRANSFER2_HEIGHT as usize] = 1;
        vtest_get[VCMD_TRANSFER2_DEPTH as usize] = 1;
        vtest_get[VCMD_TRANSFER2_DATA_SIZE as usize] = size as u32;
        vtest_get[VCMD_TRANSFER2_OFFSET as usize] = 0;

        self.write(&vtest_hdr)?;
        self.write(&vtest_get)?;
        Ok(())
    }

    fn vcmd_transfer_get(&self, resource: &dyn VclResource) -> io::Result<()> {
        self.vcmd_transfer(resource, VCMD_TRANSFER_GET2)
    }

    fn vcmd_transfer_put(&self, resource: &dyn VclResource) -> io::Result<()> {
        self.vcmd_transfer(resource, VCMD_TRANSFER_PUT2)
    }

    fn vcmd_busy_wait(&self, resource: &dyn VclResource) -> io::Result<u32> {
        let mut vtest_hdr = [0u32; VTEST_HDR_SIZE as usize];
        vtest_hdr[VTEST_CMD_LEN as usize] = VCMD_BUSY_WAIT_SIZE;
        vtest_hdr[VTEST_CMD_ID as usize] = VCMD_RESOURCE_BUSY_WAIT;

        let mut vtest_wait = [0u32; VCMD_BUSY_WAIT_SIZE as usize];
        vtest_wait[VCMD_BUSY_WAIT_HANDLE as usize] = resource.get_handle() as u32;
        vtest_wait[VCMD_BUSY_WAIT_FLAGS as usize] = VCMD_BUSY_WAIT_FLAG_WAIT;

        self.write(&vtest_hdr)?;
        self.write(&vtest_wait)?;

        self.read(&mut vtest_hdr)?;
        let mut result = 0u32;
        self.read(&mut result)?;
        Ok(result)
    }

    fn vcmd_submit(&self, encoder: VclCsEncoder) -> io::Result<()> {
        let header_size: usize = mem::size_of::<u32>() + mem::size_of::<vcmd_submit_cmd2_batch>();
        assert_eq!(header_size % mem::size_of::<u32>(), 0);
        let batch = encoder.get_slice();
        assert_eq!(batch.len() % mem::size_of::<u32>(), 0);

        let total_size = header_size + batch.len();
        let mut vtest_hdr = [0u32; VTEST_HDR_SIZE as usize];
        vtest_hdr[VTEST_CMD_LEN as usize] = (total_size / mem::size_of::<u32>()) as u32;
        vtest_hdr[VTEST_CMD_ID as usize] = VCMD_SUBMIT_CMD2;
        self.write(&vtest_hdr)?;

        // write batch count and batch headers
        let batch_count = 1;
        self.write(&batch_count)?;
        let dst = vcmd_submit_cmd2_batch {
            cmd_offset: (header_size / mem::size_of::<u32>()) as u32,
            cmd_size: (batch.len() / mem::size_of::<u32>()) as u32,
            flags: 0,
            ring_idx: 0,
            sync_count: 0,
            sync_offset: 0,
        };

        self.write(&dst)?;

        // write cs
        if batch.len() > 0 {
            self.write(batch)?;
        }

        // Why do I need to reed this to make it work?
        let mut val = 0u32;
        self.read(&mut val)?;

        Ok(())
    }
}

impl VclRenderer for Vtest {
    fn submit(&self, encoder: VclCsEncoder) -> CLResult<()> {
        self.vcmd_submit(encoder).unwrap();
        Ok(())
    }

    fn create_buffer(&self, size: usize) -> CLResult<VclBuffer> {
        VclBuffer::new_for_vtest(self, size)
    }

    fn transfer_get(&self, resource: &mut dyn VclResource) -> CLResult<()> {
        self.vcmd_transfer_get(resource)
            .expect("Failed to transfer resource");
        self.vcmd_busy_wait(resource).expect("Failed to wait");
        // Resource has been mapped at creation
        Ok(())
    }

    fn transfer_put(&self, resource: &mut dyn VclResource) -> CLResult<()> {
        self.vcmd_transfer_put(resource)
            .expect("Failed to transfer resource");
        self.vcmd_busy_wait(resource).expect("Failed to wait");
        Ok(())
    }
}

pub struct VtestResource {
    ptr: *const c_void,
    size: usize,
    handle: i32,
}

impl VtestResource {
    pub fn new(vtest: &Vtest, size: usize) -> CLResult<Self> {
        let (handle, fd) = vtest
            .vcmd_resource_create(size)
            .expect("Failed to create vtest resource");
        if fd < 0 {
            return Err(CL_VIRTGPU_IOCTL_FAILED_MESA);
        }
        let _file = unsafe { File::from_raw_fd(fd) };

        let ptr = unsafe {
            mmap(
                ptr::null_mut(),
                size,
                (PROT_WRITE | PROT_READ) as i32,
                MAP_SHARED as i32,
                fd,
                0,
            )
        };
        if ptr == unsafe { mem::transmute(MapResult::FAILED) } {
            log!(VclDebugFlags::Vtest, "Failed to map vtest resource");
            return Err(CL_VIRTGPU_IOCTL_FAILED_MESA);
        }

        Ok(Self { ptr, size, handle })
    }
}

impl VclResource for VtestResource {
    fn get_handle(&self) -> i32 {
        self.handle
    }

    fn get_bo_handle(&self) -> u32 {
        0
    }

    fn get_ptr(&self) -> *const c_void {
        self.ptr
    }

    fn len(&self) -> usize {
        self.size
    }

    fn transfer_get(&self) -> CLResult<()> {
        Ok(())
    }

    fn transfer_put(&self) -> CLResult<()> {
        Ok(())
    }

    fn wait(&self) -> CLResult<()> {
        Ok(())
    }

    fn map(&mut self, offset: usize, size: usize) -> CLResult<&[u8]> {
        Ok(unsafe { std::slice::from_raw_parts(self.ptr.add(offset) as _, size) })
    }

    fn map_mut(&mut self, offset: usize, size: usize) -> CLResult<&mut [u8]> {
        Ok(unsafe { std::slice::from_raw_parts_mut(self.ptr.add(offset) as _, size) })
    }
}
