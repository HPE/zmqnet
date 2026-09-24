#!/bin/bash

if [ ! -d "sphinx-venv" ]; then
	echo "Creating Sphinx virtual environment..."
	python3 -m venv sphinx-venv
fi

source sphinx-venv/bin/activate

python3 -m pip install Sphinx myst_parser sphinx_rtd_theme

if ! sphinx-build --version; then
    echo "Sphinx install failed!"
    exit 1
fi

echo "Sphinx installed"

sphinx-build "$@"

deactivate
