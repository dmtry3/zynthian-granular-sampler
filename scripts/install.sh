#!/bin/bash
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
"$ROOT/scripts/build.sh"
LV2_DIR="${LV2_DIR:-/usr/local/lib/lv2}"
UI_DIR="${ZYNTHIAN_UI_DIR:-/zynthian/zynthian-ui}"
mkdir -p "$LV2_DIR"
ln -sfn "$ROOT/build/granular_sampler.lv2" "$LV2_DIR/granular_sampler.lv2"
if [ -f "$ROOT/ui/zynthian_widget_granular_sampler.py" ]; then
    mkdir -p "$UI_DIR/zyngui/widgets_user"
    ln -sfn "$ROOT/ui/zynthian_widget_granular_sampler.py" "$UI_DIR/zyngui/widgets_user/zynthian_widget_granular_sampler.py"
fi
echo "Installed. Restart Zynthian: systemctl restart zynthian"
