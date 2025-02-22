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
    Ident(Ident),
    Literal(LitStr),
    AssignLit(Ident, LitStr),
    AssignType(Ident, Type),
}

impl RawArg {
    pub fn span(&self) -> Span {
        match self {
            RawArg::Literal(x) => x.span(),
            RawArg::Ident(x) | 
            RawArg::AssignLit(x, _) |
            RawArg::AssignType(x, _) => x.span(),
        }
    }
}

impl syn::parse::Parse for RawArg {
    fn parse(input: syn::parse::ParseStream) -> syn::Result<Self> {
        if input.peek(syn::LitStr) {
            return Ok(RawArg::Literal(input.parse()?))
        }
        let ident = input.parse()?;
        if !input.peek(Token![=]) {
            return Ok(RawArg::Ident(ident));
        }
        input.parse::<Token![=]>()?;
        let val = if input.peek(syn::LitStr) {
            let rhs = input.parse()?;
            RawArg::AssignLit(ident, rhs)
        } else {
            let rhs = input.parse()?;
            RawArg::AssignType(ident, rhs)
        };
        Ok(val)
    }
}

#[derive(Default, Debug)]
struct ModifierArgs {
    name: Option<LitStr>,
    name_false: Option<LitStr>,
    def: Option<Type>,
}

impl syn::parse::Parse for ModifierArgs {
    fn parse(input: syn::parse::ParseStream) -> syn::Result<Self> {
        let mut args = ModifierArgs::default();

        if input.is_empty() {
            return Ok(args);
        }

        let unhandled_err = |span: Span| return Err(syn::Error::new(span, "Unhandled argument")) as syn::Result<()>;

        for arg in syn::punctuated::Punctuated::<RawArg, Token![,]>::parse_terminated(input)?.iter() {
            match arg {
                RawArg::Literal(name) => {
                    if args.name.is_none() {
                        args.name = Some(name.clone());
                    } else if args.name_false.is_none() {
                        args.name_false = Some(name.clone())
                    } else {
                        unhandled_err(name.span())?;
                    }
                },
                RawArg::AssignType(d, def) if d == "def" => {
                    args.def.map_or(Ok(()), |_| return unhandled_err(d.span()))?;
                    args.def = Some(def.clone())
                },
                x => unhandled_err(x.span())?
            }
        }

        Ok(args)
    }
}

#[derive(Debug)]
enum ModifierType {
    BoolMod {
        name: LitStr,
        name_false: Option<LitStr>,
    },
    EnumMod {
        def: Option<Type>,
    },
}

struct Modifier {
    ident: Ident,
    array_len: usize,
    ty: ModifierType,
}

