#!/usr/bin/env python3
#
# Copyright © 2023 Igalia, S.L.
#
# Permission is hereby granted, free of charge, to any person obtaining a
# copy of this software and associated documentation files (the "Software"),
# to deal in the Software without restriction, including without limitation
# the rights to use, copy, modify, merge, publish, distribute, sublicense,
# and/or sell copies of the Software, and to permit persons to whom the
# Software is furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice (including the next
# paragraph) shall be included in all copies or substantial portions of the
# Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
# FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
# IN THE SOFTWARE.

import argparse
import os
import subprocess

# You must update the paths in docs/gitlab-ci.yml's pages job when changing this.
INPUT_PATHS = [
    'src/freedreno/isa/ir3.xml',
]

def run_docs_py(output_path, input_path):

    if not os.path.exists(output_path):
        os.makedirs(output_path)

    for path in input_path:
        print(path)
        basename = os.path.basename(path)
        name_without_extension, _ = os.path.splitext(basename)
        basename = f"{name_without_extension}.rst"

        subprocess.run(['../src/compiler/isaspec/docs.py', path, f"{output_path}/{basename}"])

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--out-dir',
                        help='Output RST directory.',
                        required=True)
    args = parser.parse_args()

    this_dir = os.path.dirname(os.path.abspath(__file__))
    mesa_dir = os.path.join(this_dir, '..')
    def fixpath(p):
        if os.path.isabs(p):
            return p
        return os.path.join(mesa_dir, p)

    input_paths = [ fixpath(p) for p in INPUT_PATHS ]

    run_docs_py(args.out_dir, input_paths)
