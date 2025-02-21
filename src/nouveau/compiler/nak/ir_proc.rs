// Copyright © 2023 Collabora, Ltd.
// SPDX-License-Identifier: MIT

extern crate proc_macro;
extern crate proc_macro2;
#[macro_use]
extern crate quote;
extern crate syn;

use compiler_proc::as_slice::*;
use proc_macro::TokenStream;
use proc_macro2::{Span, TokenStream as TokenStream2};
use syn::*;

#[proc_macro_derive(SrcsAsSlice, attributes(src_type))]
pub fn derive_srcs_as_slice(input: TokenStream) -> TokenStream {
    derive_as_slice(input, "Src", "src_type", "SrcType")
}

#[proc_macro_derive(DstsAsSlice, attributes(dst_type))]
pub fn derive_dsts_as_slice(input: TokenStream) -> TokenStream {
    derive_as_slice(input, "Dst", "dst_type", "DstType")
}


enum RawArg {
    Literal(LitStr),
    AssignLit(Ident, LitStr),
    AssignType(Ident, Type),
}

impl RawArg {
    pub fn span(&self) -> Span {
        match self {
            RawArg::Literal(x) => x.span(),
            RawArg::AssignLit(x, _) => x.span(),
            RawArg::AssignType(x, _) => x.span(),
        }
    }
}

impl syn::parse::Parse for RawArg {
    fn parse(input: syn::parse::ParseStream) -> syn::Result<Self> {
        let val = if input.peek(syn::LitStr) {
            RawArg::Literal(input.parse()?)
        } else {
            let lhs = input.parse()?;
            input.parse::<Token![=]>()?;
            if input.peek(syn::LitStr) {
                let rhs = input.parse()?;
                RawArg::AssignLit(lhs, rhs)
            } else {
                let rhs = input.parse()?;
                RawArg::AssignType(lhs, rhs)
            }
        };
        Ok(val)
    }
}

#[derive(Default, Debug)]
struct ModifierArgs {
    name: Option<LitStr>,
    def: Option<Type>,
}

impl syn::parse::Parse for ModifierArgs {
    fn parse(input: syn::parse::ParseStream) -> syn::Result<Self> {
        let mut args = ModifierArgs::default();

        if input.is_empty() {
            return Ok(args);
        }

        for arg in syn::punctuated::Punctuated::<RawArg, Token![,]>::parse_terminated(input)?.iter() {
            match arg {
                RawArg::Literal(name) => args.name = Some(name.clone()),
                RawArg::AssignType(d, def) if d == "def" => args.def = Some(def.clone()),
                x => return Err(syn::Error::new(x.span(), "Unhandled argument"))
            }
        }

        Ok(args)
    }
}

#[derive(Debug)]
enum Modifier {
    BoolMod {
        ident: Ident,
        name: LitStr
    },
    EnumMod {
        ident: Ident,
        def: Option<Type>
    },
}

impl Modifier {
    fn parse_field(field: &Field) -> syn::Result<Option<Self>> {
        let Some(attr) = field.attrs.iter().filter(|x| x.path().is_ident("modifier")).next() else {
            return Ok(None);
        };
        let is_bool = match &field.ty {
            Type::Path(TypePath {
                qself: None,
                path
            }) if path.is_ident("bool") => true,
            _ => false,
        };

        let args: ModifierArgs = match attr.meta {
            Meta::Path(_) => ModifierArgs::default(),
            _ => attr.parse_args()?,
        };
        let ident = field.ident.as_ref().unwrap().clone();
        let modif = if is_bool {
            Modifier::BoolMod {
                ident,
                name: args.name.unwrap_or_else(|| {
                    let ident = field.ident.as_ref().unwrap();
                    let fname = ident.to_string();
                    LitStr::new(&format!(".{fname}"), ident.span())
                })
            }
        } else {
            Modifier::EnumMod {
                ident,
                def: args.def.clone()
            }
        };
        Ok(Some(modif))
    }

    fn parse_all(data: &DataStruct) -> syn::Result<Vec<Self>> {
        data.fields
            .iter()
            .filter_map(|x| Self::parse_field(x).transpose())
            .collect()
    }
}

