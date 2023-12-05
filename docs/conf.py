# -*- coding: utf-8 -*-
#
# Copyright (c) 2019-2023, Arm Limited. All rights reserved.
#
# SPDX-License-Identifier: BSD-3-Clause
#
#
# Configuration file for the Sphinx documentation builder.
#
# See the options documentation at http://www.sphinx-doc.org/en/master/config


# -- Project information -----------------------------------------------------

project = "Trusted Firmware-A"
author = "Trusted Firmware-A contributors"
version = "2.8.13"
release = "2.8.13"

# -- General configuration ---------------------------------------------------

# Add any Sphinx extension module names here, as strings. They can be
# extensions coming with Sphinx (named 'sphinx.ext.*') or your custom
# ones.
extensions = [
    "myst_parser",
    "sphinx.ext.autosectionlabel",
    "sphinxcontrib.plantuml",
    "sphinxcontrib.rsvgconverter",
]

# Add any paths that contain templates here, relative to this directory.
templates_path = ["_templates"]

# The suffix(es) of source filenames.
source_suffix = [".md", ".rst"]

# The master toctree document.
master_doc = "index"

# The language for content autogenerated by Sphinx. Refer to documentation
# for a list of supported languages.
#
# This is also used if you do content translation via gettext catalogs.
# Usually you set "language" from the command line for these cases.
language = "en"

# List of patterns, relative to source directory, that match files and
# directories to ignore when looking for source files.
# This pattern also affects html_static_path and html_extra_path .
# Don't try to build the venv in case it's placed with the sources
exclude_patterns = [".env", "env", ".venv", "venv"]

# The name of the Pygments (syntax highlighting) style to use.
pygments_style = "sphinx"

# Load the contents of the global substitutions file into the 'rst_prolog'
# variable. This ensures that the substitutions are all inserted into each
# page.
with open("global_substitutions.txt", "r") as subs:
    rst_prolog = subs.read()

# Minimum version of sphinx required
needs_sphinx = "2.0"

# -- Options for HTML output -------------------------------------------------

# Don't show the "Built with Sphinx" footer
html_show_sphinx = False

# Don't show copyright info in the footer (we have this content in the page)
html_show_copyright = False

# The theme to use for HTML and HTML Help pages.  See the documentation for
# a list of builtin themes.
html_theme = "sphinx_rtd_theme"

# The logo to display in the sidebar
html_logo = "resources/TrustedFirmware-Logo_standard-white.png"

# Options for the "sphinx-rtd-theme" theme
html_theme_options = {
    "collapse_navigation": False,  # Can expand and collapse sidebar entries
    "prev_next_buttons_location": "both",  # Top and bottom of the page
    "style_external_links": True,  # Display an icon next to external links
}

# Path to _static directory
html_static_path = ["_static"]

# Path to css file relative to html_static_path
html_css_files = [
    "css/custom.css",
]

# -- Options for autosectionlabel --------------------------------------------

# Only generate automatic section labels for document titles
autosectionlabel_maxdepth = 1

# -- Options for plantuml ----------------------------------------------------

plantuml_output_format = "svg_img"

# -- Options for latexmk  ----------------------------------------------------

latex_engine = "xelatex"
latex_elements = {
    "maxlistdepth": "10",
    "pointsize": "11pt",
}
