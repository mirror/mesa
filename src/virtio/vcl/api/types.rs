macro_rules! cl_callback {
    ($cb:ident {
        $($p:ident : $ty:ty,)*
    }) => {
        #[allow(dead_code)]
        pub type $cb = unsafe extern "C" fn(
            $($p: $ty,)*
        );
    }
}

cl_callback!(
    CreateContextCB {
        errinfo: *const ::std::os::raw::c_char,
        private_info: *const ::std::ffi::c_void,
        cb: usize,
        user_data: *mut ::std::ffi::c_void,
    }
);
