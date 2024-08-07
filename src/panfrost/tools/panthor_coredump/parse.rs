// SPDX-License-Identifier: MIT
// SPDX-CopyrightText: Copyright Collabora 2024

use std::io::Cursor;
use std::io::Read;

use panthor_uapi_bindings::*;

use crate::CoredumpError;
use crate::Result;
use crate::MAGIC;

pub(crate) fn decode_drm_panthor_dump_header(
    cursor: &mut Cursor<impl AsRef<[u8]>>,
) -> Result<drm_panthor_dump_header> {
    let pos = cursor.position();

    let mut magic_bytes = [0u8; 4];
    cursor.read_exact(&mut magic_bytes)?;
    let magic = u32::from_le_bytes(magic_bytes);

    if magic != MAGIC {
        cursor.set_position(pos);
        return Err(CoredumpError::InvalidHeaderMagic {
            magic,
            position: pos,
        });
    }

    let mut header_type_bytes = [0u8; 4];
    cursor.read_exact(&mut header_type_bytes)?;
    let header_type = u32::from_le_bytes(header_type_bytes);

    let mut header_size_bytes = [0u8; 4];
    cursor.read_exact(&mut header_size_bytes)?;
    let header_size = u32::from_le_bytes(header_size_bytes);

    let mut data_size_bytes = [0u8; 4];
    cursor.read_exact(&mut data_size_bytes)?;
    let data_size = u32::from_le_bytes(data_size_bytes);

    let header = drm_panthor_dump_header {
        magic,
        header_type,
        header_size,
        data_size,
    };

    // Skip the remaining header bytes if header_size is larger than the struct size
    if header.header_size > std::mem::size_of::<drm_panthor_dump_header>() as u32 {
        cursor.set_position(
            cursor.position()
                + (header.header_size - std::mem::size_of::<drm_panthor_dump_header>() as u32)
                    as u64,
        );
    }

    Ok(header)
}

pub(crate) fn decode_drm_panthor_dump_preamble(
    data: &mut Cursor<impl AsRef<[u8]>>,
    data_size: usize,
) -> Result<drm_panthor_dump_preamble> {
    let pos = data.position() as usize;
    let cursor = &data.get_ref().as_ref()[pos..pos + data_size];
    let mut cursor = Cursor::new(cursor);

    let mut major_bytes = [0u8; 4];
    cursor.read_exact(&mut major_bytes)?;
    let version_major = u32::from_le_bytes(major_bytes);

    let mut minor_bytes = [0u8; 4];
    cursor.read_exact(&mut minor_bytes)?;
    let version_minor = u32::from_le_bytes(minor_bytes);

    let mut gpu_info_offset_bytes = [0u8; 4];
    cursor.read_exact(&mut gpu_info_offset_bytes)?;
    let gpu_info_offset = u32::from_le_bytes(gpu_info_offset_bytes);

    let mut csif_info_offset_bytes = [0u8; 4];
    cursor.read_exact(&mut csif_info_offset_bytes)?;
    let csif_info_offset = u32::from_le_bytes(csif_info_offset_bytes);

    let mut fw_info_offset_bytes = [0u8; 4];
    cursor.read_exact(&mut fw_info_offset_bytes)?;
    let fw_info_offset = u32::from_le_bytes(fw_info_offset_bytes);

    let mut vm_offset_bytes = [0u8; 4];
    cursor.read_exact(&mut vm_offset_bytes)?;
    let vm_offset = u32::from_le_bytes(vm_offset_bytes);

    let preamble = drm_panthor_dump_preamble {
        version_major,
        version_minor,
        gpu_info_offset,
        csif_info_offset,
        fw_info_offset,
        vm_offset,
    };

    data.set_position(pos as u64 + cursor.position());
    Ok(preamble)
}

