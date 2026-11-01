#!/bin/sh

set -eux

srcdir=$(realpath "${1}")
blddir=$(realpath "${2}")

. "${srcdir}"/pgo/options

pgo_data=$(mktemp)
shader_data=$(mktemp)
shader_conf=$(mktemp)
trap "rm -f '${pgo_data}' '${shader_data}' '${shader_conf}'" EXIT INT HUP TERM

rm -f "${blddir}"/pgo-ok

# To ensure profiling data is generated in the build directory
cd "${blddir}"

"${blddir}"/utils/xtgettcap
"${blddir}"/ankleclient --version
"${blddir}"/ankle \
           --config=/dev/null \
           --override tweak.grapheme-shaping=no \
           --term=xterm \
        sh -c "
          set -eux

         '${srcdir}/scripts/generate-alt-random-writes.py' \
            ${script_options} \"${pgo_data}\"

         cat \"${pgo_data}\"
         "

# Custom shaders: frame upload, the shader chain (a cursor trail, a
# cursor effect and a terminal shader) and animation frames. Only when
# built with -Dshaders=enabled.
if "${blddir}"/ankle --version | grep -q -- '+shaders'; then
    cat > "${shader_conf}" <<EOF
[main]
custom-shader="${srcdir}/shaders/cursor-fx/neon-trail.glsl" "${srcdir}/shaders/cursor-fx/neon-cursor.glsl" "${srcdir}/shaders/terminal/retro-crt.glsl"
custom-shader-animation=always
EOF

    "${blddir}"/ankle \
               --config="${shader_conf}" \
               --override tweak.grapheme-shaping=no \
               --term=xterm \
            sh -c "
              set -eux

             '${srcdir}/scripts/generate-alt-random-writes.py' \
                --scroll --colors-regular --colors-256 --colors-rgb \
                --attr-bold --attr-italic \"${shader_data}\"

             cat \"${shader_data}\"
             "
fi

touch "${blddir}"/pgo-ok
