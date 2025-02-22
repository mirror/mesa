// Copyright © 2023 Collabora, Ltd.
// SPDX-License-Identifier: MIT

use crate::args::{DisplayArgs, Modifier, OpSource};
use proc_macro::TokenStream;
use proc_macro2::{Span, TokenStream as TokenStream2};
use syn::*;

macro_rules! accumulate_error {
    // This macro takes an expression of type `expr` and prints
    // it as a string along with its result.
    // The `expr` designator is used for expressions.
    ($errors:ident, $var:ident) => {
        // `stringify!` will convert the expression *as it is* into a string.
        let $var = match $var {
            Ok(x) => Some(x),
            Err(x) => {
                $errors.push(x);
                None
            }
        };
    };
}

pub fn derive_display_op(input: TokenStream) -> TokenStream {
    let DeriveInput {
        ident, data, attrs, ..
    } = parse_macro_input!(input);

    if let Data::Enum(e) = data {
        let mut fmt_dsts_cases = TokenStream2::new();
        let mut fmt_op_cases = TokenStream2::new();
        for v in e.variants {
            let case = v.ident;
            fmt_dsts_cases.extend(quote! {
                #ident::#case(x) => x.fmt_dsts(f),
            });
            fmt_op_cases.extend(quote! {
                #ident::#case(x) => x.fmt_op(f),
            });
        }
        quote! {
            impl DisplayOp for #ident {
                fn fmt_dsts(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
                    match self {
                        #fmt_dsts_cases
                    }
                }

                fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
                    match self {
                        #fmt_op_cases
                    }
                }
            }
        }
        .into()
    } else if let Data::Struct(s) = data {
        // Missing:
        // - OpFSetP: is_trivial
        // - OpFSwzAdd: custom format
        // - OpDSetP: is_trivial
        // - OpIAdd2X (missing skip if src zero)
        // - OpISetP is_trivial
        // - OpLea if modifier add src(?)
        // - OpLeaX ^
        // - OpShf SRC modifier
        // - OpI2I ^
        // - OpPLop3 what are ops?
        // - OpMov quadlines
        // - OpPrmt custom format split array
        // - OpSuSt: How to print mask?
        // - OpCCtl: op not printed

        // TODO:
        // - modifier after SRC
        // - modifier add "."
        // - op_format for modifier
        // - skip src if 0
        // - AttrAccess

        let args = match attrs.iter().filter(|x|  x.path().is_ident("display_op")).next() {
            Some(attr) => attr.parse_args::<DisplayArgs>(),
            None => {
                ident.to_string()
                    .to_lowercase()
                    .strip_prefix("op")
                    .ok_or_else(|| syn::Error::new(Span::call_site(), "Cannot convert struct name, please use #[display_op(format = )]"))
                    .map(|x| DisplayArgs { format: LitStr::new(x, ident.span()) })
            }
        };

        let modifiers = Modifier::parse_all(&s);
        let srcs = OpSource::parse_all(&s);

        let mut errors = Vec::new();
        accumulate_error!(errors, args);
        accumulate_error!(errors, modifiers);
        accumulate_error!(errors, srcs);

        let error = errors.into_iter().reduce(|mut a, b| {
            a.combine(b);
            a
        });
        if let Some(err) = error {
            return err.into_compile_error().into();
        }

        // No panic, we already handled the errors
        let args = args.unwrap();
        let modifiers = modifiers.unwrap();
        let srcs = srcs.unwrap();

        let fmt = args.format;
        let q: TokenStream = quote! {
            impl DisplayOp for #ident {
                fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
                    write!(f, #fmt)?;
                    #(#modifiers)*
                    #(#srcs)*

                    Ok(())
                }
            }
        }
        .into();
        //eprintln!("{}", q.to_string());
        q
    } else {
        panic!("Cannot derive type");
    }
}
