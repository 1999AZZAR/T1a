# T1a Hardware Maturation Roadmap

Our goal is to evolve T1a from a capable embedded AI agent into a plug-and-play autonomous operating system for robotics and smart appliances running primarily on the Luckfox Pico Mini RV1103.

## Phase 1: Foundation (Completed)
- [x] Abstracted GPIO control (export, unexport, read, write)
- [x] Abstracted I2C control (scan, read, write)
- [x] Dynamic Alias System (`gpio_aliases.txt`, `i2c_aliases.txt`)
- [x] Zero-Token Telegram Interactive Menus (`/gpio` and `/i2c`)
- [x] LLM Tool Exposure (`hw_gpio`, `hw_i2c`) for autonomous AI workflows

## Phase 2: Sensor & Output Expansion (In Progress)
- [ ] **PWM Abstraction:** Software PWM implementation (bit-banging via separate thread) or native hardware PWM for servo control.
- [ ] **UART Interface:** Abstract UART for communication with GPS modules, cellular modems, or external microcontrollers.
- [ ] **SPI Interface:** Add SPI tool support for high-speed displays or external memory.
- [ ] **Pre-built Component Libraries:** Build native C wrappers (invocable via LLM tools) for common hardware:
  - MPU6050 (Accelerometer/Gyroscope)
  - OLED 128x64 (SSD1306/SH1106)
  - Buzzer (Melody arrays)
  - DHT11/DHT22 (Temperature/Humidity)

## Phase 3: Autonomous Robotics
- [ ] **Real-Time Control Loop:** Create a dedicated background thread for the AI to register simple "if-this-then-that" rules without needing constant LLM polling (e.g. "if MPU6050 tilt > 30, write PWM 1500 to Servo1").
- [ ] **Vision Processing:** Given the RV1103 has a built-in NPU (Neural Processing Unit), explore integrating lightweight C-based NPU pipelines to feed visual context (person detection, line tracking) directly into T1a's context stream.
- [ ] **Self-Diagnostics:** Agent automatically scans I2C and GPIO at startup, comparing connected hardware against mapped aliases and alerting the user via Telegram if a sensor goes offline.

## Phase 4: Fleet & Mesh
- [ ] **Device-to-Device Mesh:** Allow multiple T1a units to communicate over local UDP (e.g. one Luckfox handles vision, another handles motor control).
- [ ] **OTA Updates:** Reliable over-the-air updates managed by the Telegram bot via a `/pull_update` native command.
