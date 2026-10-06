# Shared fpdoom revision and compiler environment for the menu and game builds.
ensure_fpdoom() {
    local ref=04f19d6d54430693d00029970c51cd988c083b05
    if [ -n "${B310E_TOOLCHAIN:-}" ]; then PATH="$B310E_TOOLCHAIN:$PATH"; fi
    if [ -n "${B310E_HOST_CC:-}" ]; then PATH="$B310E_HOST_CC:$PATH"; fi
    export PATH
    command -v arm-none-eabi-gcc >/dev/null || { echo 'arm-none-eabi-gcc missing' >&2; return 1; }
    if [ ! -d "$Fpdoom/.git" ]; then
        mkdir -p "$(dirname "$Fpdoom")"
        git clone --no-checkout https://github.com/ilyakurdyukov/fpdoom "$Fpdoom" || return 1
        git -c safe.directory="$Fpdoom" -C "$Fpdoom" checkout --detach "$ref" || return 1
    fi
    [ "$(git -c safe.directory="$Fpdoom" -C "$Fpdoom" rev-parse HEAD)" = "$ref" ] || {
        echo "fpdoom must be at $ref; existing files were not reset" >&2; return 1;
    }
}
