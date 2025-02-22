// Copyright © 2023 Collabora, Ltd.
// SPDX-License-Identifier: MIT

use proc_macro2::{Span, TokenStream as TokenStream2};
use syn::*;

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
            RawArg::Ident(x)
            | RawArg::AssignLit(x, _)
            | RawArg::AssignType(x, _) => x.span(),
        }
    }
}

impl syn::parse::Parse for RawArg {
    fn parse(input: syn::parse::ParseStream) -> syn::Result<Self> {
        if input.peek(syn::LitStr) {
            return Ok(RawArg::Literal(input.parse()?));
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

        let unhandled_err = |span: Span| {
            return Err(syn::Error::new(span, "Unhandled argument"))
                as syn::Result<()>;
        };

        for arg in
            syn::punctuated::Punctuated::<RawArg, Token![,]>::parse_terminated(
                input,
            )?
            .iter()
        {
            match arg {
                RawArg::Literal(name) => {
                    if args.name.is_none() {
                        args.name = Some(name.clone());
                    } else if args.name_false.is_none() {
                        args.name_false = Some(name.clone())
                    } else {
                        unhandled_err(name.span())?;
                    }
                }
                RawArg::AssignType(d, def) if d == "def" => {
                    args.def
                        .map_or(Ok(()), |_| return unhandled_err(d.span()))?;
                    args.def = Some(def.clone())
                }
                x => unhandled_err(x.span())?,
            }
        }

        Ok(args)
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

        let unhandled_err = |span: Span| {
            return Err(syn::Error::new(span, "Unhandled argument"))
                as syn::Result<()>;
        };

        for arg in
            syn::punctuated::Punctuated::<RawArg, Token![,]>::parse_terminated(
                input,
            )?
            .iter()
        {
            match arg {
                RawArg::Ident(x) if x == "addr" => {
                    args.addr = true;
                }
                RawArg::Literal(fmt) => {
                    args.custom_format
                        .map_or(Ok(()), |_| return unhandled_err(fmt.span()))?;
                    args.custom_format = Some(fmt.clone());
                }
                RawArg::AssignLit(d, offset) if d == "offset" => {
                    args.addr_offset
                        .map_or(Ok(()), |_| return unhandled_err(d.span()))?;
                    args.addr_offset = Some(offset.clone())
                }
                RawArg::AssignLit(d, prefix) if d == "prefix" => {
                    args.prefix
                        .map_or(Ok(()), |_| return unhandled_err(d.span()))?;
                    args.prefix = Some(prefix.clone())
                }
                x => unhandled_err(x.span())?,
            }
        }

        Ok(args)
    }
}

#[derive(Debug)]
pub struct DisplayArgs {
    pub format: LitStr,
}

impl syn::parse::Parse for DisplayArgs {
    fn parse(input: syn::parse::ParseStream) -> syn::Result<Self> {
        let mut format = None;

        for arg in
            syn::punctuated::Punctuated::<RawArg, Token![,]>::parse_terminated(
                input,
            )?
            .iter()
        {
            match arg {
                RawArg::AssignLit(f, fmt) if f == "format" => {
                    format = Some(fmt.clone());
                }
                x => {
                    return Err(syn::Error::new(x.span(), "Unhandled argument"))
                }
            }
        }
        if format == None {
            return Err(input.error("Cannot find format"));
        }

        Ok(DisplayArgs {
            format: format.unwrap(),
        })
    }
}

#[derive(Debug)]
pub enum ModifierType {
    BoolMod {
        name: LitStr,
        name_false: Option<LitStr>,
    },
    EnumMod {
        def: Option<Type>,
    },
}

pub struct Modifier {
    pub ident: Ident,
    pub array_len: usize,
    pub ty: ModifierType,
}

impl Modifier {
    fn parse_field(field: &Field) -> syn::Result<Option<Self>> {
        let Some(attr) = field
            .attrs
            .iter()
            .filter(|x| x.path().is_ident("modifier"))
            .next()
        else {
            return Ok(None);
        };

        let is_type_bool = |ty: &Type| {
            matches!(ty, Type::Path(TypePath {
            qself: None,
            path
        }) if path.is_ident("bool"))
        };

        let (array_len, is_bool) = match &field.ty {
            Type::Array(TypeArray {
                elem,
                len:
                    Expr::Lit(ExprLit {
                        lit: Lit::Int(len), ..
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
                }),
            }
        } else {
            ModifierType::EnumMod {
                def: args.def.clone(),
            }
        };
        Ok(Some(Modifier {
            ident,
            array_len,
            ty: mod_ty,
        }))
    }

    pub fn parse_all(data: &DataStruct) -> syn::Result<Vec<Self>> {
        data.fields
            .iter()
            .filter_map(|x| Self::parse_field(x).transpose())
            .collect()
    }
}

impl quote::ToTokens for Modifier {
    fn to_tokens(&self, tokens: &mut TokenStream2) {
        let Modifier {
            ident,
            array_len,
            ty,
        } = self;

        let generate_no_arr = |ident| match ty {
            ModifierType::BoolMod {
                name,
                name_false: None,
            } => {
                quote! {
                    if #ident {
                        write!(f, #name)?;
                    }
                }
            }
            ModifierType::BoolMod {
                name,
                name_false: Some(name_false),
            } => {
                quote! {
                    write!(f, "{}", if #ident { #name } else { #name_false })?;
                }
            }
            ModifierType::EnumMod { def: None } => {
                quote! {
                    write!(f, "{}", #ident)?;
                }
            }
            ModifierType::EnumMod { def: Some(def) } => {
                quote! {
                    if #ident != #def {
                        write!(f, "{}", #ident)?;
                    }
                }
            }
        };
        let t = match array_len {
            0 => generate_no_arr(quote! { self.#ident }),
            n => (0..*n)
                .map(|i| generate_no_arr(quote! { self.#ident[#i]}))
                .collect(),
        };
        t.to_tokens(tokens);
    }
}

#[derive(Debug)]
pub enum OpSourceFormat {
    Plain,
    Addr { offset: Option<LitStr> },
    Custom(LitStr),
}

/// Tracks sources of instr as Src and Target
/// Sources can also be arrays of static size
#[derive(Debug)]
pub struct OpSource {
    pub ident: Ident,
    pub array_len: usize,
    pub prefix: String,
    pub format: OpSourceFormat,
}

impl OpSource {
    fn parse_field(field: &Field) -> syn::Result<Option<Self>> {
        let is_type_src = |x: &Type| match x {
            Type::Path(TypePath { qself: None, path })
                if path.is_ident("Src") || path.is_ident("Label") =>
            {
                true
            }
            _ => false,
        };
        let array_len: usize = match &field.ty {
            Type::Array(TypeArray {
                elem,
                len:
                    Expr::Lit(ExprLit {
                        lit: Lit::Int(len), ..
                    }),
                ..
            }) if is_type_src(&elem) => len.base10_parse()?,
            x if is_type_src(&x) => 0,
            _ => return Ok(None),
        };
        let attr = field
            .attrs
            .iter()
            .filter(|x| x.path().is_ident("op_format"))
            .next();
        let args = match attr {
            Some(x) => x.parse_args()?,
            None => OpSourceFormatArgs::default(),
        };

        let prefix =
            args.prefix.map(|x| x.value()).unwrap_or_else(|| "".into());
        let format = if args.addr {
            OpSourceFormat::Addr {
                offset: args.addr_offset,
            }
        } else if let Some(fmt) = args.custom_format {
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

    pub fn parse_all(data: &DataStruct) -> syn::Result<Vec<Self>> {
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
                OpSourceFormat::Plain | OpSourceFormat::Custom(_) => {
                    quote!( #ident )
                }
                OpSourceFormat::Addr { offset: None } => {
                    quote! { FmtAddr { src: &#ident, off: 0 } }
                }
                OpSourceFormat::Addr { offset: Some(off) } => {
                    let off = Ident::new(&off.value(), off.span());
                    quote! { FmtAddr { src: &#ident, off: self.#off}}
                }
            };
            let fstr = match &self.format {
                OpSourceFormat::Custom(fmt) => {
                    format!(" {}{}", self.prefix, fmt.value())
                }
                _ => format!(" {}{{}}", self.prefix),
            };
            quote! {
                write!(f, #fstr, #arg)?;
            }
        };

        let t = match self.array_len {
            0 => generate_no_arr(quote! { self.#ident }),
            n => (0..n)
                .map(|i| generate_no_arr(quote! { self.#ident[#i] }))
                .collect(),
        };

        t.to_tokens(tokens);
    }
}
