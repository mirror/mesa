/*
 * Copyright © 2024 Qualcomm Innovation Center, Inc. All Rights Reserved.
 * SPDX-License-Identifier: MIT
 */

use std::fmt::Display;
use std::ops::{BitAnd, BitAndAssign, BitOrAssign};
use std::str::FromStr;

#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(u8)]
pub enum VclDebugFlags {
    Empty = 0,
    Info = 1 << 0,
    Ioctl = 1 << 1,
    Error = 1 << 2,
    Vtest = 1 << 4,
    All = 0b1111,
}

impl VclDebugFlags {
    pub fn contains(&self, flags: Self) -> bool {
        (*self & flags) != Self::Empty
    }
}

impl Display for VclDebugFlags {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            VclDebugFlags::Empty => f.write_str("empty"),
            VclDebugFlags::All => f.write_str("all"),
            _ => {
                let mut strings = Vec::new();
                if self.contains(VclDebugFlags::Info) {
                    strings.push("info");
                }
                if self.contains(VclDebugFlags::Ioctl) {
                    strings.push("ioctl");
                }
                if self.contains(VclDebugFlags::Error) {
                    strings.push("error")
                }
                if self.contains(VclDebugFlags::Vtest) {
                    strings.push("vtest")
                }
                f.write_str(&strings.join(","))
            }
        }
    }
}

impl FromStr for VclDebugFlags {
    type Err = String;

    fn from_str(flag: &str) -> Result<Self, <Self as FromStr>::Err> {
        match flag {
            "info" => Ok(VclDebugFlags::Info),
            "ioctl" => Ok(VclDebugFlags::Ioctl),
            "error" => Ok(VclDebugFlags::Error),
            "vtest" => Ok(VclDebugFlags::Vtest),
            "all" => Ok(VclDebugFlags::All),
            _ => Err(format!("Unknown debug flag: {}", flag)),
        }
    }
}

impl BitAndAssign for VclDebugFlags {
    fn bitand_assign(&mut self, flags: Self) {
        let self_u8: &mut u8 = unsafe { std::mem::transmute(self) };
        *self_u8 &= flags as u8;
    }
}

impl BitAnd for VclDebugFlags {
    type Output = Self;
    fn bitand(mut self, flags: Self) -> Self::Output {
        self &= flags;
        self
    }
}

impl BitOrAssign for VclDebugFlags {
    fn bitor_assign(&mut self, flags: Self) {
        let self_u8: &mut u8 = unsafe { std::mem::transmute(self) };
        *self_u8 |= flags as u8;
    }
}

pub struct VclDebug {
    pub flags: VclDebugFlags,
}

#[macro_export]
macro_rules! log {
    ( $level:expr, $( $t:tt )* ) => {
        let debug = crate::dev::renderer::Vcl::debug();
        if debug.flags.contains($level) {
            eprintln!("vcl: {}: {}", $level, format_args!($( $t )*));
        }
    };
}

#[cfg(test)]
mod test {
    use super::*;

    #[test]
    fn flags() {
        let mut flags = VclDebugFlags::Empty;
        assert_ne!(flags & VclDebugFlags::Ioctl, VclDebugFlags::Ioctl);
        flags |= VclDebugFlags::Info;
        assert_eq!(flags, VclDebugFlags::Info);
        flags |= VclDebugFlags::Ioctl;
        assert_ne!(flags, VclDebugFlags::Ioctl);
        assert_eq!(flags & VclDebugFlags::Ioctl, VclDebugFlags::Ioctl);
        assert_eq!(flags & VclDebugFlags::Info, VclDebugFlags::Info);
        assert_eq!(flags.to_string(), "info,ioctl");
    }
}
