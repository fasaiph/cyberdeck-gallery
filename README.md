# Cyberdeck Gallery — ESP32-S3-Touch-AMOLED-1.43

A tiny image-gallery **launcher** for the **Waveshare ESP32-S3-Touch-AMOLED-1.43**
(466×466 round AMOLED touchscreen). It boots with an animated splash, shows a home
screen of app icons, and opens each "app" into a full-screen, swipeable photo gallery —
all running from on-board flash, **no SD card required**.

| Boot | Home | App |
|---|---|---|
| pink "Cyberdeck Booting up!" spinner with a live `0–100%`, then fades into home | one rounded **app icon per folder** (the folder's first image); **tap** to open | **swipe ←/→** through photos; **swipe up** to close (photo shrinks back to its icon, iPhone-style) |

> ⚠️ **Board variant.** This targets the **non-C** `ESP32-S3-Touch-AMOLED-1.43`
> (SH8601/CO5300 display, FT3168 touch, microSD slot). It will **not** run on the
> `1.43C` AI-voice variant (different display pins + CST820 touch). Display/touch drivers
> are derived from Waveshare's official `ESP32-S3-AMOLED-1.43-Demo`.

---

## Option A — Flash the prebuilt firmware (no setup)

Use this to just put the demo on a board. Nothing to install.

The ready-to-flash image is **`prebuilt/cyberdeck-gallery-esp32s3.bin`** (flash at offset `0x0`).

**Easiest — web browser (Chrome or Edge):**
1. Go to **<https://espressif.github.io/esptool-js/>**.
2. Plug the board in over USB-C, click **Connect**, pick the serial port.
3. Add a file at **Flash Address `0x0`**, choose `prebuilt/cyberdeck-gallery-esp32s3.bin`.
4. Click **Program**. Done — the board reboots into the gallery.

**Or from a terminal** (only needs `pip install esptool`):
```bash
esptool --chip esp32s3 write_flash 0x0 prebuilt/cyberdeck-gallery-esp32s3.bin
```

> If the board won't connect, hold **BOOT**, tap **RESET**, release **BOOT**, then retry.

---

## Option B — Build from source (develop)

Use this to change the images, the UI, or the code.

### 1. Install ESP-IDF v5.5.x
Follow Espressif's [Get Started guide](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32s3/get-started/).
ESP-IDF needs **Python 3.9–3.13** (if your system Python is newer, point the installer at a
supported one, e.g. `brew install python@3.12` and run the IDF installer with it).

### 2. Build & flash
```bash
. $IDF_PATH/export.sh            # load the ESP-IDF environment
idf.py set-target esp32s3        # first time only
idf.py -p <PORT> flash monitor   # e.g. -p /dev/cu.usbmodem1101  (Linux: /dev/ttyACM0)
```
The first build auto-downloads the managed components `esp_lcd_sh8601` and `lvgl/lvgl` (v8).
`idf.py flash` also writes the bundled image to flash, so your photos go with it.

### 3. Use your own pictures
Each folder under `flash_data/` becomes an app on the home screen (its icon is the
folder's first image, alphabetically).
```
flash_data/
├── makeup/   step1.png … step5.png
└── hinge/    hinge-step1.png …
```
- Drop in your own **PNG** files (ideally **466×466**, the panel size — the round screen
  clips the corners). Name them so they sort in the order you want (`01.png`, `02.png`, …).
- **Add an app:** create a new folder under `flash_data/`, then add one line to the
  `s_album[]` array in `main/main.c`. A new icon appears automatically.
- Rebuild and flash — the flash filesystem image is regenerated each build.

### 4. Make a prebuilt binary (for Option A / releases)
```bash
idf.py merge-bin                 # -> build/merged-binary.bin (flash at 0x0)
cp build/merged-binary.bin prebuilt/cyberdeck-gallery-esp32s3.bin
```

---

## Hardware wiring (fixed on this board)
| Function | Signal | GPIO |
|---|---|---|
| AMOLED (QSPI, SPI2) | CS / PCLK / D0 / D1 / D2 / D3 / RST | 9 / 10 / 11 / 12 / 13 / 14 / 21 |
| Touch FT3168 (I2C, addr 0x38) | SDA / SCL | 47 / 48 |
| microSD slot (unused here; SPI) | CS / MOSI / MISO / SCLK | 38 / 39 / 40 / 41 |

## Project layout
```
.
├── CMakeLists.txt            builds + flashes the FAT image from flash_data/
├── partitions.csv            2M app + 3M read-only "storage" (FAT) partition
├── sdkconfig.defaults        S3 + octal PSRAM; LVGL 16-bit-swap + PNG + spinner; FATFS long names
├── flash_data/               images bundled into flash (makeup/, hinge/)
├── prebuilt/                 ready-to-flash merged firmware (Option A)
├── main/
│   ├── main.c                home launcher, gallery, gestures, loader, background decode
│   ├── display_bsp.c/.h      QSPI panel + FT3168 touch + LVGL bring-up
│   ├── idf_component.yml     deps: esp_lcd_sh8601, lvgl ^8.4
│   └── CMakeLists.txt
└── components/
    ├── read_lcd_id_bsp/      detects SH8601 vs CO5300 (bit-banged read of reg 0xDA)
    └── touch_bsp/            FT3168 touch driver
```

## How it works (notes)
- **Storage:** images are baked into a read-only FAT partition (`storage`) generated from
  `flash_data/` by `fatfs_create_rawflash_image()` in `CMakeLists.txt`, mounted at `/flash`.
- **Decoding:** at boot every PNG is decoded once into a compact RGB565 bitmap in PSRAM
  (`LV_IMG_CF_TRUE_COLOR`, ~434 KB each) so all navigation is instant. The decode runs on a
  low-priority background task calling `lodepng_decode32` directly (safe off the LVGL thread
  because `LV_MEM_CUSTOM=y`), while the LVGL task animates the loading spinner.
- **UI:** home is one LVGL screen; the open app is a transparent full-screen overlay on top
  of it, so the home screen shows through as the photo shrinks away on close.
- **Tuning:** loader look → `build_loader()`; slide speed → `SLIDE_MS`; open/close feel →
  `MINI_ZOOM` / `APP_ANIM_MS` (all in `main/main.c`).
- **Orientation:** the UI is rotated 90° so the USB-C port faces you (bottom), set by
  `disp_drv.rotated = LV_DISP_ROT_270` in `main/display_bsp.c`. Use `LV_DISP_ROT_90` for the
  opposite, or `LV_DISP_ROT_NONE` for USB on the left. LVGL rotates touch to match.
- **microSD:** this board has an SD slot (SPI pins above) but the demo doesn't use it.
  Note: very old / non-spec-compliant cards can fail SD init — use a standard modern card.

---

## Publishing this to GitHub

A `.gitignore` is included (it excludes `build/`, `sdkconfig`, and `managed_components/`;
it keeps `flash_data/`, `prebuilt/`, and `dependencies.lock`).

This folder is self-contained. If it currently lives inside the Waveshare example pack, copy
it somewhere standalone first so you get a clean repo:
```bash
cp -R 07_SD_Image_Gallery ~/cyberdeck-gallery && cd ~/cyberdeck-gallery
```
Then publish:
```bash
git init
git add .
git commit -m "Cyberdeck image gallery for ESP32-S3-Touch-AMOLED-1.43"

# with the GitHub CLI:
gh repo create cyberdeck-gallery --public --source=. --push

# or manually:
git branch -M main
git remote add origin https://github.com/<you>/cyberdeck-gallery.git
git push -u origin main
```

**For the no-setup install,** either keep `prebuilt/cyberdeck-gallery-esp32s3.bin` in the
repo (it's already committed) or attach it to a **GitHub Release**:
```bash
idf.py merge-bin
gh release create v1.0 build/merged-binary.bin -t "v1.0" -n "Flash at 0x0 with esptool or esptool-js"
```
