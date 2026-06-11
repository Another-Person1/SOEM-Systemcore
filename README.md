# EC-Systemcore (UNOFFICIAL)
> [!NOTE]
> * This is **unofficial**, very **experimental**, and **not finished**.
> * I **don't claim ownership** of any third party trademarks or copyright mentioned here.
> * This repo will be optimized for the Limelight Systemcore, potentially other RPi 5 platforms might work but it's not guaranteed and thats not a priority.
> * **The code here isn't compatible with legacy platforms** such as RoboRIO 1/2, Control Hub, or other legacy platforms.

> [!WARNING]
> For now, **I don't recommend using this for anything safety-critical** since this project is in an alpha stage.

This repository is an **unofficial** attempt to add EtherCAT into Systemcore. In theory high speed EtherCAT communication can be unlocked by attaching a USB-Ethernet adapter (100 MBPS or above, gigabit is ideal) to any one of the Systemcore's 4 USB 3.0 ports, reducing CAN-FD bus usage while unlocking new possibilities for competition robotics.

This project packages a custom EtherCAT MainDevice daemon and dashboard around the **Simple Open EtherCAT Master (SOEM)** library for the Raspberry Pi Compute Module 5 ARM64 architecture.

---

## Systemcore Extension Suite

This repository is organized around a custom system package and an isolated upstream SOEM dependency:

*   **`/daemon`**: A C++17 ARM64 Linux daemon built as `/usr/bin/ec-systemcore-daemon` and installed with `ec-systemcore.service`.
*   **`/dashboard`**: A Bun/TypeScript backend with a vanilla JavaScript frontend for the local diagnostic interface.
*   **`/third_party/SOEM`**: The isolated SOEM source tree, intended to be managed as a Git submodule.
*   **`/scripts`**: Release automation, including `build_ipk.sh` for opkg-compatible package generation.

The entire suite is compiled together via the top-level CMake configuration and packaged as a standalone binary system installation file (`ec-systemcore.ipk`).

The daemon must remain a Black Channel routing component and must not implement custom heartbeat, watchdog, or FSoE safety behavior. Safety behavior belongs in certified EtherCAT components and ETG-defined mechanisms, not in this package.

---

## Contributions to This Repo

This project is fully open-source under the GPLv3 license. Contributions, pull requests, feedback, and bug fixes to this fork are highly encouraged and **do not** require a Contributor License Agreement (CLA) to be merged here. Just fork the repo, create a branch, open a PR and let's build!

> [!IMPORTANT]
> **For Upstream Submissions Only**: If you submit a core library fix here that you also wish to merge back into the official upstream RT-Labs project, you will be required to sign their corporate CLA as outlined below.

---

## Original Simple Open EtherCAT Master Library

* Copyright (C) 2005-2025 Speciaal Machinefabriek Ketels v.o.f.
* Copyright (C) 2005-2025 Arthur Ketels
* Copyright (C) 2009-2025 RT-Labs AB, Sweden

SOEM (Simple Open EtherCAT Master) is a software library for developing EtherCAT MainDevices.

This library is specifically designed for real-time communication in embedded systems. Its lightweight architecture minimizes resource consumption, making it suitable for environments with limited resources. SOEM can also be utilized on both Linux and Windows systems.

As a library rather than a standalone application, SOEM provides flexibility and customization for developers looking to implement EtherCAT technology. 

### Upstream Documentation

For core library documentation, APIs, and hardware integration maps, see the official RT-Labs documentation:  **https://docs.rt-labs.com/soem**

### Upstream Contributions

Contributions to the core upstream SOEM codebase are handled by RT-Labs. If you want to contribute to their upstream repository, you will need to sign a Contributor License Agreement and send it to them either by e-mail or by physical mail. More information is available on [https://rt-labs.com/contribution](https://rt-labs.com/contribution).
