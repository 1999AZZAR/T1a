# T1a Hardware Maturation Roadmap

Our goal is to evolve T1a from a capable embedded AI agent into a plug-and-play autonomous operating system for robotics and smart appliances, running primarily on the ultra-dense Luckfox Pico Mini RV1103.

## Phase 1: Foundation & Core Optimizations (Completed)
- [x] Abstracted GPIO control (export, unexport, read, write)
- [x] Abstracted I2C control (scan, read, write)
- [x] Dynamic Alias System (`gpio_aliases.txt`, `i2c_aliases.txt`)
- [x] Zero-Token Telegram Interactive Menus (`/gpio` and `/i2c`)
- [x] Resilient API polling with custom JSON, XML, and DSML tag parsers
- [x] Hybrid file-based storage architecture (`core_memory.txt`, `memory.jsonl`, `chat.bin`)
- [x] Deterministic native commands (`/start` and `/clear` memory purges)
- [x] Aggressive debloat (deprecated TSV storage, ripped out unused ACP protocol) achieving an ultra-lean < 150KB compiled binary

## Phase 2: Luckfox Pico RV1103 Deployment (In Progress)
- [ ] **Cross-Compilation Pipeline:** Implement a `make luckfox` target utilizing the `arm-rockchip830-linux-uclibcgnueabihf-gcc` toolchain for instant binary flashing.
- [ ] **Network Coprocessor (ESP-Hosted over SPI):** Offload Wi-Fi/BLE from the native Luckfox RV1103 (which lacks wireless) to an ESP32-C3 Super Mini using a high-bandwidth 6-pin SPI Master/Slave topology (MISO, MOSI, CLK, CS, Handshake, Data Ready).
- [ ] **Micro-Form Factor Stack:** Physically stack the 28x21mm Luckfox with the 22.5x18mm ESP32-C3 to create an ultra-dense, Wi-Fi enabled Linux brain featuring 28 combined GPIOs, barely larger than a postage stamp.
- [ ] **Service Management:** Replace `systemd` user services with a `/etc/init.d/S99t1a` daemon script specifically for Buildroot's lightweight `sysvinit`.

## Phase 3: Sensor & Output Expansion (Planned)
- [x] **PWM Abstraction:** Software PWM implementation (bit-banging via separate thread) or native hardware PWM for servo control.
- [ ] **UART Interface:** Abstract UART for communication with GPS modules, cellular modems, or external microcontrollers.
- [x] **Pre-built Component Libraries:** Build native C wrappers (invocable via LLM tools) for common hardware:
  - MPU6050 (Accelerometer/Gyroscope)
  - OLED 128x64 (SSD1306/SH1106)
  - Buzzer (Melody arrays)
  - DHT11/DHT22 (Temperature/Humidity)
  - RC Servos (Pan/Tilt)

## Phase 4: Autonomous Robotics
- [ ] **Real-Time Control Loop:** Create a dedicated background thread for the AI to register simple "if-this-then-that" rules without needing constant LLM polling (e.g. "if MPU6050 tilt > 30, write PWM 1500 to Servo1").
- [ ] **Vision Processing:** Given the RV1103 has a built-in NPU (Neural Processing Unit), explore integrating lightweight C-based NPU pipelines to feed visual context (person detection, line tracking) directly into T1a's context stream.
- [ ] **Self-Diagnostics:** Agent automatically scans I2C and GPIO at startup, comparing connected hardware against mapped aliases and alerting the user via Telegram if a sensor goes offline.

## Phase 5: Fleet & Mesh
- [ ] **Device-to-Device Mesh (Offloaded):** Shift the entire mesh networking logic (e.g., using ESP-NOW or a UDP mesh) onto the ESP32-C3 coprocessor. The Luckfox will only receive the final payload via SPI, saving massive CPU and RAM resources on the RV1103.
- [ ] **OTA Updates:** Reliable over-the-air updates managed by the Telegram bot via a `/pull_update` native command.
