#!/bin/sh
# Build the RV SCP stack against an existing Turner arm64 kernel build.
set -eu
ref=${1:?path to mainline-mt6991 git repository}
out=${2:?path to configured kernel output}
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
rev=9f3c13fb89c5a166c303dcc8ca9ab9d1a40ff31d
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
git -C "$ref" archive "$rev" drivers/misc/mediatek/scp drivers/misc/mediatek/include drivers/misc/mediatek/sensorhub_probe drivers/clk/mediatek drivers/soc/mediatek/mtk-mbox.c drivers/soc/mediatek/mtk_tinysys_ipi.c drivers/rpmsg include/linux/soc/mediatek include/linux/rpmsg include/trace/events/mbox.h >"$tmp/src.tar"
(cd "$tmp" && tar xf src.tar && patch -s -p1 <"$here/turner-sensorhub.patch" && patch -s -p1 <"$here/turner-scp-dvfs.patch")
deps=$tmp/deps
mkdir "$deps"
cp "$tmp/drivers/soc/mediatek/mtk-mbox.c" "$tmp/drivers/soc/mediatek/mtk_tinysys_ipi.c" "$tmp/drivers/rpmsg/mtk_rpmsg_mbox.c" "$tmp/drivers/clk/mediatek/clk-fmeter-mt6991-core.c" "$deps/"
cp "$here/fmeter/turner_vlp_fmeter.c" "$deps/"
echo "CFLAGS_turner_vlp_fmeter.o += -I$tmp/drivers/clk/mediatek/mt6991" >"$deps/Makefile"
echo 'obj-m := turner_vlp_fmeter.o mtk-mbox.o mtk_rpmsg_mbox.o mtk_tinysys_ipi.o clk-fmeter-mt6991-core.o' >>"$deps/Makefile"
scp=$tmp/drivers/misc/mediatek/scp
hub=$tmp/drivers/misc/mediatek/sensorhub_probe
cflags="-I$tmp/include -I$scp/include -I$tmp/drivers/misc/mediatek/include -I$tmp/drivers/clk/mediatek"
make -s -C "$out" ARCH=arm64 M="$deps" KCFLAGS="-include linux/rpmsg.h -include $tmp/include/linux/rpmsg/mtk_rpmsg.h $cflags -I$tmp/drivers/rpmsg" modules
make -s -C "$out" ARCH=arm64 M="$scp/rv" KBUILD_EXTRA_SYMBOLS="$deps/Module.symvers" CONFIG_MTK_TINYSYS_SCP_SUPPORT=m CONFIG_OPLUS_SENSOR_WAKEUP_TRACE=n KCFLAGS="$cflags -DCONFIG_MTK_TINYSYS_SCP_SUPPORT_MODULE=1 -include $tmp/include/linux/soc/mediatek/mtk_sip_svc.h" modules
make -s -C "$out" ARCH=arm64 M="$hub" KBUILD_EXTRA_SYMBOLS="$deps/Module.symvers $scp/rv/Module.symvers" KCFLAGS="$cflags" modules
mkdir -p "$here/modules"
cp "$deps"/*.ko "$scp/rv/scp.ko" "$hub/sensorhub_probe.ko" "$here/modules/"
cp -R "$here/clock" "$tmp/clock"
(cd "$tmp/clock" && dtc -@ -I dts -O dtb -o clock.dtbo source.dts && xxd -i -n blob clock.dtbo > blob.h)
make -s -C "$out" ARCH=arm64 M="$tmp/clock" modules
cp "$tmp/clock/turner_scp_clock.ko" "$here/modules/"