pub(crate) fn decode_drm_panthor_gpu_info(
    data: &mut Cursor<impl AsRef<[u8]>>,
    data_size: usize,
) -> Result<drm_panthor_gpu_info> {
    let pos = data.position() as usize;
    let cursor = &data.get_ref().as_ref()[pos..pos + data_size];
    let mut cursor = Cursor::new(cursor);

    let mut gpu_id_bytes = [0u8; 4];
    cursor.read_exact(&mut gpu_id_bytes)?;
    let gpu_id = u32::from_le_bytes(gpu_id_bytes);

    let mut gpu_rev_bytes = [0u8; 4];
    cursor.read_exact(&mut gpu_rev_bytes)?;
    let gpu_rev = u32::from_le_bytes(gpu_rev_bytes);

    let mut csf_id_bytes = [0u8; 4];
    cursor.read_exact(&mut csf_id_bytes)?;
    let csf_id = u32::from_le_bytes(csf_id_bytes);

    let mut l2_features_bytes = [0u8; 4];
    cursor.read_exact(&mut l2_features_bytes)?;
    let l2_features = u32::from_le_bytes(l2_features_bytes);

    let mut tiler_features_bytes = [0u8; 4];
    cursor.read_exact(&mut tiler_features_bytes)?;
    let tiler_features = u32::from_le_bytes(tiler_features_bytes);

    let mut mem_features_bytes = [0u8; 4];
    cursor.read_exact(&mut mem_features_bytes)?;
    let mem_features = u32::from_le_bytes(mem_features_bytes);

    let mut mmu_features_bytes = [0u8; 4];
    cursor.read_exact(&mut mmu_features_bytes)?;
    let mmu_features = u32::from_le_bytes(mmu_features_bytes);

    let mut thread_features_bytes = [0u8; 4];
    cursor.read_exact(&mut thread_features_bytes)?;
    let thread_features = u32::from_le_bytes(thread_features_bytes);

    let mut max_threads_bytes = [0u8; 4];
    cursor.read_exact(&mut max_threads_bytes)?;
    let max_threads = u32::from_le_bytes(max_threads_bytes);

    let mut thread_max_workgroup_size_bytes = [0u8; 4];
    cursor.read_exact(&mut thread_max_workgroup_size_bytes)?;
    let thread_max_workgroup_size = u32::from_le_bytes(thread_max_workgroup_size_bytes);

    let mut thread_max_barrier_size_bytes = [0u8; 4];
    cursor.read_exact(&mut thread_max_barrier_size_bytes)?;
    let thread_max_barrier_size = u32::from_le_bytes(thread_max_barrier_size_bytes);

    let mut coherency_features_bytes = [0u8; 4];
    cursor.read_exact(&mut coherency_features_bytes)?;
    let coherency_features = u32::from_le_bytes(coherency_features_bytes);

    let mut texture_features = [0u32; 4];
    for i in 0..4 {
        let mut texture_feature_bytes = [0u8; 4];
        cursor.read_exact(&mut texture_feature_bytes)?;
        texture_features[i] = u32::from_le_bytes(texture_feature_bytes);
    }

    let mut as_present_bytes = [0u8; 4];
    cursor.read_exact(&mut as_present_bytes)?;
    let as_present = u32::from_le_bytes(as_present_bytes);

    let mut padding_bytes = [0u8; 4];
    cursor.read_exact(&mut padding_bytes)?;
    let padding = u32::from_le_bytes(padding_bytes);
    assert!(padding == 0);

    let mut shader_present_bytes = [0u8; 8];
    cursor.read_exact(&mut shader_present_bytes)?;
    let shader_present = u64::from_le_bytes(shader_present_bytes);

    let mut l2_present_bytes = [0u8; 8];
    cursor.read_exact(&mut l2_present_bytes)?;
    let l2_present = u64::from_le_bytes(l2_present_bytes);

    let mut tiler_present_bytes = [0u8; 8];
    cursor.read_exact(&mut tiler_present_bytes)?;
    let tiler_present = u64::from_le_bytes(tiler_present_bytes);

    let mut core_features_bytes = [0u8; 4];
    cursor.read_exact(&mut core_features_bytes)?;
    let core_features = u32::from_le_bytes(core_features_bytes);

    let mut pad_bytes = [0u8; 4];
    cursor.read_exact(&mut pad_bytes)?;
    let pad = u32::from_le_bytes(pad_bytes);

    assert!(cursor.position() as usize == data_size);
    data.set_position(pos as u64 + cursor.position());

    Ok(drm_panthor_gpu_info {
        gpu_id,
        gpu_rev,
        csf_id,
        l2_features,
        tiler_features,
        mem_features,
        mmu_features,
        thread_features,
        max_threads,
        thread_max_workgroup_size,
        thread_max_barrier_size,
        coherency_features,
        texture_features,
        as_present,
        shader_present,
        l2_present,
        tiler_present,
        core_features,
        pad,
    })
}

