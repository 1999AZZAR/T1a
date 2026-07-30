# T1a Sensor & Actuator Map

This document outlines the hardware protocols, addressing, and control abstractions for the primary sensors and actuators supported by T1a.

## 1. OLED 128x64 (SSD1306 / SH1106)
* **Bus:** I2C
* **Default Address:** `0x3C` (sometimes `0x3D`)
* **T1a Alias:** `oled` (mapped in `i2c_aliases.txt`)
* **Control Abstraction:**
  * Requires a framebuffer (1024 bytes for 128x64) in RAM.
  * **Native C Wrapper:** T1a will need a native C function `nc_hw_oled_write(char* text)` that converts ASCII text to a bitmapped font and flushes the framebuffer over I2C to the display.
  * **LLM Tool:** `hw_oled_print(text)` to allow the agent to display internal state, IP address, or messages directly to the physical screen.

## 2. MPU6050 (Accelerometer & Gyroscope)
* **Bus:** I2C
* **Default Address:** `0x68` (or `0x69` if AD0 is pulled high)
* **T1a Alias:** `mpu6050` (mapped in `i2c_aliases.txt`)
* **Registers of Interest:**
  * Power Management 1: `0x6B` (Must write `0x00` to wake up)
  * Accel Data (X, Y, Z): `0x3B` to `0x40` (16-bit 2's complement)
  * Gyro Data (X, Y, Z): `0x43` to `0x48` (16-bit 2's complement)
  * Temp Data: `0x41` to `0x42`
* **Control Abstraction:**
  * **Native C Wrapper:** A background polling loop or specific read function `nc_hw_mpu_read()` that reads the 14 bytes of raw data, converts them to floats (g-force and degrees/sec), and exposes them.
  * **LLM Tool:** `hw_mpu_status()` returning JSON `{"ax": 0.0, "ay": 1.0, "az": 0.0, "gx": 0, "gy": 0, "gz": 0}`.

## 3. Passive/Active Buzzer
* **Bus:** GPIO (PWM or Digital Out)
* **T1a Alias:** `buzzer` (mapped in `gpio_aliases.txt`)
* **Control Abstraction:**
  * **Active Buzzer:** Simple GPIO HIGH/LOW. Can use existing `hw_gpio_write(buzzer, 1)`.
  * **Passive Buzzer:** Requires PWM to generate frequencies (pitch).
  * **Native C Wrapper:** `nc_hw_buzzer_tone(frequency, duration_ms)` utilizing software bit-banging or hardware PWM on the Luckfox.
  * **LLM Tool:** `hw_buzzer_beep(freq, duration)` to let the agent signal success, errors, or alerts audibly.

## 4. DHT11 / DHT22 (Temperature & Humidity)
* **Bus:** GPIO (Single-Wire Proprietary Protocol)
* **T1a Alias:** `dht` (mapped in `gpio_aliases.txt`)
* **Control Abstraction:**
  * **Protocol:** Requires precise microsecond timing. Host pulls LOW for 18ms (DHT11) or 1ms (DHT22), then reads 40 bits of data pulsing HIGH/LOW.
  * **Native C Wrapper:** Because Linux is not a Real-Time OS, reading DHT sensors directly via user-space GPIO can fail due to OS context switching. We will write `nc_hw_dht_read(pin)` using high-priority thread loops or falling-edge GPIO interrupts.
  * **LLM Tool:** `hw_dht_read()` returning JSON `{"temp_c": 24.5, "humidity": 60.0}`.
