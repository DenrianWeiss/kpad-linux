#!/bin/sh
# Build the Turner triband driver and its platform modules in a clean source tree.
set -eu
ref=${1:?path to the MT6991 reference git checkout}
out=${2:?absolute path to the configured Turner kernel output}
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
pci=$here/../pcie-host/Module.symvers
test -s "$pci"
rev=9f3c13fb89c5a166c303dcc8ca9ab9d1a40ff31d
src=$(mktemp -d)
trap 'rm -rf "$src"' EXIT
git -C "$ref" archive "$rev" include drivers/clk/mediatek drivers/misc/mediatek/uarthub drivers/misc/mediatek/include drivers/misc/mediatek/connectivity >"$src/source.tar"
(
    cd "$src"
    tar xf source.tar
    patch -s -p1 <"$here/turner.patch"
    patch -s -p1 <"$here/turner-txpower-v2.patch"
    patch -s -p1 <"$here/station-mgmt-filter.patch"
    patch -s -p1 <"$here/uart-hub.patch"
    patch -s -p1 <"$here/uart-receiver.patch"
    patch -s -p1 <"$here/uart-command-write.patch"
    patch -s -p1 <"$here/mt6653-linux-hci-rx.patch"
    patch -s -p1 <"$here/managed-power.patch"
)
c=$src/drivers/misc/mediatek/connectivity
symbols=$pci
includes="-I$src/include -I$src/drivers/misc/mediatek/include -I$src/drivers/misc/mediatek/include/mt-plat -I$c/connadp/common -I$c/compat/include"
build() {
    module=$1
    shift
    make -s -j8 -C "$out" ARCH=arm64 M="$c/$module" TARGET_BUILD_VARIANT=user KBUILD_EXTRA_SYMBOLS="$symbols" "$@" modules
    symbols="$symbols $c/$module/Module.symvers"
    cp "$c/$module/"*.ko "$here/modules/"
}
mkdir -p "$here/modules"
build connadp KCFLAGS="$includes"
build connfem KCFLAGS="$includes"
build conninfra CONFIG_MTK_COMBO_CHIP_CONSYS_6991=y CONFIG_MTK_CONNSYS_DEDICATED_LOG_PATH=y KCFLAGS="-include $src/include/linux/soc/mediatek/mtk_sip_svc.h -DCONFIG_MTK_COMBO_CHIP_CONSYS_6991=1 $includes"
build wlan/adaptor/build/connac3x KCFLAGS="$includes"
build wlan/core/gen4m CONFIG_MTK_PLATFORM=mt6991 SEGMENT=SP MODULE_NAME=wlan_drv_gen4m_6991_6653_triband KCFLAGS="$includes"
cp -R "$here/pins" "$src/pins"
(cd "$src/pins" && dtc -@ -I dts -O dtb -o pins.dtbo live.dts && xxd -i -n pins pins.dtbo > blob.h)
make -s -C "$out" ARCH=arm64 M="$src/pins" modules
cp "$src/pins/turner_pins_test.ko" "$here/modules/"

build bt/linux_v2 KCFLAGS="$includes -DCONFIG_MTK_UARTHUB=1"
make -s -j8 -C "$out" ARCH=arm64 M="$src/drivers/misc/mediatek/uarthub" CONFIG_MTK_UARTHUB=m \
    KCFLAGS="$includes -I$src/drivers/clk/mediatek -DCONFIG_MTK_UARTHUB_MODULE=1" KBUILD_EXTRA_SYMBOLS="$symbols" modules
cp "$src/drivers/misc/mediatek/uarthub/uarthub_drv.ko" "$here/modules/"
${CC:-cc} -static -O2 "$c/bt/linux_v2/uart_daemon/uart_launcher.c" -o "$here/uart_launcher"
