# HiTony - International AI Voice Assistant

**HiTony** is the international version of xiaozhi, designed for English-speaking users with support for global AI models (OpenAI, Anthropic, Google, etc.).

## 🌍 Key Features

- **Multi-language Support**: English-first, with support for multiple languages
- **Global AI Models**: OpenAI GPT, Whisper, TTS-1, and extensible to other providers
- **Wake Word**: "Hi Tony" - optimized for English speakers
- **Music Streaming**: YouTube music playback with rhythm animation
- **Modern Architecture**: ESP32-S3 dual-core, LVGL UI, WebSocket streaming

## 🔧 Hardware

- **MCU**: ESP32-S3-WROOM-1 (N16R8, 240MHz dual-core)
- **Audio**: Dual ES7210 microphones + ES8311 speaker codec
- **Display**: 2.8" capacitive touch screen (240×320)
- **LED**: WS2812 RGB LED for status indication

## ⚙️ Configuration

### 1. Edit `main/config.h`:
```cpp
#define HITONY_WS_URL "ws://your-server.com:9001/ws"
#define HITONY_DEVICE_ID "hitony-001"
#define HITONY_DEVICE_TOKEN "your-secure-token"
```

### 2. Edit `sdkconfig.defaults`:
```
CONFIG_ESP_WIFI_SSID="YourWiFiSSID"
CONFIG_ESP_WIFI_PASSWORD="YourPassword"
```

## 🚀 Build & Flash

```bash
# Set target
idf.py set-target esp32s3

# Build
idf.py build

# Flash to device
idf.py -p /dev/cu.usbmodem1101 flash monitor
```

## 📡 Audio Pipeline

- **Input**: Dual microphones → AFE (noise suppression, beamforming) → Opus 48kbps VBR
- **Output**: WebSocket → Opus decode → I2S speaker
- **Wake Word**: ESP-SR WakeNet 9 (customizable)
- **VAD**: MODE_0 (quality mode, optimized for fast speech)

## 🌐 Server Requirements

HiTony firmware works with the HiTony server (Node.js/Python):
- WebSocket endpoint for audio streaming
- OpenAI API integration (Whisper ASR, GPT chat, TTS-1)
- Music streaming via YouTube
- Per-user API key management

## 📝 Notes

- Audio format: Opus 48kbps VBR, 16kHz mono
- Default wake word: "Hi ESP" (can be customized to "Hi Tony")
- TTS audio streamed as Opus packets from server
- Music playback with real-time beat detection and UI animation

## 🔗 Related Projects

- **Server**: [hitony-server](https://github.com/tonypk/hitony-server) (WebSocket + AI backend)
- **Original**: [xiaozhi](https://github.com/78/xiaozhi) (Chinese version)

## 📄 License

MIT License - See LICENSE file for details