pub(crate) fn decode_drm_panthor_csif_info(
    data: &mut Cursor<impl AsRef<[u8]>>,
    data_size: usize,
) -> Result<drm_panthor_csif_info> {
    let pos = data.position() as usize;
    let cursor = &data.get_ref().as_ref()[pos..pos + data_size];
    let mut cursor = Cursor::new(cursor);

    let mut csg_slot_count_bytes = [0u8; 4];
    cursor.read_exact(&mut csg_slot_count_bytes)?;
    let csg_slot_count = u32::from_le_bytes(csg_slot_count_bytes);

    let mut cs_slot_count_bytes = [0u8; 4];
    cursor.read_exact(&mut cs_slot_count_bytes)?;
    let cs_slot_count = u32::from_le_bytes(cs_slot_count_bytes);

    let mut cs_reg_count_bytes = [0u8; 4];
    cursor.read_exact(&mut cs_reg_count_bytes)?;
    let cs_reg_count = u32::from_le_bytes(cs_reg_count_bytes);

    let mut scoreboard_slot_count_bytes = [0u8; 4];
    cursor.read_exact(&mut scoreboard_slot_count_bytes)?;
    let scoreboard_slot_count = u32::from_le_bytes(scoreboard_slot_count_bytes);

    let mut unpreserved_cs_reg_count_bytes = [0u8; 4];
    cursor.read_exact(&mut unpreserved_cs_reg_count_bytes)?;
    let unpreserved_cs_reg_count = u32::from_le_bytes(unpreserved_cs_reg_count_bytes);

    let mut pad_bytes = [0u8; 4];
    cursor.read_exact(&mut pad_bytes)?;
    let pad = u32::from_le_bytes(pad_bytes);

    data.set_position(pos as u64 + cursor.position());

    Ok(drm_panthor_csif_info {
        csg_slot_count,
        cs_slot_count,
        cs_reg_count,
        scoreboard_slot_count,
        unpreserved_cs_reg_count,
        pad,
    })
}

pub(crate) fn decode_drm_panthor_fw_info(
    data: &mut Cursor<impl AsRef<[u8]>>,
    data_size: usize,
) -> Result<drm_panthor_fw_info> {
    let pos = data.position() as usize;
    let cursor = &data.get_ref().as_ref()[pos..pos + data_size];
    let mut cursor = Cursor::new(cursor);

    let mut version_bytes = [0u8; 4];
    cursor.read_exact(&mut version_bytes)?;
    let version = u32::from_le_bytes(version_bytes);

    let mut features_bytes = [0u8; 4];
    cursor.read_exact(&mut features_bytes)?;
    let features = u32::from_le_bytes(features_bytes);

    let mut group_num_bytes = [0u8; 4];
    cursor.read_exact(&mut group_num_bytes)?;
    let group_num = u32::from_le_bytes(group_num_bytes);

    data.set_position(pos as u64 + cursor.position());

    Ok(drm_panthor_fw_info {
        version,
        features,
        group_num,
    })
}