impl Modifier {
    fn parse_field(field: &Field) -> syn::Result<Option<Self>> {
        let Some(attr) = field.attrs.iter().filter(|x| x.path().is_ident("modifier")).next() else {
            return Ok(None);
        };

        let is_type_bool = |ty: &Type| matches!(ty, Type::Path(TypePath {
            qself: None,
            path
        }) if path.is_ident("bool"));

        let (array_len, is_bool) = match &field.ty {
            Type::Array(TypeArray {
                elem,
                len: Expr::Lit(ExprLit {
                    lit: Lit::Int(len),
                    ..
                }),
                ..
            }) => (len.base10_parse()?, is_type_bool(&elem)),
            ty => (0usize, is_type_bool(&ty)),
        };

        let args: ModifierArgs = match attr.meta {
            Meta::Path(_) => ModifierArgs::default(),
            _ => attr.parse_args()?,
        };
        let ident = field.ident.as_ref().unwrap().clone();
        let mod_ty = if is_bool {
            ModifierType::BoolMod {
                name_false: args.name_false,
                name: args.name.unwrap_or_else(|| {
                    let ident = field.ident.as_ref().unwrap();
                    let fname = ident.to_string();
                    LitStr::new(&format!(".{fname}"), ident.span())
                })
            }
        } else {
            ModifierType::EnumMod {
                def: args.def.clone()
            }
        };
        Ok(Some(Modifier {
            ident,
            array_len,
            ty: mod_ty,
        }))
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
        let Modifier { ident, array_len, ty } = self;

        let generate_no_arr = |ident| {
            match ty {
                ModifierType::BoolMod { name, name_false: None } => {
                    quote! {
                        if #ident {
                            write!(f, #name)?;
                        }
                    }
                },
                ModifierType::BoolMod { name, name_false: Some(name_false) } => {
                    quote! {
                        write!(f, "{}", if #ident { #name } else { #name_false })?;
                    }
                },
                ModifierType::EnumMod { def: None } => {
                    quote! {
                        write!(f, "{}", #ident)?;
                    }
                },
                ModifierType::EnumMod { def: Some(def)} => {
                    quote! {
                        if #ident != #def {
                            write!(f, "{}", #ident)?;
                        }
                    }
                },
            }
        };
        let t = match array_len {
            0 => generate_no_arr(quote!{ self.#ident }),
            n => (0..*n).map(|i| {
                generate_no_arr(quote!{ self.#ident[#i]})
            }).collect()
        };
        t.to_tokens(tokens);
    }
}

#[derive(Default, Debug)]
struct OpSourceFormatArgs {
    addr: bool,
    addr_offset: Option<LitStr>,
    custom_format: Option<LitStr>,
    prefix: Option<LitStr>,
}

impl syn::parse::Parse for OpSourceFormatArgs {
    fn parse(input: syn::parse::ParseStream) -> syn::Result<Self> {
        let mut args = OpSourceFormatArgs::default();

        let unhandled_err = |span: Span| return Err(syn::Error::new(span, "Unhandled argument")) as syn::Result<()>;

        for arg in syn::punctuated::Punctuated::<RawArg, Token![,]>::parse_terminated(input)?.iter() {
            match arg {
                RawArg::Ident(x) if x == "addr" => {
                    args.addr = true;
                }
                RawArg::Literal(fmt) => {
                    args.custom_format.map_or(Ok(()), |_| return unhandled_err(fmt.span()))?;
                    args.custom_format = Some(fmt.clone());
                }
                RawArg::AssignLit(d, offset) if d == "offset" => {
                    args.addr_offset.map_or(Ok(()), |_| return unhandled_err(d.span()))?;
                    args.addr_offset = Some(offset.clone())
                },
                RawArg::AssignLit(d, prefix) if d == "prefix" => {
                    args.prefix.map_or(Ok(()), |_| return unhandled_err(d.span()))?;
                    args.prefix = Some(prefix.clone())
                },
                x => unhandled_err(x.span())?
            }
        }

        Ok(args)
    }
}

#[derive(Debug)]
enum OpSourceFormat {
    Plain,
    Addr {
        offset: Option<LitStr>
    },
    Custom(LitStr),
}

/// Tracks sources of instr as Src and Target
/// Sources can also be arrays of static size
#[derive(Debug)]
struct OpSource {
    ident: Ident,
    array_len: usize,
    prefix: String,
    format: OpSourceFormat
}

impl OpSource {
    fn parse_field(field: &Field) -> syn::Result<Option<Self>> {
        let is_type_src = |x: &Type| match x {
            Type::Path(TypePath {
                qself: None,
                path
            }) if path.is_ident("Src") || path.is_ident("Label") => true,
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
        let attr = field.attrs.iter().filter(|x| x.path().is_ident("op_format")).next();
        let args = match attr {
            Some(x) => x.parse_args()?,
            None => OpSourceFormatArgs::default(),
        };

        let prefix = args.prefix.map(|x| x.value()).unwrap_or_else(|| "".into());
        let format = if args.addr {
            OpSourceFormat::Addr {
                offset: args.addr_offset
            }
        } else if let Some(fmt) =  args.custom_format {
            OpSourceFormat::Custom(fmt)
        } else {
            OpSourceFormat::Plain
        };

        Ok(Some(OpSource {
            ident: field.ident.as_ref().unwrap().clone(),
            array_len,
            prefix,
            format,
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
        let generate_no_arr = |ident| {
            let arg = match &self.format {
                OpSourceFormat::Plain | OpSourceFormat::Custom(_) => quote!( #ident ),
                OpSourceFormat::Addr { offset: None } => quote!{ FmtAddr { src: &#ident, off: 0 } },
                OpSourceFormat::Addr { offset: Some(off) } => {
                    let off = Ident::new(&off.value(), off.span());
                    quote!{ FmtAddr { src: &#ident, off: self.#off}}
                },
            };
            let fstr = match &self.format {
                OpSourceFormat::Custom(fmt) => format!(" {}{}", self.prefix, fmt.value()),
                _ => format!(" {}{{}}", self.prefix),
            };
            quote! {
                write!(f, #fstr, #arg)?;
            }
        };

        let t = match self.array_len {
            0 => generate_no_arr(quote! { self.#ident }),
            n => (0..n).map(|i| generate_no_arr(quote!{ self.#ident[#i] })).collect()
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

#[proc_macro_derive(DisplayOp, attributes(display_op, modifier, op_format))]
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
