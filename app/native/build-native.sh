#!/usr/bin/env bash
set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
app_root=$(cd -- "$here/.." && pwd)
opengl=${PS5_OPENGL_SOURCE:-/mnt/c/Users/643/Documents/Codex/2026-10-11/ok-x20/work/ps5-opengl-source}
template=${PS5_NATIVE_APP_TEMPLATE:-$opengl/build/native-app/PPSA99005}
sdk=${PS5_PAYLOAD_SDK:-$template/.deps/native/ps5-payload-sdk}
prefix=${PS5_OPENGL_PREFIX:-$opengl/../ps5-opengl-sdk-1.0.1/sdk}
imgui=$opengl/third_party/imgui
build=$app_root/.native-build/PPSA99760
dist=$app_root/dist/PPSA99760
compiler=${PS5_CLANG:-clang-18}

test -f "$template/Makefile"
test -s "$prefix/lib/libPS5OpenGLCore33.a"
rm -rf -- "$build"
mkdir -p "$build"/{src,include,vendor,build/native-objects}
cp "$template/Makefile" "$build/Makefile"
for directory in assets runtime sce_sys tooling tools; do
    mkdir -p "$build/$directory"
    cp -a "$template/$directory/." "$build/$directory/"
done
cp -a "$template/.deps" "$build/.deps"
# Game processes cannot load the WebKit-only POSIX shim. Its broad symbol set
# otherwise wins resolution from the wildcard SDK stub list and SysCore aborts
# before NOTE_EXEC. Native libc/libkernel provide the required game ABI.
rm -f "$build/.deps/native/ps5-payload-sdk/target/lib/libScePosixForWebKit.so"
cp "$opengl/native-app/runtime_shims.c" "$build/src/runtime_shims.c"
cp "$opengl/native-app/app_heap.c" "$build/src/app_heap.c"
cp "$opengl/native-app/ps5-pie.ld" "$build/tooling/native/ps5-pie.ld"
cp "$opengl/native-app/app-symbols.map" "$build/tooling/native/app-symbols.map"
for headers in EGL GL KHR; do cp -a "$prefix/include/$headers" "$build/include/"; done

flags=(-std=c++11 -O2 -g -fPIC -ffunction-sections -fdata-sections -Wall -Wextra -Werror
       -I"$prefix/include" -I"$imgui" -I"$imgui/backends"
       -DGL_GLEXT_PROTOTYPES=1 -DIMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS \
       -DIMGUI_IMPL_OPENGL_LOADER_CUSTOM -include GL/gl.h)
cxx=(env PS5_PAYLOAD_SDK="$sdk" sh "$template/tooling/prospero-clang18")
"${cxx[@]}" "${flags[@]}" -c "$here/main.cpp" -o "$build/build/native-objects/main.o"
for source in imgui.cpp imgui_draw.cpp imgui_tables.cpp imgui_widgets.cpp; do
    "${cxx[@]}" "${flags[@]}" -c "$imgui/$source" -o "$build/build/native-objects/${source%.cpp}.o"
done
"${cxx[@]}" "${flags[@]}" -c "$imgui/backends/imgui_impl_opengl3.cpp" \
    -o "$build/build/native-objects/imgui_impl_opengl3.o"
ld.lld-18 -r -o "$build/build/native-objects/emc-native.o" "$build/build/native-objects/"*.o

compiler_runtime=$($compiler --print-resource-dir)/lib/linux/libclang_rt.builtins-x86_64.a
cat > "$build/vendor/libemc_native_group.a" <<EOF
SEARCH_DIR("$sdk/target/lib")
EXTERN(ps5_agc_gate2_run)
GROUP (
  "$build/build/native-objects/emc-native.o"
  "$prefix/lib/libPS5OpenGLCore33.a"
  "$sdk/target/lib/libSceLibcInternal.so"
  "$sdk/target/lib/libkernel.so"
  "$sdk/target/lib/libSceNet.so"
  "$sdk/target/lib/libScePad.so"
  "$sdk/target/lib/libSceUserService.so"
  "$sdk/target/lib/libunwind.a"
  "$sdk/target/lib/libc++abi.a"
  "$sdk/target/lib/libc++.a"
  "$compiler_runtime"
)
EOF
printf 'APP_INCLUDE_PATHS = include\nAPP_STATIC_ARCHIVES = vendor/libemc_native_group.a\n' > "$build/.env"

cp "$opengl/native-app/param.json" "$build/sce_sys/param.json"
python3 - "$build/sce_sys/param.json" <<'PY'
import json, sys
path = sys.argv[1]
data = json.load(open(path))
data.update({
    "titleId": "PPSA99760",
    "conceptId": "99760",
    "contentId": "UP9000-PPSA99760_00-PS5EMCDUMPER1360",
    # This is the launch profile verified on 13.60 hardware. Generic zero
    # attributes make SysCore reject this folder title before main().
    "attribute": 1644167168,
    "attribute3": 524352,
    "downloadDataSize": 2048,
})
data["localizedParameters"]["en-US"]["titleName"] = "PS5 EMC Dumper"
with open(path, "w") as output: json.dump(data, output, indent=2); output.write("\n")
PY
cp "$app_root/native/assets/icon0.png" "$build/sce_sys/icon0.png"
cp "$app_root/../loader/emc-log-reader/ps5_emc_log_reader_standalone_1360.elf" "$build/emc-reader.elf"
make -C "$build" --no-print-directory -j8 USE_CCACHE=0 app
rm -rf -- "$dist"
mkdir -p "$(dirname "$dist")"
cp -a "$build/dist/PPSA99760" "$dist"
cp "$build/emc-reader.elf" "$dist/emc-reader.elf"
rm -f "$app_root/dist/PPSA99760.zip"
(cd "$app_root/dist" && zip -qr "PPSA99760.zip" "PPSA99760")
sha256sum "$dist/eboot.bin" "$dist/emc-reader.elf"