pub(crate) fn decode_drm_panthor_dump_gpuva(
    data: &mut Cursor<impl AsRef<[u8]>>,
    data_size: usize,
) -> Result<(drm_panthor_dump_gpuva, Vec<u8>)> {
    let pos = data.position() as usize;
    let cursor = &data.get_ref().as_ref()[pos..pos + data_size];
    let mut cursor = Cursor::new(cursor);

    let mut addr_bytes = [0u8; 8];
    cursor.read_exact(&mut addr_bytes)?;
    let addr = u64::from_le_bytes(addr_bytes);

    let mut range_bytes = [0u8; 8];
    cursor.read_exact(&mut range_bytes)?;
    let range = u64::from_le_bytes(range_bytes);

    let mut start_offset_bytes = [0u8; 8];
    cursor.read_exact(&mut start_offset_bytes)?;
    let start_offset = u64::from_le_bytes(start_offset_bytes);

    let gpuva = drm_panthor_dump_gpuva {
        addr,
        range,
        start_offset,
    };

    let inline_data = cursor.get_ref().as_ref()
        [start_offset as usize..(start_offset + gpuva.range) as usize]
        .to_vec();

    data.set_position(pos as u64 + cursor.position());

    Ok((gpuva, inline_data))
}

pub(crate) fn decode_drm_panthor_dump_group_info(
    data: &mut Cursor<impl AsRef<[u8]>>,
    data_size: usize,
) -> Result<drm_panthor_dump_group_info> {
    let pos = data.position() as usize;
    let cursor = &data.get_ref().as_ref()[pos..pos + data_size];
    let mut cursor = Cursor::new(cursor);

    let mut queue_count_bytes = [0u8; 4];
    cursor.read_exact(&mut queue_count_bytes)?;
    let queue_count = u32::from_le_bytes(queue_count_bytes);

    let mut faulty_bitmask_bytes = [0u8; 4];
    cursor.read_exact(&mut faulty_bitmask_bytes)?;
    let faulty_bitmask = u32::from_le_bytes(faulty_bitmask_bytes);

    data.set_position(pos as u64 + cursor.position());

    Ok(drm_panthor_dump_group_info {
        queue_count,
        faulty_bitmask,
    })
}

pub(crate) fn decode_drm_panthor_dump_queue_info(
    data: &mut Cursor<impl AsRef<[u8]>>,
    data_size: usize,
) -> Result<drm_panthor_dump_queue_info> {
    let pos = data.position() as usize;
    let cursor = &data.get_ref().as_ref()[pos..pos + data_size];
    let mut cursor = Cursor::new(cursor);

    let mut flags_bytes = [0u8; 4];
    cursor.read_exact(&mut flags_bytes)?;
    let flags = u32::from_le_bytes(flags_bytes);

    let mut cs_id_bytes = [0u8; 4];
    cursor.read_exact(&mut cs_id_bytes)?;
    let cs_id = i32::from_le_bytes(cs_id_bytes);

    let mut ringbuf_gpuva_bytes = [0u8; 8];
    cursor.read_exact(&mut ringbuf_gpuva_bytes)?;
    let ringbuf_gpuva = u64::from_le_bytes(ringbuf_gpuva_bytes);

    let mut ringbuf_insert_bytes = [0u8; 8];
    cursor.read_exact(&mut ringbuf_insert_bytes)?;
    let ringbuf_insert = u64::from_le_bytes(ringbuf_insert_bytes);

    let mut ringbuf_extract_bytes = [0u8; 8];
    cursor.read_exact(&mut ringbuf_extract_bytes)?;
    let ringbuf_extract = u64::from_le_bytes(ringbuf_extract_bytes);

    let mut ringbuf_size_bytes = [0u8; 8];
    cursor.read_exact(&mut ringbuf_size_bytes)?;
    let ringbuf_size = u64::from_le_bytes(ringbuf_size_bytes);

    data.set_position(pos as u64 + cursor.position());

    Ok(drm_panthor_dump_queue_info {
        flags,
        cs_id,
        ringbuf_gpuva,
        ringbuf_insert,
        ringbuf_extract,
        ringbuf_size,
    })
}
