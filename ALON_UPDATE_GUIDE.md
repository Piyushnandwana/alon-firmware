# Alon firmware: how updates work (handoff guide for another AI or helper)

Paste this whole file to any AI assistant. It explains the project, the repo, the build pipeline, and how the device updates itself over WiFi, so the AI can help change the code and get it onto the device.

## 1. What the project is

"Alon" is a LEGO-minifigure desk pet running on a **Waveshare ESP32-S3-Touch-AMOLED-1.75C** (466x466 round AMOLED, capacitive touch, 16MB flash, octal PSRAM). It has a pet system (studs, XP, care), games, a zone-based home screen (clock top, weather right, avatar centre, battery arc bottom), and a WiFi-based OTA updater.

## 2. Links

- Code repo (private or public, owner Piyushnandwana): https://github.com/Piyushnandwana/alon-firmware
- Main sketch: https://github.com/Piyushnandwana/alon-firmware/blob/main/Alon.ino
- Avatar images header (RGB565 arrays): https://github.com/Piyushnandwana/alon-firmware/blob/main/img_alon.h
- Partition table: https://github.com/Piyushnandwana/alon-firmware/blob/main/partitions.csv
- Build workflow: https://github.com/Piyushnandwana/alon-firmware/blob/main/.github/workflows/build.yml
- Build runs (check pass/fail): https://github.com/Piyushnandwana/alon-firmware/actions
- Raw OTA firmware the device downloads: https://raw.githubusercontent.com/Piyushnandwana/alon-firmware/firmware-bin/Alon-ota-app.bin
- Raw full USB-flash image: https://raw.githubusercontent.com/Piyushnandwana/alon-firmware/firmware-bin/Alon-flash-at-0x0.bin

Note: if the repo is private, the raw.githubusercontent.com link will not work without a token, and the device's OTA download will fail. The repo (or at least that branch's files) must be publicly readable for the device to fetch it.

## 3. The update pipeline (end to end)

1. Edit `Alon.ino` (and `img_alon.h` or `partitions.csv` if needed) in the `main` branch.
2. `git push origin main`.
3. GitHub Actions workflow "Build Alon firmware" runs automatically on every push to `main`. It takes about 3 to 5 minutes.
4. The workflow installs the Arduino CLI, the ESP32 core, and the libraries, compiles the sketch, and produces two files:
   - `Alon-ota-app.bin`: app-only image used for wireless updates.
   - `Alon-flash-at-0x0.bin`: merged image (bootloader + partitions + app) used for USB flashing.
5. The workflow force-pushes both files to a separate branch called **`firmware-bin`**, so they are available as plain files at raw.githubusercontent.com.
6. On the device: open the WiFi screen and tap **CHECK UPDATE**. The device downloads `Alon-ota-app.bin` from the raw URL above, writes it to the spare OTA slot, and reboots into it.

If the build in step 3 fails, `firmware-bin` is not updated and the device keeps pulling the old firmware. This was the cause of "I still see the old avatar" earlier: the build had failed, so the old binary was still being served. Always check the Actions tab shows a green check before updating the device.

Checking a build from a terminal (GitHub CLI):

```
gh run list --limit 3
gh run view <run-id> --log-failed
```

## 4. Build settings (must match exactly)

- Board FQBN: `esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=custom,PSRAM=opi,CDCOnBoot=cdc`
- ESP32 Arduino core: `esp32:esp32@3.3.10`
- Core index URL: https://espressif.github.io/arduino-esp32/package_esp32_index.json
- Libraries: "GFX Library for Arduino" (Arduino_GFX, driver `Arduino_CO5300` over `Arduino_ESP32QSPI`), "SensorLib" (used only for the CST92xx touch controller), "WiFiManager"
- Built-in core libraries used: WiFi, WebServer, Update, HTTPUpdate, HTTPClient, NetworkClientSecure, Preferences, Wire
- Partition table: dual OTA, two 4MB app slots (`app0`/`app1`), 16MB flash. Defined in `partitions.csv` (copied next to the sketch by the workflow).

## 5. How the device does the OTA (code)

In `Alon.ino`:

```
#define OTA_URL "https://raw.githubusercontent.com/Piyushnandwana/alon-firmware/firmware-bin/Alon-ota-app.bin"
...
httpUpdate.rebootOnUpdate(true);
t_httpUpdate_return r = httpUpdate.update(client, OTA_URL);
```

- WiFi is set up with WiFiManager (the device opens a setup portal the first time, then remembers the network).
- The OTA runs from the WiFi screen's CHECK UPDATE button.
- Because of the dual-slot partition table, a failed or interrupted download leaves the currently running firmware untouched.

## 6. Flashing over USB (fallback, needed if OTA is broken or on first install)

1. Download `Alon-flash-at-0x0.bin` from the raw link in section 2 (or from the workflow run's Artifacts).
2. Flash it at offset **0x0** (for example with esptool or the Espressif web flasher, chip ESP32-S3).
3. Reset the device.

The USB flash is required whenever the partition table changes (OTA only replaces the app, not the partition table).

## 7. Things that bite when editing the code

- **Arduino auto-prototype trap:** the Arduino preprocessor auto-generates function prototypes, and it breaks on functions whose parameters use types defined later in the file (for example `const UiEvent &ev` or the `Expression` enum). Add explicit forward declarations for those functions right after the type is defined.
- **`drawFastVLine` is not available** on `Arduino_CO5300`. Use `fillRect(x, y, 1, h, color)`.
- **SensorLib's `imu.begin()` crashes the device** with the QMI8658. The IMU is driven with raw I2C register reads instead (address 0x6B, auto-detect 0x6B/0x6A).
- **Touch reset pin is GPIO40** and INT is GPIO11. GPIO2 is the SD-card clock on this board; do not pulse it.
- **Side buttons:** BOOT (GPIO0) is handled in code (toggles screen on/off). The other side button is the hardware reset on the EN pin and cannot be reprogrammed.
- **Images** are RGB565 arrays in `img_alon.h` drawn with `gfx->draw16bitRGBBitmap()`. Four 116x206 expressions (neutral, happy, sleepy, sad) drawn at x=175, y=134.
- **Home screen zones** redraw independently with cached values (`hm_last*`). Anything that draws over another zone must reset those caches so the zone repaints.

## 8. Quick local check before pushing (optional)

The sketch can be syntax-checked without the board by compiling it with stub headers for the Arduino libraries (`g++ -std=c++17 -fsyntax-only`). This catches most errors but not the Arduino auto-prototype issue, so always confirm the GitHub Actions run is green.

## 9. What to tell another AI to do

"Clone https://github.com/Piyushnandwana/alon-firmware, edit `Alon.ino`, commit and push to `main`. Wait for the 'Build Alon firmware' Actions run to turn green (about 4 minutes). Then I tap CHECK UPDATE on the device's WiFi screen and it pulls the new `Alon-ota-app.bin` from the `firmware-bin` branch. Read section 7 first so you avoid known build and hardware traps."

The AI needs push access to the repo (a GitHub account with write permission, or a personal access token) for the push step. It does not need access to the device itself: the device updates itself over WiFi once the build is green.
