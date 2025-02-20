use crate::api::icd::CLResult;
use crate::api::icd::DISPATCH;
use crate::core::device::*;
use crate::core::version::*;

use mesa_rust::pipe::screen::ScreenVMAllocation;
use mesa_rust::util::vm::VM;
use mesa_rust_gen::*;
use rusticl_opencl_gen::*;

use std::cmp;
use std::env;
use std::num::NonZeroU64;
use std::ops::Deref;
use std::ptr;
use std::ptr::addr_of;
use std::ptr::addr_of_mut;
use std::sync::Mutex;
use std::sync::Once;

/// Maximum size a pixel can be across all supported image formats.
pub const MAX_PIXEL_SIZE_BYTES: u64 = 4 * 4;

pub struct PlatformVM<'a> {
    vm: Mutex<VM>,
    // we make use of the drop to automatically free the reserved VM
    _dev_allocs: Vec<ScreenVMAllocation<'a>>,
}

impl Deref for PlatformVM<'_> {
    type Target = Mutex<VM>;

    fn deref(&self) -> &Self::Target {
        &self.vm
    }
}

#[repr(C)]
pub struct Platform {
    dispatch: &'static cl_icd_dispatch,
    pub devs: Vec<Device>,
    // lifetime has to match the one of devs
    pub vm: Option<PlatformVM<'static>>,
}

pub enum PerfDebugLevel {
    None,
    Once,
    Spam,
}

pub struct PlatformDebug {
    pub allow_invalid_spirv: bool,
    pub clc: bool,
    pub max_grid_size: u32,
    pub memory: bool,
    pub nir: bool,
    pub no_variants: bool,
    pub perf: PerfDebugLevel,
    pub program: bool,
    pub reuse_context: bool,
    pub sync_every_event: bool,
    pub validate_spirv: bool,
}

pub struct PlatformFeatures {
    pub fp16: bool,
    pub fp64: bool,
}

static PLATFORM_ENV_ONCE: Once = Once::new();
static PLATFORM_ONCE: Once = Once::new();

macro_rules! gen_cl_exts {
    (@COUNT $e:expr) => { 1 };
    (@COUNT $e:expr, $($es:expr),+) => { 1 + gen_cl_exts!(@COUNT $($es),*) };

    (@CONCAT $e:tt) => { $e };
    (@CONCAT $e:tt, $($es:tt),+) => { concat!($e, ' ', gen_cl_exts!(@CONCAT $($es),*)) };

    ([$(($major:expr, $minor:expr, $patch:expr, $ext:tt)$(,)?)+]) => {
        pub static PLATFORM_EXTENSION_STR: &str = concat!(gen_cl_exts!(@CONCAT $($ext),*));
        pub static PLATFORM_EXTENSIONS: [cl_name_version; gen_cl_exts!(@COUNT $($ext),*)] = [
            $(mk_cl_version_ext($major, $minor, $patch, $ext)),+
        ];
    }
}
gen_cl_exts!([
    (1, 0, 0, "cl_khr_byte_addressable_store"),
    (1, 0, 0, "cl_khr_create_command_queue"),
    (1, 0, 0, "cl_khr_expect_assume"),
    (1, 0, 0, "cl_khr_extended_versioning"),
    (1, 0, 0, "cl_khr_icd"),
    (1, 0, 0, "cl_khr_il_program"),
    (1, 0, 0, "cl_khr_spirv_no_integer_wrap_decoration"),
    (1, 0, 0, "cl_khr_suggested_local_work_size"),
]);

static mut PLATFORM: Platform = Platform {
    dispatch: &DISPATCH,
    devs: Vec::new(),
    vm: None,
};
static mut PLATFORM_DBG: PlatformDebug = PlatformDebug {
    allow_invalid_spirv: false,
    clc: false,
    max_grid_size: 0,
    memory: false,
    nir: false,
    no_variants: false,
    perf: PerfDebugLevel::None,
    program: false,
    reuse_context: true,
    sync_every_event: false,
    validate_spirv: false,
};
static mut PLATFORM_FEATURES: PlatformFeatures = PlatformFeatures {
    fp16: false,
    fp64: false,
};

