# Surya Kernel — Gaming Mode + MemKernel + CI

Paket ini berisi file tambahan (bukan full source tree kernel) yang perlu
digabung ke `android_kernel_xiaomi_surya` (branch `rethinking`, kernel 4.14.x
non-GKI, base MIUI 12.0.9 / QJGMIXM).

> **Catatan penting soal governor & I/O scheduler**
> `cpufreq_schedhorizon.c` dan `zen-iosched.c` di sini adalah **implementasi
> referensi yang saya tulis dari nol** mengikuti pola umum governor/IO
> scheduler custom di kernel Android (interactive-style ramp + FIFO
> sync-priority), karena saya tidak punya akses ke source upstream
> "schedhorizon"/"zen-iosched" yang persis dipakai kernel tertentu. Kode ini
> memakai API legacy `cpufreq_governor.h` dan `elevator.c` yang standar di
> kernel 4.14 QCOM, jadi seharusnya compile dengan sedikit/tanpa perubahan —
> tapi **cek dulu** apakah tree `Cilok-LAB/android_kernel_xiaomi_surya` sudah
> punya salah satu dari governor/scheduler ini dari sononya (banyak kernel
> MSM 4.14 custom sudah menyertakan varian serupa). Kalau sudah ada, pakai
> yang built-in saja dan lewati langkah 2–3 di bawah.

## Struktur paket

```
surya-kernel-addon/
├── drivers/
│   ├── cpufreq/
│   │   ├── cpufreq_schedhorizon.c
│   │   ├── Kconfig.schedhorizon.fragment
│   │   └── Makefile.schedhorizon.fragment
│   └── misc/
│       ├── game_mode.c
│       └── Kconfig.game_mode.fragment
├── block/
│   ├── zen-iosched.c
│   └── Kconfig.iosched.zen.fragment
├── configs/
│   └── gaming_mode.config      # fragment defconfig, auto-merge oleh build.sh
├── build.sh
├── .github/workflows/build-kernel.yml
└── README.md (file ini)
```

## Langkah integrasi manual

### 1. Clone source tree

```bash
git clone https://github.com/Cilok-LAB/android_kernel_xiaomi_surya.git -b rethinking surya-kernel
cd surya-kernel
```

Salin seluruh isi `surya-kernel-addon/` (kecuali README ini) ke root
`surya-kernel/`, lalu jalankan langkah 2–5.

### 2. game_mode (flag global, wajib untuk fitur lain)

```bash
cp -r ../surya-kernel-addon/drivers/misc/game_mode.c drivers/misc/
```

Tempel isi `drivers/misc/Kconfig.game_mode.fragment` ke `drivers/misc/Kconfig`,
dan tambahkan baris berikut ke `drivers/misc/Makefile`:

```makefile
obj-$(CONFIG_GAME_MODE)			+= game_mode.o
```

### 3. Governor schedhorizon

```bash
cp ../surya-kernel-addon/drivers/cpufreq/cpufreq_schedhorizon.c drivers/cpufreq/
```

- Tempel isi `Kconfig.schedhorizon.fragment` ke `drivers/cpufreq/Kconfig`
  (config di luar `choice`, plus opsi `CPU_FREQ_DEFAULT_GOV_SCHEDHORIZON`
  di dalam blok `choice ... endchoice`).
- Tempel isi `Makefile.schedhorizon.fragment` ke `drivers/cpufreq/Makefile`.

### 4. I/O scheduler zen

```bash
cp ../surya-kernel-addon/block/zen-iosched.c block/
```

- Tempel isi `block/Kconfig.iosched.zen.fragment` ke `block/Kconfig.iosched`
  (ikuti instruksi di dalam file untuk `config IOSCHED_ZEN` dan
  `config DEFAULT_ZEN`).
- Tambahkan ke `block/Makefile`:
  ```makefile
  obj-$(CONFIG_IOSCHED_ZEN)		+= zen-iosched.o
  ```

### 5. Defconfig

Cek nama file dulu:

```bash
ls arch/arm64/configs/vendor/ | grep -i surya
```

