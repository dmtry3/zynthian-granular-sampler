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
echo "Installed. Restart Zynthian: systemctl restart zynthian"
