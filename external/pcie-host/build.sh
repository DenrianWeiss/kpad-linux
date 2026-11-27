#!/bin/sh
# Build the MT6991 reference host as a module for the Turner USB KDE fixture.
set -eu
ref=${1:?path to mainline-mt6991 git repository}
out=${2:?path to configured arm64 kernel output}
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
rev=9f3c13fb89c5a166c303dcc8ca9ab9d1a40ff31d
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
git -C "$ref" show "$rev:drivers/pci/controller/pcie-mediatek-gen3.c" >"$tmp/turner_ref_pci.c"
git -C "$ref" show "$rev:drivers/pci/controller/pcie-mediatek-gen3.h" >"$tmp/pcie-mediatek-gen3.h"
git -C "$ref" show "$rev:drivers/misc/mediatek/clkbuf/src/clkbuf-ctrl.h" >"$tmp/clkbuf-ctrl.h"
(cd "$tmp" && patch -s -p1 <"$here/linux73-module.patch")
printf 'ccflags-y += -I$(srctree)/drivers/pci\nobj-m := turner_ref_pci.o\n' >"$tmp/Makefile"
make -C "$out" ARCH=arm64 M="$tmp" modules
cp "$tmp/turner_ref_pci.ko" "$here/"
cp "$tmp/Module.symvers" "$here/Module.symvers"
