# T1a Hardware Pinout Map (Luckfox Pico Mini B RV1103)

The Luckfox Pico Mini B (RV1103) features 17 exposed GPIO pins, highly multiplexed. To ensure we do not exhaust our resources or create hardware conflicts, T1a enforces the following strict pin mapping for all Phase 2 and Phase 3 peripherals.

> **Note:** Pin configurations (I2C, SPI, PWM) must be enabled in the device tree using the `luckfox-config` tool on the board.

## Shared Buses

### I2C Bus (I2C3)
Shared by the **OLED 128x64 (0x3C)** and the **MPU6050 (0x68)**.
* **Pin 54 (GPIO1_C6):** I2C3_SDA
* **Pin 55 (GPIO1_C7):** I2C3_SCL

### SPI Bus (SPI0) - *ESP32-C3 Network Coprocessor*
Dedicated high-bandwidth bus for offloading Wi-Fi, BLE, and Mesh networking to the ESP32-C3 Super Mini.
* **Pin 50 (GPIO1_C2):** SPI0_MISO
* **Pin 51 (GPIO1_C3):** SPI0_MOSI
* **Pin 52 (GPIO1_C4):** SPI0_CLK
* **Pin 53 (GPIO1_C5):** SPI0_CS0
* **Pin 17 (GPIO1_C1):** Standard GPIO (Host Handshake to ESP32)
* **Pin 56 (GPIO1_D0):** Standard GPIO (Data Ready from ESP32)

## Actuators (PWM)

### Pan/Tilt Servos
Hardware PWM outputs for 50Hz RC Servos.
* **Pin 4 (GPIO1_A4 / PWM10_M1):** Servo 0 (Pan)
* **Pin 5 (GPIO1_A5 / PWM11_M1):** Servo 1 (Tilt)

### Passive Buzzer
Hardware PWM for R2-D2 melodies and standard notes.
* **Pin 15 (GPIO1_B7 / PWM1_M0):** Buzzer Audio Out

## Sensors (1-Wire)

### DHT11 / DHT22
Delegated to the Linux Kernel IIO subsystem for microsecond-accurate timing.
* **Pin 16 (GPIO1_C0):** DHT Data Pin

## Summary of Resource Utilization
* **Total Pins Available:** 17
* **Pins Allocated:** 12
* **Pins Remaining:** 5 (Available for future UART GPS/Cellular modems)
