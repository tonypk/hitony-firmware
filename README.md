# EchoEar Firmware (ESP-IDF)

MVP firmware for ESP32-S3 EchoEar: ESP-SR wake word + PCM16 WS streaming.

## Configure
- Edit `main/config.h`:
  - `ECHOEAR_WS_URL`
  - `ECHOEAR_DEVICE_ID`
  - `ECHOEAR_DEVICE_TOKEN`
  - I2S pin mapping

- Edit `sdkconfig.defaults`:
  - `CONFIG_ESP_WIFI_SSID`
  - `CONFIG_ESP_WIFI_PASSWORD`

## Build & Flash
```bash
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.usbmodem1101 flash
```

## Notes
- Audio format: PCM16 16kHz mono
- Wake word uses ESP-SR Wakenet model (default NihaoXiaozhi)
- TTS audio is streamed as binary frames from server
