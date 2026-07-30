# T1a Sensor & Actuator Map

This document outlines the hardware protocols, addressing, and native C implementations for the primary sensors and actuators supported by T1a on the Luckfox RV1103.

## 1. OLED 128x64 (SSD1306 / SH1106)
* **Bus:** I2C
* **Default Address:** `0x3C` (sometimes `0x3D`)
* **T1a Alias:** `oled` (mapped in `i2c_aliases.txt`)
* **Control Abstraction:**
  * **Native C Wrapper:** Implemented in `src/hw_oled.c`. It contains a hardcoded 5x7 ASCII bitmap font and automatically renders string text into a 1024-byte RAM framebuffer, wrapping lines as needed. The full framebuffer is then flushed to the screen using the `0x40` data prefix over I2C.
  * **LLM Tool:** `hw_oled_print(text)` to allow the agent to display internal state, IP address, or messages directly to the physical screen in readable text.

## 2. MPU6050 (Accelerometer, Gyroscope & Temperature)
* **Bus:** I2C
* **Default Address:** `0x68` (or `0x69` if AD0 is pulled high)
* **T1a Alias:** `mpu6050` (mapped in `i2c_aliases.txt`)
* **Registers of Interest:**
  * Power Management 1: `0x6B` (Must write `0x00` to wake up)
  * Accel Data (X, Y, Z): `0x3B` to `0x40` (16-bit 2's complement)
  * Temp Data: `0x41` to `0x42`
  * Gyro Data (X, Y, Z): `0x43` to `0x48` (16-bit 2's complement)
* **Control Abstraction:**
  * **Native C Wrapper:** Implemented in `src/hw_mpu6050.c`. Reads 14 sequential bytes to capture all 6 axes plus the internal temperature sensor, crunching the math to convert them to G-force, degrees/sec, and Celsius.
  * **LLM Tool:** `hw_mpu_status()` returning JSON `{"ax": 0.0, "ay": 1.0, "az": 0.0, "gx": 0, "gy": 0, "gz": 0, "temp_c": 24.5}`.

## 3. Passive/Active Buzzer
* **Bus:** GPIO (PWM or Digital Out)
* **T1a Alias:** `buzzer` (mapped in `gpio_aliases.txt`)
* **Control Abstraction:**
  * **Native C Wrapper:** Implemented in `src/hw_buzzer.c` utilizing Linux Hardware PWM (`/sys/class/pwm/pwmchip0/`). It calculates nanosecond periods for specific Hz frequencies and plays them synchronously.
  * **Melodies:** Supports raw frequencies or predefined melody strings including the 7 standard notes (`do`, `re`, `mi`, `fa`, `sol`, `la`, `si`) and droid personality sounds (`r2d2_happy`, `r2d2_sad`, `r2d2_confused`).
  * **LLM Tool:** `hw_buzzer_beep(melody, freq, duration_ms)` to let the agent signal success, errors, or alerts audibly.

## 4. DHT11 / DHT22 (Temperature & Humidity)
* **Bus:** GPIO (Single-Wire Proprietary Protocol)
* **T1a Alias:** `dht` (mapped in `gpio_aliases.txt`)
* **Control Abstraction:**
  * **Native C Wrapper:** Implemented in `src/hw_dht.c`. To bypass user-space OS scheduling latency which ruins the microsecond timing of the 1-wire protocol, T1a delegates this entirely to the Linux Kernel. It reads directly from the Industrial I/O (IIO) subsystem (`/sys/bus/iio/devices/iio:device0/in_temp_input`). *(Requires `CONFIG_DHT11` enabled in kernel config)*.
  * **LLM Tool:** `hw_dht_read()` returning JSON `{"temp_c": 24.5, "humidity": 60.0}`.

## 5. Standard 180-Degree RC Servos (Pan/Tilt)
* **Bus:** Hardware PWM (20ms Period / 50Hz)
* **T1a Alias:** `servo0`, `servo1`
* **Control Abstraction:**
  * **Native C Wrapper:** Implemented in `src/hw_servo.c` using the Linux PWM subsystem (`/sys/class/pwm/pwmchip1` and `pwmchip2`). It accepts a 0 to 180 degree integer and calculates the necessary 500us to 2500us pulse length to accurately actuate standard micro servos (e.g. SG90).
  * **LLM Tool:** `hw_servo_set(servo_id, angle)` (where `servo_id` is 0 or 1, and `angle` is 0-180) to physically orient cameras or manipulate physical arms.

## 6. Direct IO (Status LEDs, Buttons, Relays)
* **Bus:** Standard GPIO
* **T1a Alias:** `F1`, `F2`, `F3`, `F4`, `F5` (mapped in `gpio_aliases.txt`)
* **Control Abstraction:**
  * **Native C Wrapper:** Implemented in `src/hw_directio.c`, proxying requests directly to the underlying `hw_gpio` subsystem. Provides an ultra-simple interface for LLM control of the 5 remaining unallocated pins on the RV1103.
  * **LLM Tool:** `hw_directio(pin, state)`. `pin` must be `"F1"` through `"F5"`. Omit `state` to read, provide `0` or `1` to write.
