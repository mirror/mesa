// SPDX-License-Identifier: MIT
// SPDX-CopyrightText: Copyright Collabora 2024

//! This tool is used to parse a coredump from the Panthor driver.

use std::fmt;
use std::fs::File;
use std::io;
use std::io::Cursor;
use std::io::Write as IoWrite;

use crate::context::*;
use crate::parse::*;

mod context;
mod parse;

// PANT
const MAGIC: u32 = 0x544e4150;

#[derive(Debug)]
pub enum CoredumpError {
    InvalidHeaderMagic {
        magic: u32,
        position: u64,
    },
    UnexpectedHeaderType {
        expected: u32,
        found: u32,
        position: u64,
    },
    IoError(io::Error),
    FmtError(fmt::Error),
    InvalidDump(&'static str),
}

impl fmt::Display for CoredumpError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            CoredumpError::InvalidHeaderMagic { magic, position } => {
                write!(
                    f,
                    "Invalid header magic: 0x{:x} at position 0x{:x}. Expected 0x{:x}",
                    *magic, *position, MAGIC
                )
            }
            CoredumpError::UnexpectedHeaderType {
                expected: expected_ty,
                found: found_ty,
                position,
            } => write!(
                f,
                "Unexpected header at this point. Expected {} found {} at position 0x{:x}",
                *expected_ty, *found_ty, *position,
            ),
            CoredumpError::IoError(err) => write!(f, "Possibly malformed dump: {}", err),
            CoredumpError::FmtError(err) => write!(f, "{}", err),
            CoredumpError::InvalidDump(reason) => write!(f, "Invalid dump: {}", reason),
        }
    }
}

impl std::error::Error for CoredumpError {}

impl From<io::Error> for CoredumpError {
    fn from(err: io::Error) -> CoredumpError {
        CoredumpError::IoError(err)
    }
}

impl From<fmt::Error> for CoredumpError {
    fn from(err: fmt::Error) -> CoredumpError {
        CoredumpError::FmtError(err)
    }
}

pub(crate) type Result<T> = std::result::Result<T, CoredumpError>;

fn write_output(output: &str, output_file: Option<&String>) {
    if let Some(output_file) = output_file {
        let mut file = File::create(output_file).expect("Failed to create output file");
        file.write_all(output.as_bytes())
            .expect("Failed to write to output file");
    } else {
        println!("{}", output);
    }
}

fn main() {
    let args: Vec<String> = std::env::args().collect();

    let dump_file = if args.len() < 2 {
        eprintln!("Usage: panthor_coredump <coredump> [<output>]");
        std::process::exit(1);
    } else {
        &args[1]
    };

    let output_file = if args.len() > 2 { Some(&args[2]) } else { None };

    let dump_file =
        std::fs::read(dump_file).expect(&format!("Failed to read coredump at {}", dump_file));
    let cursor = Cursor::new(&dump_file[..]);

    let mut decode_ctx = DecodeCtx::new(cursor).unwrap();

    match decode_ctx.decode() {
        Ok(output) => {
            write_output(&output, output_file);
        }
        Err(e) => {
            eprintln!("Failed to decode coredump: {}", e);

            eprintln!("The output may be incomplete or invalid");
            eprintln!("The decoder position is {}", decode_ctx.position());

            let output = decode_ctx.output();
            write_output(&output, output_file);
            std::process::exit(1);
        }
    }
}
