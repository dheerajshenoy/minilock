# MiniLock

Wayland screen locking utility with wide image support (including animated ones).

# Installation

1. Clone the repository:
   ```bash
   git clone https://github.com/dheerajshenoy/minilock.git
   cd minilock
    ```

2. Install dependencies:
   ```bash
   sudo apt install build-essential cmake libwayland-dev libxkbcommon-dev libjpeg-dev libpng-dev
   ```

3. Install optional dependencies for other image format image support:
   ```bash
   sudo apt install libgif-dev libwebp-dev
   ```

4. Build the project:
   ```bash
   mkdir build
   cd build
   cmake ..
   make
   ```

5. Install the binary:
   ```bash
   sudo make install
   ```

<!-- # Add a note -->
> [!NOTE]
> Image format dependencies are loaded at runtime, so you can install them later if you want support for more image formats.

# Supported Image Formats

- JPEG
- PNG
- GIF (optional)
- WebP (optional)
- BMP (optional)
- TIFF (optional)
- SVG (optional)
