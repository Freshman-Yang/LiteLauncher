<p align="center">
  <img src="LiteLauncher/Assets/AppIcon.ico" alt="LiteLauncher Logo" width="96" height="96" />
</p>

<h1 align="center">LiteLauncher</h1>

<p align="center">
  A minimalist, lightweight application launcher for Windows.<br/>
  Millisecond response time and low resource usage, built to replace heavy system search.
</p>

<p align="center">
  <img src="https://img.shields.io/badge/Platform-Windows%2010%20%7C%2011-0078D6?logo=windows" alt="Platform" />
  <img src="https://img.shields.io/badge/C%2B%2B-20-blue.svg" alt="C++20" />
  <img src="https://img.shields.io/badge/Arch-x64%20%7C%20ARM64-brightgreen.svg" alt="Architecture" />
  <a href="LICENSE"><img src="https://img.shields.io/badge/License-MIT-green.svg" alt="License" /></a>
</p>

<p align="center">
  <b>English</b> | <a href="README_zh-CN.md">简体中文</a>
</p>

---

## ✨ Features

- **Comprehensive Indexing**: Automatically indexes Win32 applications, UWP (Microsoft Store) apps, and common Windows system settings.
- **Smart Fuzzy Search**: Supports prefix, substring, and abbreviation matching (e.g., `vsc` matches *VS Code*).
- **Quick Context Actions**: Right-click context menu support for **Run as administrator**, **Open file location**, and one-click **Uninstall** for UWP apps.
- **Customizable Appearance**: Native support for Windows 11 visual styles and Dark Mode, with switchable window backdrops including **Pure Blur**, **Acrylic**, and **Mica**.
- **Lightweight & Quiet**: Single-instance background process that automatically releases memory when the window is hidden.
- **Custom Scan Paths**: Add custom directories to scan in the settings panel.

---

## ⌨️ Keyboard Shortcuts

| Shortcut | Description |
| :--- | :--- |
| `Alt + Space` | Show / Hide the search window |
| `↑` / `↓` | Navigate through search results |
| `Enter` | Launch the selected application or command |
| `→` | Accept autocomplete suggestion |
| `Esc` | Hide the search window (or close context menu) |
| `Ctrl + A` | Select all text in the search box |

---

## 📌 Built-in Commands

Type any of the following commands into the search box:

- `Settings LiteLauncher`: Open Settings to configure window backdrop, language, startup behavior, and custom scan paths.
- `Refresh LiteLauncher`: Rescan and rebuild the application index cache immediately.
- `Exit LiteLauncher`: Exit the background process completely.

---

## 🛠️ Build Guide

### Prerequisites

- **OS**: Windows 10 / 11
- **IDE / Toolchain**: Visual Studio 2022 (with the **Desktop development with C++** workload installed)
- **Windows SDK**: Windows 10 SDK (`10.0.19041.0` or higher; `10.0.22621.0` recommended)
- **Supported Architectures**: `x64`, `ARM64`

### Build Steps

1. **Clone the repository**:
   ```bash
   git clone https://github.com/Freshman-Yang/LiteLauncher.git
   cd LiteLauncher
   ```

2. **Build with CMake Presets (Recommended — supports Visual Studio / VS Code / Ninja CLI)**:
   - **Visual Studio**: Open the project root directory via **Open a local folder**, then select `x64-release` / `x64-debug` / `arm64-release` / `arm64-debug` from the configuration dropdown at the top to configure and build automatically.
   - **Command Line** (Run inside **Visual Studio Developer PowerShell** or a `vcvarsall` environment):
     ```powershell
     # Windows x64 (Release / Debug)
     cmake --preset x64-release
     cmake --build --preset x64-release

     cmake --preset x64-debug
     cmake --build --preset x64-debug

     # Windows ARM64 (Release / Debug)
     cmake --preset arm64-release
     cmake --build --preset arm64-release
     ```

3. **Export clean standalone artifacts**:
   ```powershell
   cmake --install out/build/x64-release
   ```

The compiled executable will be located at `out/build/<preset>/LiteLauncher.exe` (or in the install directory `out/install/<preset>/LiteLauncher.exe`).

---

## 📄 License

This project is licensed under the [MIT License](LICENSE).



