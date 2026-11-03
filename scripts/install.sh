#!/bin/bash
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
"$ROOT/scripts/build.sh"
LV2_DIR="${LV2_DIR:-/usr/local/lib/lv2}"
UI_DIR="${ZYNTHIAN_UI_DIR:-/zynthian/zynthian-ui}"
MY_DATA="${ZYNTHIAN_MY_DATA_DIR:-/zynthian/zynthian-my-data}"
mkdir -p "$LV2_DIR"
ln -sfn "$ROOT/build/granular_sampler.lv2" "$LV2_DIR/granular_sampler.lv2"
if [ -f "$ROOT/ui/zynthian_widget_granular_sampler.py" ]; then
    mkdir -p "$UI_DIR/zyngui/widgets_user"
    ln -sfn "$ROOT/ui/zynthian_widget_granular_sampler.py" "$UI_DIR/zyngui/widgets_user/zynthian_widget_granular_sampler.py"
fi
# Samples: Zynthian's file selector shows files/Samples as "User Samples". Our factory samples
# are a link inside it; the user's own samples go next to it, straight into files/Samples.
python3 "$ROOT/scripts/gen_samples.py"
mkdir -p "$MY_DATA/files/Samples"
ln -sfn "$ROOT/samples" "$MY_DATA/files/Samples/granular-sampler"
echo "Samples: $MY_DATA/files/Samples (factory ones in granular-sampler/)"
# Zynthian keeps a cache of each plugin's preset list: rebuild it for this plugin only, with Zynthian's own tool.
ENVARS="${ZYNTHIAN_ENVARS:-/zynthian/config/zynthian_envars.sh}"
if [ -f "$UI_DIR/zyngine/zynthian_lv2.py" ] && [ -f "$ENVARS" ]; then
    ( set +e; source "$ENVARS"; cd "$UI_DIR" && python3 zyngine/zynthian_lv2.py presets urn:granularsampler:sampler ) \
        && echo "Preset list updated." || echo "Warning: preset list not updated (run install.sh again on Zynthian)."
else
    echo "Note: $ENVARS or zynthian_lv2.py not found, preset list cache not updated."
fi
echo "Installed. Restart Zynthian: systemctl restart zynthian"
