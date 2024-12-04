/// Casts a &[T] to a [&u8] without copying.
/// Inspired by cast_slice from the bytemuck crate. Drop this copy once external crates are supported.
///
/// # Safety
///
/// T must not contain any uninitialized bytes such as padding.
#[inline]
pub unsafe fn as_byte_slice<T>(t: &[T]) -> &[u8] {
    let new_len = core::mem::size_of_val(t) / core::mem::size_of::<u8>();
    unsafe { core::slice::from_raw_parts(t.as_ptr().cast(), new_len) }
}

/// Casts a &mut [T] to a &mut [u8] without copying.
/// Inspired by cast_slice from the bytemuck crate. Drop this copy once external crates are supported.
///
/// # Safety
///
/// T must not contain any uninitialized bytes such as padding.
#[inline]
pub unsafe fn as_mut_byte_slice<T>(t: &mut [T]) -> &mut [u8] {
    let new_len = core::mem::size_of_val(t) / core::mem::size_of::<u8>();
    unsafe { core::slice::from_raw_parts_mut(t.as_mut_ptr() as *mut u8, new_len) }
}
