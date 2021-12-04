SKIPUNZIP=1

# Extract verify.sh
ui_print "- Extracting verify.sh"
unzip -o "$ZIPFILE" 'verify.sh' -d "$TMPDIR" >&2
if [ ! -f "$TMPDIR/verify.sh" ]; then
  ui_print    "*********************************************************"
  ui_print    "! Unable to extract verify.sh!"
  ui_print    "! This zip may be corrupted, please try downloading again"
  abort "*********************************************************"
fi
. $TMPDIR/verify.sh

# Extract riru.sh

# Variables provided by riru.sh:
#
# RIRU_API: API version of installed Riru, 0 if not installed
# RIRU_MIN_COMPATIBLE_API: minimal supported API version by installed Riru, 0 if not installed or version < v23.2
# RIRU_VERSION_CODE: version code of installed Riru, 0 if not installed or version < v23.2
# RIRU_VERSION_NAME: version name of installed Riru, "" if not installed or version < v23.2

extract "$ZIPFILE" 'riru.sh' "$TMPDIR"
. $TMPDIR/riru.sh

# Functions from riru.sh
check_riru_version
enforce_install_from_magisk_app

# Check architecture
if [ "$ARCH" != "arm" ] && [ "$ARCH" != "arm64" ] && [ "$ARCH" != "x86" ] && [ "$ARCH" != "x64" ]; then
  abort "! Unsupported platform: $ARCH"
else
  ui_print "- Device platform: $ARCH"
fi

# Extract libs
ui_print "- Extracting module files"

extract "$ZIPFILE" 'module.prop' "$MODPATH"
extract "$ZIPFILE" 'post-fs-data.sh' "$MODPATH"
extract "$ZIPFILE" 'uninstall.sh' "$MODPATH"

# Riru v24+ load files from the "riru" folder in the Magisk module folder
# This "riru" folder is also used to determine if a Magisk module is a Riru module

mkdir "$MODPATH/riru"
mkdir "$MODPATH/riru/lib"
mkdir "$MODPATH/riru/lib64"

if [ "$ARCH" = "arm" ] || [ "$ARCH" = "arm64" ]; then
  ui_print "- Extracting arm libraries"
  extract "$ZIPFILE" "lib/armeabi-v7a/lib$RIRU_MODULE_LIB_NAME.so" "$MODPATH/riru/lib" true

  if [ "$IS64BIT" = true ]; then
    ui_print "- Extracting arm64 libraries"
    extract "$ZIPFILE" "lib/arm64-v8a/lib$RIRU_MODULE_LIB_NAME.so" "$MODPATH/riru/lib64" true
  fi
fi

if [ "$ARCH" = "x86" ] || [ "$ARCH" = "x64" ]; then
  ui_print "- Extracting x86 libraries"
  extract "$ZIPFILE" "lib/x86/lib$RIRU_MODULE_LIB_NAME.so" "$MODPATH/riru/lib" true

  if [ "$IS64BIT" = true ]; then
    ui_print "- Extracting x64 libraries"
    extract "$ZIPFILE" "lib/x86_64/lib$RIRU_MODULE_LIB_NAME.so" "$MODPATH/riru/lib64" true
  fi
fi

# Riru pre-v24 uses "/system", "/data/adb/riru/modules" is used as the module list
# If "/data/adb/riru/modules/example" exists, Riru will try to load "/system/lib(64)/libriru_example.so

# If your module does not need to support Riru pre-v24, you can raise the value of "moduleMinRiruApiVersion" in "module.gradle"
# and remove this part

if [ "$RIRU_API" -lt 11 ]; then
  ui_print "- Using old Riru"
  mv "$MODPATH/riru" "$MODPATH/system"
  mkdir -p "/data/adb/riru/modules/$RIRU_MODULE_ID_PRE24"
fi

set_perm_recursive "$MODPATH" 0 0 0755 0644

CONFIG_PATH="/data/misc/mph/config"

