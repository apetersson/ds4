#!/bin/sh
set -eu

python_bin=${VISION_PYTHON:-python3.12}
venv_dir=${VISION_VENV:-.venv-vision}

if ! command -v "$python_bin" >/dev/null 2>&1; then
    echo "error: $python_bin is required for the qualified Vision-Exp runtime" >&2
    echo "       install Python 3.12, or set VISION_PYTHON=/path/to/python3.12" >&2
    exit 1
fi

python_version=$(
    "$python_bin" -c 'import sys; print(f"{sys.version_info.major}.{sys.version_info.minor}")'
)
if [ "$python_version" != "3.12" ]; then
    echo "error: $python_bin is Python $python_version; Vision-Exp requires Python 3.12" >&2
    exit 1
fi

if [ ! -x "$venv_dir/bin/python" ]; then
    echo "Creating Vision-Exp environment in $venv_dir"
    "$python_bin" -m venv "$venv_dir"
fi

venv_version=$(
    "$venv_dir/bin/python" -c 'import sys; print(f"{sys.version_info.major}.{sys.version_info.minor}")'
)
if [ "$venv_version" != "3.12" ]; then
    echo "error: $venv_dir uses Python $venv_version; remove it and run this target again" >&2
    exit 1
fi

echo "Installing the qualified Vision-Exp Python dependencies"
"$venv_dir/bin/python" -m pip install 'torch==2.13.*' safetensors pillow numpy

"$venv_dir/bin/python" -c \
    'import numpy, PIL, safetensors, torch; print(f"Vision-Exp environment ready: Python 3.12, PyTorch {torch.__version__}")'

echo "Run DS4 with:"
echo "  --vision-python $venv_dir/bin/python --vision-encoder $PWD/misc/encode-deepseek4-vision.py"
