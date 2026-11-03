#!/bin/bash
LV2_DIR="${LV2_DIR:-/usr/local/lib/lv2}"
UI_DIR="${ZYNTHIAN_UI_DIR:-/zynthian/zynthian-ui}"
MY_DATA="${ZYNTHIAN_MY_DATA_DIR:-/zynthian/zynthian-my-data}"
rm -rf "$LV2_DIR/granular_sampler.lv2"
rm -f  "$UI_DIR/zyngui/widgets_user/zynthian_widget_granular_sampler.py"
# only our link; the user's samples in files/Samples stay
[ -L "$MY_DATA/files/Samples/granular-sampler" ] && rm -f "$MY_DATA/files/Samples/granular-sampler"
echo "Removed links."