if [ ! -d /data/misc/mph ]
then
  rm -rf /data/misc/mph
  mkdir /data/misc/mph
fi

if [ ! -d "$CONFIG_PATH/properties" ]; then
    ui_print "- Creating default configuration (1)"
    mkdir -p "$CONFIG_PATH/properties"
    echo -n "V12" > "$CONFIG_PATH/properties/ro.miui.ui.version.name"
    echo -n "10" > "$CONFIG_PATH/properties/ro.miui.ui.version.code"
    echo -n "1592409600" > "$CONFIG_PATH/properties/ro.miui.version.code_time"
    echo -n "/sdcard/" > "$CONFIG_PATH/properties/ro.miui.internal.storage"
    echo -n "Xiaomi" > "$CONFIG_PATH/properties/ro.product.manufacturer"
    echo -n "Xiaomi" > "$CONFIG_PATH/properties/ro.product.brand"
    echo -n "Xiaomi" > "$CONFIG_PATH/properties/ro.product.name"
    echo -n "Xiaomi" > "$CONFIG_PATH/properties/ro.product.vendor.brand"
fi

if [ ! -d "$CONFIG_PATH/packages" ]; then
    ui_print "- Creating default configuration (2)"
    mkdir -p "$CONFIG_PATH/packages"
    
    touch "$CONFIG_PATH/packages/prefer_system"
    
    touch "$CONFIG_PATH/packages/cmb.pb"
    touch "$CONFIG_PATH/packages/cn.adidas.app"
    touch "$CONFIG_PATH/packages/com.autonavi.minimap"
    touch "$CONFIG_PATH/packages/com.coolapk.market"
    touch "$CONFIG_PATH/packages/com.dianping.v1"
    touch "$CONFIG_PATH/packages/com.eastmoney.android.fund"
    touch "$CONFIG_PATH/packages/com.eg.android.AlipayGphone"
    touch "$CONFIG_PATH/packages/com.huami.watch.hmwatchmanager"
    touch "$CONFIG_PATH/packages/com.icbc"
    touch "$CONFIG_PATH/packages/com.sankuai.meituan"
    touch "$CONFIG_PATH/packages/com.smzdm.client.android"
    touch "$CONFIG_PATH/packages/com.starbucks.cn"
    touch "$CONFIG_PATH/packages/com.taobao.idlefish"
    touch "$CONFIG_PATH/packages/com.taobao.taobao"
    touch "$CONFIG_PATH/packages/com.tencent.weread"
    touch "$CONFIG_PATH/packages/com.tigerbrokers.stock"
    touch "$CONFIG_PATH/packages/com.wudaokou.hippo"
    touch "$CONFIG_PATH/packages/com.xes.jazhanghui.activity"
    touch "$CONFIG_PATH/packages/com.xiaomi.hm.health"
    touch "$CONFIG_PATH/packages/com.xiaomi.smarthome"
    touch "$CONFIG_PATH/packages/com.xiaomi.wearable"
    touch "$CONFIG_PATH/packages/com.ximalaya.ting.android"
    touch "$CONFIG_PATH/packages/cool.dingstock.mobile"
    touch "$CONFIG_PATH/packages/me.ele"
    touch "$CONFIG_PATH/packages/org.xinkb.blackboard.android"
    touch "$CONFIG_PATH/packages/com.jingdong.app.mall"
    touch "$CONFIG_PATH/packages/com.tmri.app.main"
    touch "$CONFIG_PATH/packages/com.chinaworld.main"
    touch "$CONFIG_PATH/packages/com.max.xiaoheihe"
    touch "$CONFIG_PATH/packages/tv.danmaku.bili"
    touch "$CONFIG_PATH/packages/cmb.pb"
fi

set_perm_recursive /data/misc/mph 0 0 0755 0644
chcon -R u:object_r:magisk_file:s0 /data/misc/mph

ui_print "! Reboot is needed to apply config changes. !"
