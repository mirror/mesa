# formatting.py
# Sphinx extension providing formatting for Gallium-specific data
# (c) Corbin Simpson 2010
# Public domain to the extent permitted; contact author for special licensing

import docutils.nodes
import sphinx.addnodes

from sphinx.util.nodes import split_explicit_title
from docutils import nodes, utils

def parse_opcode(env, sig, signode):
    opcode, desc = sig.split("-", 1)
    opcode = opcode.strip().upper()
    desc = " (%s)" % desc.strip()
    signode += sphinx.addnodes.desc_name(opcode, opcode)
    signode += sphinx.addnodes.desc_annotation(desc, desc)
    return opcode


def ext_role(name, rawtext, text, lineno, inliner, options={}, content=[]):
    text = utils.unescape(text)
    has_explicit_title, title, ext = split_explicit_title(text)

    parts = ext.split('_', 1)
    full_url = f'https://registry.khronos.org/OpenGL/extensions/{parts[0]}/{parts[0]}_{parts[1]}.txt'

    pnode = nodes.reference(title, title, internal=False, refuri=full_url)
    return [pnode], []

def setup(app):
    app.add_object_type("opcode", "opcode", "%s (TGSI opcode)",
        parse_opcode)
    app.add_role('gl-ext', ext_role)