impl quote::ToTokens for Modifier {
    fn to_tokens(&self, tokens: &mut TokenStream2) {
        let t = match self {
            Modifier::BoolMod { ident, name } => {
                quote! {
                    if self.#ident {
                        write!(f, #name)?;
                    }
                }
            },
            Modifier::EnumMod { ident, def: None } => {
                quote! {
                    write!(f, "{}", self.#ident)?;
                }
            },
            Modifier::EnumMod { ident, def: Some(def)} => {
                quote! {
                    if self.#ident != #def {
                        write!(f, "{}", self.#ident)?;
                    }
                }
            },
        };
        t.to_tokens(tokens);
    }
}

#[derive(Debug)]
struct OpSource {
    ident: Ident,
    array_len: usize,
    // format: String
}

impl OpSource {
    fn parse_field(field: &Field) -> syn::Result<Option<Self>> {
        let is_type_src = |x: &Type| match x {
            Type::Path(TypePath {
                qself: None,
                path
            }) if path.is_ident("Src") => true,
            _ => false,
        };
        let array_len: usize = match &field.ty {
            Type::Array(TypeArray {
                elem,
                len: Expr::Lit(ExprLit {
                    lit: Lit::Int(len),
                    ..
                }),
                ..
            }) if is_type_src(&elem) => len.base10_parse()?,
            x if is_type_src(&x) => 0,
            _ => return Ok(None),
        };
        /*let attr = field.attrs.iter().filter(|x| x.path().is_ident("src_format")).next();
        let format = match attr {
            Some(x) => x.parse_args::<LitStr>()?,
            None => LitStr::new("{}", Span::call_site()),
        };*/

        Ok(Some(OpSource {
            ident: field.ident.as_ref().unwrap().clone(),
            array_len,
        }))
    }

    fn parse_all(data: &DataStruct) -> syn::Result<Vec<Self>> {
        data.fields
        .iter()
        .filter_map(|x| Self::parse_field(x).transpose())
        .collect()
    }
}

impl quote::ToTokens for OpSource {
    fn to_tokens(&self, tokens: &mut TokenStream2) {
        let ident = &self.ident;
        let t = if self.array_len == 0 {
            // not an array!
            quote! {
                write!(f, " {}", self.#ident)?;
            }
        } else {
            let index = 0..self.array_len;
            quote! {
                #(
                    write!(f, " {}", self.#ident[#index])?;
                )*
            }
        };
        t.to_tokens(tokens);
    }
}


#[derive(Debug)]
struct DisplayArgs {
    format: LitStr
}

impl syn::parse::Parse for DisplayArgs {
    fn parse(input: syn::parse::ParseStream) -> syn::Result<Self> {
        let mut format = None;

        for arg in syn::punctuated::Punctuated::<RawArg, Token![,]>::parse_terminated(input)?.iter() {
            match arg {
                RawArg::AssignLit(f, fmt) if f == "format" => {
                    format = Some(fmt.clone());
                }
               x => return Err(syn::Error::new(x.span(), "Unhandled argument"))
            }
        }
        if format == None {
            return Err(input.error("Cannot find format"));
        }

        Ok(DisplayArgs {
            format: format.unwrap()
        })
    }
}

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

#[proc_macro_derive(DisplayOp, attributes(display_op, modifier))]
pub fn enum_derive_display_op(input: TokenStream) -> TokenStream {
    let DeriveInput { ident, data, attrs, .. } = parse_macro_input!(input);

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
        let args = attrs.iter().filter(|x|  x.path().is_ident("display_op")).next()
            .ok_or_else(|| syn::Error::new(Span::call_site(), "No display_op attribute found"))
            .and_then(|attr| attr.parse_args::<DisplayArgs>());
        let modifiers = Modifier::parse_all(&s);
        let srcs = OpSource::parse_all(&s);

        let mut errors = Vec::new();
        accumulate_error!(errors, args);
        accumulate_error!(errors, modifiers);
        accumulate_error!(errors, srcs);

        let error = errors.into_iter()
            .reduce(|mut a, b| {
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
