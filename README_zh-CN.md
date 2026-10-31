<p align="center">
  <img src="LiteLauncher/Assets/AppIcon.ico" alt="LiteLauncher Logo" width="96" height="96" />
</p>

<h1 align="center">LiteLauncher</h1>

<p align="center">
  专为 Windows 打造的极简、轻量级快速启动器。<br/>
  毫秒级响应，极低资源占用，替代繁重的系统搜索。
</p>

<p align="center">
  <img src="https://img.shields.io/badge/Platform-Windows%2010%20%7C%2011-0078D6?logo=windows" alt="Platform" />
  <img src="https://img.shields.io/badge/C%2B%2B-20-blue.svg" alt="C++20" />
  <img src="https://img.shields.io/badge/Arch-x64%20%7C%20ARM64-brightgreen.svg" alt="Architecture" />
  <a href="LICENSE"><img src="https://img.shields.io/badge/License-MIT-green.svg" alt="License" /></a>
</p>

<p align="center">
  <a href="README.md">English</a> | <b>简体中文</b>
</p>

---

## ✨ 特性

- **全面检索**：自动索引 Win32 程序、UWP 商店应用及常用 Windows 系统设置。
- **智能模糊搜索**：支持拼音、首字母及英文缩写匹配（如 `wx` 匹配“微信”，`vsc` 匹配 VS Code）。
- **快捷操作**：支持右键菜单，提供“以管理员身份运行”、“打开文件所在位置”以及 UWP 应用“一键卸载”。
- **个性化外观**：原生适配 Windows 11 视觉风格与深色模式，支持透明模糊、亚克力（Acrylic）、云母（Mica）等多种窗口材质切换。
- **轻量常驻**：后台单实例运行，窗口隐藏时自动释放空闲内存。
- **自定义路径**：支持在设置中添加自定义扫描目录。

---

## ⌨️ 快捷键

| 按键 | 说明 |
| :--- | :--- |
| `Alt + Space` | 呼出 / 隐藏搜索框 |
| `↑` / `↓` | 切换选中的搜索结果 |
| `Enter` | 打开选中的应用或执行指令 |
| `→` | 补全自动提示的文本 |
| `Esc` | 隐藏搜索框（或关闭右键菜单） |
| `Ctrl + A` | 全选输入框文本 |

---

## 📌 内置指令

在搜索框直接输入以下指令即可执行对应操作：

- `设置 LiteLauncher`：打开设置窗口，配置窗口材质、语言、开机自启及自定义检索目录。
- `刷新 LiteLauncher`：立即重新扫描并刷新应用索引。
- `退出 LiteLauncher`：完全退出后台进程。

---

## 🛠️ 构建指南

### 环境要求

- **操作系统**：Windows 10 / 11
- **开发工具**：Visual Studio 2022（需勾选 **使用 C++ 的桌面开发**）
- **SDK**：Windows 10 SDK (10.0.19041.0 及以上，推荐 10.0.22621.0)
- **支持架构**：`x64`、`ARM64`

### 编译步骤

1. **克隆仓库**：
   ```bash
   git clone https://github.com/Freshman-Yang/LiteLauncher.git
   cd LiteLauncher
   ```

2. **使用 CMake Presets 构建（推荐，支持 Visual Studio / VS Code / Ninja CLI）**：
   - **Visual Studio**：直接通过“打开本地文件夹”打开项目根目录，在顶部配置下拉框选择 `x64-release` / `x64-debug` / `arm64-release` / `arm64-debug` 即可自动生成与构建。
   - **命令行（需在 Visual Studio Developer PowerShell / `vcvarsall` 环境下执行）**：
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

3. **导出纯净可执行产物**：
   ```powershell
   cmake --install out/build/x64-release
   ```

构建产物位于 `out/build/<preset>/LiteLauncher.exe`（或安装目录 `out/install/<preset>/LiteLauncher.exe`）。

---

## 📄 开源许可

本项目基于 [MIT License](LICENSE) 开源。