Tempel isi `configs/gaming_mode.config` ke akhir file tersebut. (`build.sh`
juga akan mencoba auto-merge fragment ini kalau belum ada — lihat langkah di
bawah — jadi kalau lupa manual pun masih ter-cover, asalkan nama
`DEFCONFIG` di `build.sh` sudah benar.)

### 6. MemKernel sebagai LKM

Tidak perlu dilakukan manual — `build.sh` sudah menjalankan:

```bash
curl -LSs "https://raw.githubusercontent.com/Poko-Apps/MemKernel/main/kernel/setup.sh" | bash -s M memk
```

sebelum build, dan hasil `memk_memk.ko` otomatis dimasukkan ke zip
AnyKernel3 di `modules/vendor/lib/modules/`.

Kalau mau MemKernel statis (built-in, bukan module), ganti parameter `M`
jadi `Y` di baris tersebut di dalam `build.sh`.

### 7. build.sh

Sesuaikan dua hal di `build.sh` sebelum dipakai:

- `DEFCONFIG="vendor/surya-stock_defconfig"` → ganti sesuai nama file asli
  dari langkah 5.
- URL toolchain Clang r383902 — link di script mengambil dari mirror AOSP
  resmi; kalau berubah/404, ganti dengan mirror lain (mis. dari repo
  `kdrag0n/proton-clang` versi lama, atau upload toolchain sendiri sebagai
  release GitHub dan `curl` dari situ).

Jalankan lokal untuk tes cepat sebelum push ke CI:

```bash
chmod +x build.sh
./build.sh
```

### 8. Bot Telegram + GitHub Secrets

1. Buat bot lewat [@BotFather](https://t.me/BotFather), simpan token.
2. Dapatkan chat ID lewat [@GetIDsBot](https://t.me/GetIDsBot).
3. Di repo GitHub → **Settings → Secrets and variables → Actions**, tambahkan:
   - `TELEGRAM_BOT_TOKEN`
   - `TELEGRAM_CHAT_ID`

### 9. Push dan jalankan workflow

```bash
git add .
git commit -m "Add gaming mode (schedhorizon + zen-iosched), MemKernel LKM, CI"
git push origin rethinking
```

Lalu jalankan lewat tab **Actions → Build Kernel Surya → Run workflow**
(trigger-nya `workflow_dispatch`, jadi manual). Bot Telegram akan mengirim
notifikasi mulai build, lalu file `.zip` AnyKernel3 begitu selesai — atau
notifikasi gagal kalau ada error, lengkap dengan link ke log run-nya.

## Cara pakai di HP setelah flash

```
echo 1 > /sys/kernel/game_mode/enable          # aktifkan mode gaming
echo schedhorizon > /sys/devices/system/cpu/cpufreq/policy0/scaling_governor
echo zen > /sys/block/sda/queue/scheduler       # sesuaikan nama block device (mmcblk0/sda/dm-0, cek `ls /sys/block/`)
```

Otomatisasi toggle `game_mode` berdasarkan foreground app biasanya
dilakukan lewat Magisk module/Tasker/script userspace terpisah — bukan
bagian dari kernel ini.

## Troubleshooting singkat

| Gejala | Kemungkinan penyebab |
|---|---|
| `unknown symbol game_mode_is_active` saat load module | `drivers/misc/game_mode.c` harus built-in (`=y`), bukan module, kalau governor/iosched di-compile built-in juga. Kalau semua LKM, pastikan urutan insmod: game_mode dulu. |
| `memk_memk.ko` tidak muncul di `out/` | Cek log `setup.sh` MemKernel — kadang perlu `make modules` terpisah setelah `make Image`. Tambahkan `make O=out ARCH=arm64 modules` di `build.sh` kalau perlu. |
| Governor/iosched tidak muncul di `scaling_available_governors` / `scheduler` | Defconfig belum ke-apply — cek `out/.config` mengandung `CONFIG_CPU_FREQ_GOV_SCHEDHORIZON=y` / `CONFIG_IOSCHED_ZEN=y` setelah `make defconfig`. |
| Build gagal di step Clang | Versi Clang r383902 kadang butuh `LD=ld.lld` eksplisit untuk beberapa tree; tambahkan `LD=ld.lld` ke perintah `make` kalau linking gagal. |