fn load_env() {
    // SAFETY: no other references exist at this point
    let debug = unsafe { &mut *addr_of_mut!(PLATFORM_DBG) };
    if let Ok(debug_flags) = env::var("RUSTICL_DEBUG") {
        for flag in debug_flags.split(',') {
            match flag {
                "allow_invalid_spirv" => debug.allow_invalid_spirv = true,
                "clc" => debug.clc = true,
                "memory" => debug.memory = true,
                "nir" => debug.nir = true,
                "no_reuse_context" => debug.reuse_context = false,
                "no_variants" => debug.no_variants = true,
                "perf" => debug.perf = PerfDebugLevel::Once,
                "perfspam" => debug.perf = PerfDebugLevel::Spam,
                "program" => debug.program = true,
                "sync" => debug.sync_every_event = true,
                "validate" => debug.validate_spirv = true,
                "" => (),
                _ => eprintln!("Unknown RUSTICL_DEBUG flag found: {}", flag),
            }
        }
    }

    debug.max_grid_size = env::var("RUSTICL_MAX_WORK_GROUPS")
        .ok()
        .and_then(|s| s.parse().ok())
        .unwrap_or(u32::MAX);

    // SAFETY: no other references exist at this point
    let features = unsafe { &mut *addr_of_mut!(PLATFORM_FEATURES) };
    if let Ok(feature_flags) = env::var("RUSTICL_FEATURES") {
        for flag in feature_flags.split(',') {
            match flag {
                "fp16" => features.fp16 = true,
                "fp64" => features.fp64 = true,
                "" => (),
                _ => eprintln!("Unknown RUSTICL_FEATURES flag found: {}", flag),
            }
        }
    }
}

impl Platform {
    pub fn as_ptr(&self) -> cl_platform_id {
        ptr::from_ref(self) as cl_platform_id
    }

    pub fn get() -> &'static Self {
        debug_assert!(PLATFORM_ONCE.is_completed());
        // SAFETY: no mut references exist at this point
        unsafe { &*addr_of!(PLATFORM) }
    }

    pub fn dbg() -> &'static PlatformDebug {
        debug_assert!(PLATFORM_ENV_ONCE.is_completed());
        unsafe { &*addr_of!(PLATFORM_DBG) }
    }

    pub fn features() -> &'static PlatformFeatures {
        debug_assert!(PLATFORM_ENV_ONCE.is_completed());
        unsafe { &*addr_of!(PLATFORM_FEATURES) }
    }

    fn alloc_vm(devs: &[Device]) -> Option<PlatformVM<'_>> {
        // No need to check system SVM devices
        let devs = devs.iter().filter(|dev| !dev.system_svm_supported());

        let (start, end) = devs.clone().filter_map(|dev| dev.vm_alloc_range()).reduce(
            |(min_a, max_a), (min_b, max_b)| (cmp::max(min_a, min_b), cmp::min(max_a, max_b)),
        )?;

        // Allocate 1/8 of the available VM. No specific reason for this limit. Might have to bump
        // this later, but it's probably fine as there is plenty of VM available.
        let size = NonZeroU64::new((end.get() / 8).next_power_of_two())?;
        if start > size {
            return None;
        }

        let mut allocs = Vec::new();
        for dev in devs {
            allocs.push(dev.screen().alloc_vm(size, size)?);
        }

        Some(PlatformVM {
            vm: Mutex::new(VM::new(size, size)),
            _dev_allocs: allocs,
        })
    }

    fn init(&'static mut self) {
        unsafe {
            glsl_type_singleton_init_or_ref();
        }

        self.devs = Device::all();
        self.vm = Self::alloc_vm(&self.devs);

        if self
            .devs
            .iter()
            .any(|dev| !dev.system_svm_supported() && dev.svm_supported())
            && self.vm.is_none()
        {
            // TODO: in theory we should also remove the exposed SVM extension, but...
            eprintln!("rusticl: could not initialize SVM support");
        }
    }

    pub fn init_once() {
        PLATFORM_ENV_ONCE.call_once(load_env);
        // SAFETY: no concurrent static mut access due to std::Once
        #[allow(static_mut_refs)]
        PLATFORM_ONCE.call_once(|| unsafe { PLATFORM.init() });
    }
}

impl Drop for Platform {
    fn drop(&mut self) {
        unsafe {
            glsl_type_singleton_decref();
        }
    }
}

pub trait GetPlatformRef {
    fn get_ref(&self) -> CLResult<&'static Platform>;
}

impl GetPlatformRef for cl_platform_id {
    fn get_ref(&self) -> CLResult<&'static Platform> {
        if !self.is_null() && *self == Platform::get().as_ptr() {
            Ok(Platform::get())
        } else {
            Err(CL_INVALID_PLATFORM)
        }
    }
}

#[macro_export]
macro_rules! perf_warning {
    (@PRINT $format:tt, $($arg:tt)*) => {
        eprintln!(std::concat!("=== Rusticl perf warning: ", $format, " ==="), $($arg)*)
    };

    ($format:tt $(, $arg:tt)*) => {
        match $crate::core::platform::Platform::dbg().perf {
            $crate::core::platform::PerfDebugLevel::Once => {
                static PERF_WARN_ONCE: std::sync::Once = std::sync::Once::new();
                PERF_WARN_ONCE.call_once(|| {
                    perf_warning!(@PRINT $format, $($arg)*);
                })
            },
            $crate::core::platform::PerfDebugLevel::Spam => perf_warning!(@PRINT $format, $($arg)*),
            _ => (),
        }
    };
}
