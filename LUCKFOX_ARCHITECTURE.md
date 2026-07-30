# Implementation Plan: Luckfox Pico Mini Deployment

## Goal Description
The user pointed out a crucial economic reality: ESP32-S3 boards are often more expensive (~150k IDR) than the wildly cheap and powerful Luckfox Pico Mini (RV1103), which natively runs Linux.

Because the Luckfox runs Linux, we get to keep our `shell` tool, file system, POSIX sockets, and zero-dependency C code completely intact. This plan outlines the steps to formally document and optimize the T1a deployment process specifically for the Luckfox Pico Mini.

### Hardware Footprint & Topology
This architecture stacks two incredibly dense boards to create a Wi-Fi-enabled Linux brain that is barely larger than a postage stamp:
- **Luckfox Pico Mini (Main Compute):** 28 x 21 mm. Yields 17 usable GPIOs. It is the absolute smallest standalone Linux SBC capable of directly driving peripherals without a carrier board.
- **ESP32-C3 Super Mini (Network Coprocessor):** 22.5 x 18 mm. Yields 11 usable GPIOs. It offloads all radio/TCP-IP work without occupying the Luckfox USB Host port.

We will create a comprehensive `LUCKFOX_GUIDE.md` in the repository.

### 1. Cross-Compilation Pipeline
Instead of compiling on the Luckfox directly (which is slow and lacks resources), we will set up an x86_64 to Cortex-A7 cross-compilation workflow.
- **Toolchain:** `arm-rockchip830-linux-uclibcgnueabihf-gcc` (provided by Luckfox SDK).
- **BearSSL:** Compile BearSSL once using the cross-compiler.
- **Makefile Update:** Add a `make luckfox` target that overrides `CC` and `CFLAGS` specifically for the RV1103 architecture.

### 2. Buildroot / Alpine Linux Environment
The Luckfox Pico Mini typically runs a minimal Buildroot image.
- **Networking Architecture (ESP-Hosted over SPI):** Offload wireless connectivity to an ESP32-C3 Super Mini using the **ESP-Hosted** framework. The Luckfox RV1103 acts as the SPI Master and the ESP32-C3 as the SPI Slave. This requires a 6-pin hardware connection:
  - **SPI Bus (4 Pins):** MISO, MOSI, CLK, and CS.
  - **Control Pins (2 Pins):** Handshake (ESP32 signals readiness) and Data Ready (ESP32 signals data availability).
  This provides the Luckfox with a standard, high-bandwidth Linux network interface (`wlan0`/`eth0`) while keeping the USB Host port free.
- **Service Management:** Because Buildroot uses `sysvinit` (or sometimes `systemd` if configured, though rarely on 16MB SPI flash), we will provide a `/etc/init.d/S99t1a` daemon script instead of the current `systemd` user service.

### 3. Flash Memory Optimization
The cheapest Luckfox Pico Mini (without eMMC) boots from a tiny SPI NOR Flash (often 16MB or 32MB).
- **Binary Size:** Our 153KB binary is perfect for this.
- **Memory Storage:** We will map the `workspace_dir` for `chat.bin` and `guardian.jsonl` to `/tmp` (RAM disk) to prevent burning out the SPI flash, and optionally write a script to sync it to flash only on safe shutdown, or use an SD Card if the board has one.

## User Review Required
> [!IMPORTANT]
> Before I execute this plan and write the guide/Makefile updates, I need to know:
> 1. Does your Luckfox Pico Mini use the SPI Flash (Buildroot) or do you have an SD Card running a fuller Linux distro (like Alpine or Ubuntu)?
> 2. Do you want me to write the `make luckfox` Makefile target right now?
