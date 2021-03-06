# ----------------------------------------------------------------------------
# -                        Open3D: www.open3d.org                            -
# ----------------------------------------------------------------------------
# The MIT License (MIT)
#
# Copyright (c) 2018 www.open3d.org
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
# FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
# IN THE SOFTWARE.
# ----------------------------------------------------------------------------

# Sphinx makefile with api docs generation
# (1) The user call `make *` (e.g. `make html`) gets forwarded to make.py
# (2) make.py generate Python api docs, one ".rst" file per class / function
# (3) make.py calls the actual `sphinx-build`

from __future__ import print_function
import argparse
import subprocess
import sys
import importlib
import os
from inspect import getmembers, isbuiltin, isclass, ismodule
import shutil
import warnings
import weakref
from tempfile import mkdtemp
import re
from pathlib import Path
import nbformat
import nbconvert


def _create_or_clear_dir(dir_path):
    if os.path.exists(dir_path):
        shutil.rmtree(dir_path)
        print("Removed directory %s" % dir_path)
    os.makedirs(dir_path)
    print("Created directory %s" % dir_path)


class PyAPIDocsBuilder:
    """
    Generate Python API *.rst files, per (sub) module, per class, per function.
    The file name is the full module name.

    E.g. If output_dir == "python_api", the following files are generated:
    python_api/open3d.camera.rst
    python_api/open3d.camera.PinholeCameraIntrinsic.rst
    ...
    """

    def __init__(self, output_dir, module_names):
        self.output_dir = output_dir
        self.module_names = module_names
        print("Generating *.rst Python API docs in directory: %s" %
              self.output_dir)

    def generate_rst(self):
        _create_or_clear_dir(self.output_dir)

        for module_name in self.module_names:
            module = self._get_open3d_module(module_name)
            PyAPIDocsBuilder._generate_sub_module_class_function_docs(
                module_name, module, self.output_dir)

    @staticmethod
    def _get_open3d_module(full_module_name):
        """Returns the module object for the given module path"""
        import open3d  # make sure the root module is loaded

        try:
            # try to import directly. This will work for pure python submodules
            module = importlib.import_module(full_module_name)
            return module
        except ImportError:
            # traverse the module hierarchy of the root module.
            # This code path is necessary for modules for which we manually
            # define a specific module path (e.g. the modules defined with
            # pybind).
            current_module = open3d
            for sub_module_name in full_module_name.split(".")[1:]:
                current_module = getattr(current_module, sub_module_name)
            return current_module

    @staticmethod
    def _generate_function_doc(sub_module_full_name, function_name,
                               output_path):
        # print("Generating docs: %s" % (output_path,))
        out_string = ""
        out_string += "%s.%s" % (sub_module_full_name, function_name)
        out_string += "\n" + "-" * len(out_string)
        out_string += "\n\n" + ".. currentmodule:: %s" % sub_module_full_name
        out_string += "\n\n" + ".. autofunction:: %s" % function_name
        out_string += "\n"

        with open(output_path, "w") as f:
            f.write(out_string)

    @staticmethod
    def _generate_class_doc(sub_module_full_name, class_name, output_path):
        # print("Generating docs: %s" % (output_path,))
        out_string = ""
        out_string += "%s.%s" % (sub_module_full_name, class_name)
        out_string += "\n" + "-" * len(out_string)
        out_string += "\n\n" + ".. currentmodule:: %s" % sub_module_full_name
        out_string += "\n\n" + ".. autoclass:: %s" % class_name
        out_string += "\n    :members:"
        out_string += "\n    :undoc-members:"
        out_string += "\n    :inherited-members:"
        out_string += "\n"

        with open(output_path, "w") as f:
            f.write(out_string)

    @staticmethod
    def _generate_sub_module_doc(sub_module_full_name, class_names,
                                 function_names, sub_module_doc_path):
        # print("Generating docs: %s" % (sub_module_doc_path,))
        class_names = sorted(class_names)
        function_names = sorted(function_names)
        out_string = ""
        out_string += sub_module_full_name
        out_string += "\n" + "-" * len(out_string)
        out_string += "\n\n" + ".. currentmodule:: %s" % sub_module_full_name

        if len(class_names) > 0:
            out_string += "\n\n**Classes**"
            out_string += "\n\n.. autosummary::"
            out_string += "\n"
            for class_name in class_names:
                out_string += "\n    " + "%s" % (class_name,)
            out_string += "\n"

        if len(function_names) > 0:
            out_string += "\n\n**Functions**"
            out_string += "\n\n.. autosummary::"
            out_string += "\n"
            for function_name in function_names:
                out_string += "\n    " + "%s" % (function_name,)
            out_string += "\n"

        obj_names = class_names + function_names
        if len(obj_names) > 0:
            out_string += "\n\n.. toctree::"
            out_string += "\n    :hidden:"
            out_string += "\n"
            for obj_name in obj_names:
                out_string += "\n    %s <%s.%s>" % (
                    obj_name,
                    sub_module_full_name,
                    obj_name,
                )
            out_string += "\n"

        with open(sub_module_doc_path, "w") as f:
            f.write(out_string)

    @staticmethod
    def _generate_sub_module_class_function_docs(sub_module_full_name,
                                                 sub_module, output_dir):
        print("Generating docs for submodule: %s" % sub_module_full_name)

        # Class docs
        class_names = [
            obj[0] for obj in getmembers(sub_module) if isclass(obj[1])
        ]
        for class_name in class_names:
            file_name = "%s.%s.rst" % (sub_module_full_name, class_name)
            output_path = os.path.join(output_dir, file_name)
            PyAPIDocsBuilder._generate_class_doc(sub_module_full_name,
                                                 class_name, output_path)

        # Function docs
        function_names = [
            obj[0] for obj in getmembers(sub_module) if isbuiltin(obj[1])
        ]
        for function_name in function_names:
            file_name = "%s.%s.rst" % (sub_module_full_name, function_name)
            output_path = os.path.join(output_dir, file_name)
            PyAPIDocsBuilder._generate_function_doc(sub_module_full_name,
                                                    function_name, output_path)

        # Submodule docs
        sub_module_doc_path = os.path.join(output_dir,
                                           sub_module_full_name + ".rst")
        PyAPIDocsBuilder._generate_sub_module_doc(
            sub_module_full_name,
            class_names,
            function_names,
            sub_module_doc_path,
        )


