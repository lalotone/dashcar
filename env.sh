# Source this file to get idf.py in your shell:  . ./env.sh   (first time: tools/setup.sh)
# ESP-IDF 5.5 needs Python 3.9-3.13, and export.sh would otherwise pick the system python3 (may be
# too new, e.g. 3.14), so point it at the venv that tools/setup.sh created.
export IDF_PATH="${IDF_PATH:-$HOME/esp/esp-idf}"
if [ -z "${IDF_PYTHON_ENV_PATH:-}" ]; then
    for _env in "$HOME"/.espressif/python_env/idf5.5_py3.*_env; do
        [ -d "$_env" ] && export IDF_PYTHON_ENV_PATH="$_env"
    done
    unset _env
fi
. "$IDF_PATH/export.sh" > /dev/null
