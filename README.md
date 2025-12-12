# PocketTrader

PocketTrader is a lightweight, end-to-end arbitrage trading demo designed for the **BeagleBone Black (AM3358)**. It demonstrates a high-performance, low-latency trading system architecture, including:

*   **Real-time Arbitrage Engine (C)**: Runs on the BeagleBone, processing UDP market data and making trade decisions.
*   **Exchange Simulator (Python)**: Runs on a host PC, simulating two exchanges (EXA and EXB) and matching orders.
*   **LCD Dashboard (Qt5)**: Runs on the BeagleBone, visualizing spreads, P&L, and latency in real-time.

---

## 1. System Architecture

The system is split into three main components:

1.  **Arbitrage Engine (`pockettrader_core_userspace/`)**
    *   **Language:** C (C99)
    *   **Role:** The "brain" of the trading bot.
    *   **Input:** Receives UDP multicast/unicast TICK data from EXA and EXB.
    *   **Logic:** Computes spread, checks risk limits, and sends TRADE orders via UDP.
    *   **IPC:** Updates a POSIX Shared Memory segment (`/pockettrader_shm`) with state for the GUI.

2.  **LCD Dashboard (`pockettrader_gui/`)**
    *   **Language:** C++ (Qt5 Widgets)
    *   **Role:** The "face" of the bot.
    *   **Logic:** Reads the Shared Memory segment at 10-20Hz to update the UI.
    *   **Features:** Displays Bid/Ask, Spread, Latency Histograms, and P&L.

3.  **Host Environment (`pockettrader_host/`)**
    *   **Language:** Python 3
    *   **Role:** The "market".
    *   **Components:**
        *   `exchange_sim.py`: Generates market data and handles order matching.
        *   `trade_bridge.py`: Receives trade requests from the BBB and forwards them to the exchange sim.

---

## 2. Prerequisites

### Hardware
*   **BeagleBone Black (BBB)** or compatible AM335x board.
*   **Host PC** (Linux recommended, or Windows/Mac with Python).
*   **Network Connection**: Ethernet or USB-Network between Host and BBB.

### Software (Host PC)
*   **Cross-Compiler**: `arm-linux-gnueabihf-gcc` (for compiling the C core).
*   **Python 3.9+**: For running the exchange simulator.

### Software (BeagleBone Black)
*   **Linux Kernel**: Standard Debian image or a Custom Kernel (see Section 5).
*   **Qt5 Libraries**: `qtbase5-dev`, `qt5-default` (or equivalent for your distro).

---

## 3. Build Instructions

### A. Compiling the Arbitrage Engine (C Core)

You can cross-compile this on your host PC or compile it directly on the BeagleBone.

**Option 1: Cross-Compile (Recommended)**
```bash
cd pockettrader_core_userspace
arm-linux-gnueabihf-gcc pockettrader_core.c -o pockettrader_core -lpthread -lrt -O3 -Wall
```

**Option 2: Native Compile (On BBB)**
```bash
cd pockettrader_core_userspace
gcc pockettrader_core.c -o pockettrader_core -lpthread -lrt -O3 -Wall
```

### B. Compiling the Dashboard (Qt GUI)

This is best done directly on the BeagleBone unless you have a configured Qt cross-compilation environment.

**On the BeagleBone:**
```bash
cd pockettrader_gui
qmake
make
```
*Note: Ensure you have `qt5-default` and `libqt5widgets5` installed.*

---

## 4. Running the System

### Step 1: Start the Host Simulation
On your Host PC, launch the market simulator. This script starts the exchanges and the trade bridge.

```bash
cd pockettrader_host
./run_normal.sh
```
*You should see output indicating EXA and EXB are sending ticks.*

### Step 2: Start the Arbitrage Engine (BBB)
Transfer the `pockettrader_core` binary to your BeagleBone.

```bash
# On BeagleBone
./pockettrader_core --exa-port 6001 --exb-port 6002 --trade-port 7000
```
*The core will initialize shared memory and start listening for ticks.*

### Step 3: Start the GUI (BBB)
Run the GUI application on the BeagleBone (requires a display or X11 forwarding).

```bash
# On BeagleBone
export DISPLAY=:0  # If running on attached LCD
./pockettrader_gui
```

---

## 5. Custom Linux Kernel & BeagleBone Setup

If you are building a custom Linux image (e.g., using Buildroot or Yocto) for the BeagleBone, follow these guidelines to ensure PocketTrader runs correctly.

### Kernel Configuration (`menuconfig`)
Ensure the following options are enabled in your Linux Kernel configuration:

*   **General Setup**:
    *   `CONFIG_POSIX_MQUEUE=y` (POSIX Message Queues)
    *   `CONFIG_SHMEM=y` (Shared Memory support)
*   **Networking**:
    *   `CONFIG_INET=y` (TCP/IP networking)
    *   `CONFIG_IP_MULTICAST=y` (If you plan to use multicast feeds)

### Root Filesystem Requirements
Your rootfs must include the following libraries:

1.  **Standard Libs**: `glibc` (or `musl`), `libpthread`, `librt`.
2.  **Qt5 Framework**:
    *   If using Buildroot: Enable `BR2_PACKAGE_QT5`, `BR2_PACKAGE_QT5BASE_WIDGETS`, `BR2_PACKAGE_QT5BASE_GUI`.
    *   If using Yocto: Add `qtbase` and `qtbase-plugins` to your image.
3.  **Fonts**: Ensure at least one font (e.g., DejaVu) is installed for Qt to render text.

### Deployment to Custom Image
1.  **Static Linking (Optional)**: If you don't want to manage shared libraries on the target, you can try statically linking the C core:
    ```bash
    arm-linux-gnueabihf-gcc pockettrader_core.c -o pockettrader_core -static -lpthread -lrt
    ```
    *(Note: Qt GUI is very hard to statically link; use shared libs for Qt).*

2.  **Copying Files**: Use `scp` to copy the compiled binaries (`pockettrader_core`, `pockettrader_gui`) to `/usr/bin/` or `/home/root/` on your target device.

---

## 6. Troubleshooting

*   **"shm_open: No such file or directory"**: Ensure your kernel supports shared memory and that `/dev/shm` is mounted. You can mount it manually:
    ```bash
    mkdir -p /dev/shm
    mount -t tmpfs tmpfs /dev/shm
    ```
*   **No Ticks Received**:
    *   Check Firewall on Host PC (allow UDP 6001/6002).
    *   Ensure BBB can ping the Host IP.
    *   Verify the IP address in `pockettrader_host` scripts matches your network setup.
*   **GUI fails to start**:
    *   "Could not connect to display": Make sure X11 is running or use `-platform linuxfb` if running directly on the framebuffer without X11.