class SphinxDocsBuilder:
    """
    SphinxDocsBuilder calls Python api docs generation and then calls
    sphinx-build:

    (1) The user call `make *` (e.g. `make html`) gets forwarded to make.py
    (2) Calls PyAPIDocsBuilder to generate Python api docs rst files
    (3) Calls `sphinx-build` with the user argument
    """

    def __init__(self, html_output_dir, is_release):
        # Get the modules for which we want to build the documentation.
        # We use the modules listed in the index.rst file here.
        self.documented_modules = self._get_module_names_from_index_rst()

        # self.documented_modules = "open3d.pybind"  # Points to the open3d.so
        # self.c_module_relative = "open3d"  # The relative module reference to open3d.so
        self.python_api_output_dir = "python_api"
        self.html_output_dir = html_output_dir
        self.is_release = is_release

    @staticmethod
    def _get_module_names_from_index_rst():
        """Reads the modules of the python api from the index.rst"""
        module_names = []
        with open("index.rst", "r") as f:
            for line in f:
                m = re.match("^\s*python_api/(.*)\s*$", line)
                if m:
                    module_names.append(m.group(1))
        return module_names

    def run(self):
        self._gen_python_api_docs()
        self._run_sphinx()

    def _gen_python_api_docs(self):
        """
        Generate Python docs.
        Each module, class and function gets one .rst file.
        """
        # self.python_api_output_dir cannot be a temp dir, since other
        # "*.rst" files reference it
        pd = PyAPIDocsBuilder(self.python_api_output_dir,
                              self.documented_modules)
        pd.generate_rst()

    def _run_sphinx(self):
        """
        Call Sphinx command with hard-coded "html" target
        """
        build_dir = os.path.join(self.html_output_dir, "html")

        if self.is_release:
            version_list = [
                line.rstrip("\n").split(" ")[1]
                for line in open("../src/Open3D/version.txt")
            ]
            release_version = ".".join(version_list[:3])
            print("Building docs for release:", release_version)

            cmd = [
                "sphinx-build",
                "-b",
                "html",
                "-D",
                "version=" + release_version,
                "-D",
                "release=" + release_version,
                ".",
                build_dir,
            ]
        else:
            cmd = [
                "sphinx-build",
                "-b",
                "html",
                ".",
                build_dir,
            ]
        print('Calling: "%s"' % " ".join(cmd))
        subprocess.check_call(cmd, stdout=sys.stdout, stderr=sys.stderr)


