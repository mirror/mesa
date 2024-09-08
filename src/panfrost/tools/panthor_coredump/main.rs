// SPDX-License-Identifier: MIT
// SPDX-CopyrightText: Copyright Collabora 2024

//! This tool is used to parse a coredump from the Panthor driver.

use std::fs::File;
use std::io::Cursor;
use std::io::Write as IoWrite;

use crate::context::*;
use crate::parse::*;

mod context;
mod parse;

// PANT
const MAGIC: u32 = 0x544e4150;

fn main() {
    let args: Vec<String> = std::env::args().collect();

    let dump_non_faulty = args.contains(&String::from("--dump-non-faulty"));

    let dump_file = if args.len() < 2 || (dump_non_faulty && args.len() < 3) {
        eprintln!("Usage: panthor_coredump <coredump> [<output>] [--dump-non-faulty]");
        std::process::exit(1);
    } else {
        &args[1]
    };

    let output_file = if dump_non_faulty && args.len() > 3 {
        Some(&args[2])
    } else if !dump_non_faulty && args.len() > 2 {
        Some(&args[2])
    } else {
        None
    };

    let dump_file =
        std::fs::read(dump_file).expect(&format!("Failed to read coredump at {}", dump_file));
    let cursor = Cursor::new(&dump_file[..]);

    let mut decode_ctx = DecodeCtx::new(cursor, dump_non_faulty);
    let parsed_data = decode_ctx.decode();

    if let Some(output_file) = output_file {
        let mut file = File::create(output_file).expect("Failed to create output file");
        file.write_all(parsed_data.as_bytes())
            .expect("Failed to write to output file");
    } else {
        println!("{}", parsed_data);
    }
}
