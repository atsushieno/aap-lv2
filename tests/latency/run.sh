#!/bin/sh
# Builds the actual bridge and lilv natively, and runs them against real LV2 plugin binaries.
# AAP_CORE_DIR selects aap-core (default: the external/aap-core submodule).
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo=$(CDPATH= cd -- "$here/../.." && pwd)
src=$repo/androidaudioplugin-lv2/src/main/cpp/src
ext=$repo/external
aapcore=${AAP_CORE_DIR:-$ext/aap-core}
build=$(mktemp -d "${TMPDIR:-/tmp}/aap-lv2-latency.XXXXXX")
trap 'rm -rf "$build"' EXIT
includes="-I$aapcore/include -I$aapcore/external/cmidi2 -I$src -I$ext -I$ext/serd/src -I$ext/sord/src -I$ext/lilv/src -I$ext/serd/include -I$ext/sord/include -I$ext/lv2 -I$ext/sratom/include -I$ext/lilv/include"
defines="-DHAVE_MLOCK=1 -DZIX_STATIC -DZIX_INTERNAL"
# The bundled serd configuration assumes Linux.
cat > "$build/compat.h" <<'HEADER'
#ifdef __APPLE__
#define POSIX_FADV_SEQUENTIAL 0
#define posix_fadvise(fd, offset, length, advice) 0
#endif
HEADER
objects=
for c in "$src/zix/ring.c" "$src/std_workaround.c" "$src/abstract_io.c" \
    "$ext"/lilv/src/plugin.c "$ext"/lilv/src/pluginclass.c "$ext"/lilv/src/filesystem.c "$ext"/lilv/src/ui.c \
    "$ext"/lilv/src/util.c "$ext"/lilv/src/state.c "$ext"/lilv/src/lib.c "$ext"/lilv/src/zix/tree.c \
    "$ext"/lilv/src/instance.c "$ext"/lilv/src/node.c "$ext"/lilv/src/world.c "$ext"/lilv/src/query.c \
    "$ext"/lilv/src/scalepoint.c "$ext"/lilv/src/collections.c "$ext"/lilv/src/port.c \
    "$ext"/serd/src/base64.c "$ext"/serd/src/writer.c "$ext"/serd/src/system.c "$ext"/serd/src/reader.c \
    "$ext"/serd/src/n3.c "$ext"/serd/src/byte_source.c "$ext"/serd/src/uri.c "$ext"/serd/src/env.c \
    "$ext"/serd/src/node.c "$ext"/serd/src/string.c "$ext"/sord/src/syntax.c "$ext"/sord/src/sord.c \
    "$ext"/sratom/src/sratom.c; do
    object=$build/$(echo "$c" | tr '/' '_').o
    "${CC:-clang}" -std=gnu11 -w -g -include "$build/compat.h" $defines $includes -c "$c" -o "$object"
    objects="$objects $object"
done
mkdir "$build/bundles"
cp -R "$here/latency-probe.lv2" "$here/../offline-rendering/offline-probe.lv2" "$build/bundles/"
"${CC:-clang}" -std=c11 -dynamiclib -I"$ext/lv2" "$here/latency-probe.c" -o "$build/bundles/latency-probe.lv2/latency-probe.dylib"
"${CC:-clang}" -std=c11 -dynamiclib -I"$ext/lv2" "$here/../offline-rendering/offline-probe.c" -o "$build/bundles/offline-probe.lv2/offline-probe.dylib"
"${CXX:-clang++}" -std=c++20 -w -g -fsanitize=undefined -fno-sanitize=vptr $defines $includes \
    "$here/latency-test.cpp" "$src/android-audio-plugin-lv2-bridge.cpp" "$src/aap-lv2-extensions.cpp" "$src/symap.cpp" $objects \
    -o "$build/latency-test"
"$build/latency-test" "$build/bundles"
