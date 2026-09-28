#!/usr/bin/env bash
# build.sh — Build script kernel surya (POCO X3 NFC), MIUI 12.0.9, kernel 4.14.x
#
# Dipakai oleh GitHub Actions (.github/workflows/build-kernel.yml) lewat:
#   source build.sh
# sehingga variabel ZIPNAME diekspor ke $GITHUB_ENV oleh workflow.
#
# Bisa juga dijalankan manual di WSL/Linux:
#   chmod +x build.sh && ./build.sh
set -e

# ----------------------------- Konfigurasi -----------------------------
export KBUILD_BUILD_USER="Asep"
export KBUILD_BUILD_HOST="surya-ci"

DEFCONFIG="vendor/surya-stock_defconfig"   # sesuaikan nama file defconfig
ARCH=arm64
SUBARCH=arm64
OUT_DIR="out"
ANYKERNEL_DIR="AnyKernel3"
ANYKERNEL_REPO="https://github.com/osm0sis/AnyKernel3.git"

# Toolchain Clang r383902 (Clang 11) — sesuaikan path setelah diekstrak
CLANG_DIR="${CLANG_DIR:-$HOME/clang-r383902}"
export PATH="${CLANG_DIR}/bin:${PATH}"

export ARCH SUBARCH
export CLANG_TRIPLE=aarch64-linux-gnu-
export CROSS_COMPILE=aarch64-linux-gnu-
export CROSS_COMPILE_ARM32=arm-linux-gnueabi-

THREADS=$(nproc --all)

# ------------------------- Siapkan toolchain -------------------------
if [ ! -d "${CLANG_DIR}" ]; then
	echo ">> Mengunduh Clang r383902..."
	mkdir -p "${CLANG_DIR}"
	curl -LSs "https://android.googlesource.com/platform/prebuilts/clang/host/linux-x86/+archive/refs/tags/android-11.0.0_r48/clang-r383902.tar.gz" \
		-o /tmp/clang.tar.gz
	tar -xzf /tmp/clang.tar.gz -C "${CLANG_DIR}"
fi

if [ ! -d "gcc64" ]; then
	echo ">> Mengunduh GCC aarch64 (fallback CROSS_COMPILE)..."
	git clone --depth=1 https://github.com/mvaisakh/gcc-arm64 gcc64
fi
if [ ! -d "gcc32" ]; then
	echo ">> Mengunduh GCC arm32..."
	git clone --depth=1 https://github.com/mvaisakh/gcc-arm gcc32
fi
export PATH="$(pwd)/gcc64/bin:$(pwd)/gcc32/bin:${PATH}"

# --------------------------- Integrasi MemKernel LKM ---------------------------
if [ ! -d "drivers/memkernel" ]; then
	echo ">> Menambahkan MemKernel sebagai LKM..."
	curl -LSs "https://raw.githubusercontent.com/Poko-Apps/MemKernel/main/kernel/setup.sh" | bash -s M memk
fi

# --------------------------- Gabungkan defconfig gaming ---------------------------
GAMING_FRAGMENT="configs/gaming_mode.config"
DEFCONFIG_PATH="arch/${ARCH}/configs/${DEFCONFIG}"
if [ -f "${GAMING_FRAGMENT}" ] && [ -f "${DEFCONFIG_PATH}" ]; then
	if ! grep -q "CONFIG_CPU_FREQ_GOV_SCHEDHORIZON" "${DEFCONFIG_PATH}"; then
		echo ">> Menggabungkan ${GAMING_FRAGMENT} ke ${DEFCONFIG_PATH}"
		grep -v '^#' "${GAMING_FRAGMENT}" | grep -v '^$' >> "${DEFCONFIG_PATH}"
	fi
else
	echo "!! Peringatan: ${DEFCONFIG_PATH} atau ${GAMING_FRAGMENT} tidak ditemukan, lewati auto-merge."
fi

# ------------------------------- Build -------------------------------
echo ">> make ${DEFCONFIG}"
make O=${OUT_DIR} ARCH=${ARCH} ${DEFCONFIG}

echo ">> make -j${THREADS}"
make -j"${THREADS}" O=${OUT_DIR} ARCH=${ARCH} \
	CC=clang \
	CLANG_TRIPLE=${CLANG_TRIPLE} \
	CROSS_COMPILE=${CROSS_COMPILE} \
	CROSS_COMPILE_ARM32=${CROSS_COMPILE_ARM32} \
	2>&1 | tee build.log

IMAGE="${OUT_DIR}/arch/${ARCH}/boot/Image.gz-dtb"
if [ ! -f "${IMAGE}" ]; then
	IMAGE="${OUT_DIR}/arch/${ARCH}/boot/Image.gz"
fi
if [ ! -f "${IMAGE}" ]; then
	echo "!! Build gagal: kernel image tidak ditemukan."
	exit 1
fi

MEMK_KO="${OUT_DIR}/drivers/memkernel/memk_memk.ko"

# ----------------------------- Kemas AnyKernel3 -----------------------------
if [ ! -d "${ANYKERNEL_DIR}" ]; then
	git clone --depth=1 "${ANYKERNEL_REPO}" "${ANYKERNEL_DIR}"
fi

cp "${IMAGE}" "${ANYKERNEL_DIR}/Image.gz-dtb" 2>/dev/null || cp "${IMAGE}" "${ANYKERNEL_DIR}/Image.gz"

if [ -f "${MEMK_KO}" ]; then
	mkdir -p "${ANYKERNEL_DIR}/modules/vendor/lib/modules"
	cp "${MEMK_KO}" "${ANYKERNEL_DIR}/modules/vendor/lib/modules/"
	echo ">> MemKernel LKM disertakan: ${MEMK_KO}"
else
	echo "!! Peringatan: memk_memk.ko tidak ditemukan, LKM tidak disertakan di zip."
fi

BUILD_TAG="$(date '+%Y%m%d-%H%M')"
ZIPNAME="Surya-Kernel-Gaming-${BUILD_TAG}.zip"

pushd "${ANYKERNEL_DIR}" >/dev/null
zip -r9 "../${ZIPNAME}" . -x ".git*" -x "*.md"
popd >/dev/null

echo ">> Selesai: ${ZIPNAME}"
export ZIPNAME
