// Copyright © 2023 Collabora, Ltd.
// SPDX-License-Identifier: MIT

extern crate proc_macro;
extern crate proc_macro2;
#[macro_use]
extern crate quote;
extern crate syn;

use compiler_proc::as_slice::*;
use proc_macro::TokenStream;
use proc_macro2::TokenStream as TokenStream2;
use syn::*;

mod args;
mod display_op;

#[proc_macro_derive(SrcsAsSlice, attributes(src_type))]
pub fn derive_srcs_as_slice(input: TokenStream) -> TokenStream {
    derive_as_slice(input, "Src", "src_type", "SrcType")
}

#[proc_macro_derive(DstsAsSlice, attributes(dst_type))]
pub fn derive_dsts_as_slice(input: TokenStream) -> TokenStream {
    derive_as_slice(input, "Dst", "dst_type", "DstType")
}

#[proc_macro_derive(FromVariants)]
pub fn derive_from_variants(input: TokenStream) -> TokenStream {
    let DeriveInput { ident, data, .. } = parse_macro_input!(input);
    let enum_type = ident;

    let mut impls = TokenStream2::new();

    if let Data::Enum(e) = data {
        for v in e.variants {
            let var_ident = v.ident;
            let from_type = match v.fields {
                Fields::Unnamed(FieldsUnnamed { unnamed, .. }) => unnamed,
                _ => panic!("Expected Op(OpFoo)"),
            };

            let quote = quote! {
                impl From<#from_type> for #enum_type {
                    fn from (op: #from_type) -> #enum_type {
                        #enum_type::#var_ident(op)
                    }
                }
            };

            impls.extend(quote);
        }
    }

    impls.into()
}

#[proc_macro_derive(DisplayOp, attributes(display_op, modifier, op_format))]
pub fn derive_display_op(input: TokenStream) -> TokenStream {
    display_op::derive_display_op(input)
}