class DoxygenDocsBuilder:

    def __init__(self, html_output_dir):
        self.html_output_dir = html_output_dir

    def run(self):
        doxygen_temp_dir = "doxygen"
        _create_or_clear_dir(doxygen_temp_dir)

        cmd = ["doxygen", "Doxyfile"]
        print('Calling: "%s"' % " ".join(cmd))
        subprocess.check_call(cmd, stdout=sys.stdout, stderr=sys.stderr)
        shutil.copytree(
            os.path.join("doxygen", "html"),
            os.path.join(self.html_output_dir, "html", "cpp_api"),
        )

        if os.path.exists(doxygen_temp_dir):
            shutil.rmtree(doxygen_temp_dir)


class JupyterDocsBuilder:

    def __init__(self, current_file_dir, clean_notebooks, execute_notebooks):
        """
        execute_notebooks is one of {"auto", "always"}
        """
        if execute_notebooks not in {"auto", "always"}:
            raise ValueError(f"Invalid execute option: {execute_notebooks}.")
        self.clean_notebooks = clean_notebooks
        self.execute_notebooks = execute_notebooks
        self.current_file_dir = current_file_dir
        print("Notebook execution mode: {}".format(self.execute_notebooks))

    def run(self):
        # Setting os.environ["CI"] will disable interactive (blocking) mode in
        # Jupyter notebooks
        os.environ["CI"] = "true"

        # Copy TestData directory to the tutorial folder
        test_data_in_dir = (Path(self.current_file_dir).parent / "examples" /
                            "TestData")
        test_data_out_dir = Path(self.current_file_dir) / "TestData"
        if test_data_out_dir.exists():
            shutil.rmtree(test_data_out_dir)
        shutil.copytree(test_data_in_dir, test_data_out_dir)

        # Copy and execute notebooks in the tutorial folder
        nb_paths = []
        example_dirs = ["Basic", "Advanced"]
        for example_dir in example_dirs:
            in_dir = (Path(self.current_file_dir).parent / "examples" /
                      "Python" / example_dir)
            out_dir = Path(self.current_file_dir) / "tutorial" / example_dir
            shutil.copy(
                in_dir.parent / "open3d_tutorial.py",
                out_dir.parent / "open3d_tutorial.py",
            )

            if self.clean_notebooks:
                for nb_out_path in out_dir.glob("*.ipynb"):
                    print("Delete: {}".format(nb_out_path))
                    nb_out_path.unlink()

            for nb_in_path in in_dir.glob("*.ipynb"):
                nb_out_path = out_dir / nb_in_path.name
                if not nb_out_path.is_file():
                    print("Copy: {}\n   -> {}".format(nb_in_path, nb_out_path))
                    shutil.copy(nb_in_path, nb_out_path)
                else:
                    print("Copy skipped: {}.format(nb_out_path)")
                nb_paths.append(nb_out_path)

        # Execute Jupyter notebooks
        for nb_path in nb_paths:
            print("[Processing notebook {}]".format(nb_path.name))
            with open(nb_path) as f:
                nb = nbformat.read(f, as_version=4)

            # https://github.com/spatialaudio/nbsphinx/blob/master/src/nbsphinx.py
            has_code = any(c.source for c in nb.cells if c.cell_type == "code")
            has_output = any(
                c.get("outputs") or c.get("execution_count")
                for c in nb.cells
                if c.cell_type == "code")
            execute = (self.execute_notebooks == "auto" and has_code and
                       not has_output) or self.execute_notebooks == "always"
            print("has_code: {}, has_output: {}, execute: {}".format(
                has_code, has_output, execute))

            if execute:
                ep = nbconvert.preprocessors.ExecutePreprocessor(timeout=6000)
                try:
                    ep.preprocess(nb, {"metadata": {"path": nb_path.parent}})
                except nbconvert.preprocessors.execute.CellExecutionError:
                    print(
                        "Execution of {} failed, this will cause Travis to fail."
                        .format(nb_path.name))
                    if "TRAVIS" in os.environ:
                        raise

                with open(nb_path, "w", encoding="utf-8") as f:
                    nbformat.write(nb, f)


if __name__ == "__main__":
    """
    # Clean existing notebooks in docs/tutorial, execute notebooks if the
    # notebook does not have outputs, and build docs for Python and C++.
    $ python make_docs.py --clean_notebooks --execute_notebooks=auto --sphinx --doxygen

    # Build docs for Python (--sphinx) and C++ (--doxygen).
    $ python make_docs.py --execute_notebooks=auto --sphinx --doxygen

    # Build docs for release (version number will be used instead of git hash).
    $ python make_docs.py --is_release --sphinx --doxygen
    """
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--clean_notebooks",
        dest="clean_notebooks",
        action="store_true",
        default=False,
        help=("Whether to clean existing notebooks in docs/tutorial. "
              "Notebooks are copied from examples/Python to docs/tutorial."),
    )
    parser.add_argument(
        "--execute_notebooks",
        dest="execute_notebooks",
        default="auto",
        help="Jupyter notebook execution mode, one of {auto, always}.",
    )
    parser.add_argument(
        "--sphinx",
        dest="build_sphinx",
        action="store_true",
        default=False,
        help="Build Sphinx for main docs and Python API docs.",
    )
    parser.add_argument(
        "--doxygen",
        dest="build_doxygen",
        action="store_true",
        default=False,
        help="Build Doxygen for C++ API docs.",
    )
    parser.add_argument(
        "--is_release",
        dest="is_release",
        action="store_true",
        default=False,
        help="Show Open3D version number rather than git hash.",
    )
    args = parser.parse_args()

    pwd = os.path.dirname(os.path.realpath(__file__))

    # Clear output dir if new docs are to be built
    html_output_dir = os.path.join(pwd, "_out")
    _create_or_clear_dir(html_output_dir)

    # Clear C++ build directory
    cpp_build_dir = os.path.join(pwd, "_static", "C++", "build")
    if os.path.exists(cpp_build_dir):
        shutil.rmtree(cpp_build_dir)
        print("Removed directory %s" % cpp_build_dir)

    # Sphinx is hard-coded to build with the "html" option
    # To customize build, run sphinx-build manually
    if args.build_sphinx:
        print("Sphinx build enabled")
        print("Building Jupyter docs")
        jdb = JupyterDocsBuilder(pwd, args.clean_notebooks,
                                 args.execute_notebooks)
        jdb.run()
        print("Building Sphinx docs")
        sdb = SphinxDocsBuilder(html_output_dir, args.is_release)
        sdb.run()
    else:
        print("Sphinx build disabled, use --sphinx to enable")

    # Doxygen is hard-coded to build with default option
    # To customize build, customize Doxyfile or run doxygen manually
    if args.build_doxygen:
        print("Doxygen build enabled")
        ddb = DoxygenDocsBuilder(html_output_dir)
        ddb.run()
    else:
        print("Doxygen build disabled, use --doxygen to enable")
