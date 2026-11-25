DLSS5 MFC - NRLive controls migration
========================================

The attached OpenNR-Installer 0.9.9 source was used as the reference for the NRLive page's settings:
- NrLiveConfig.cs (INI sections/keys/defaults/ranges and HUD preset behavior)
- NrLiveProfile.cs (profile JSON schema and NRLive command-line arguments)
- NrLivePage.Settings.cs / Rows.cs (the three settings groups and their control types)

Only the screenshot-related functions were carried into this MFC page:
1. Session target mode (foreground window picker), countdown, admin launch, start/stop.
2. Scale/stop/overlay shortcut recording, validation and profile persistence; only ScaleHotkey is globally registered by this app. StopKey and OverlayKey are passed to NRLive, matching the original launcher behavior. Bind Bypass is editable.
3. Capture mode (WGC/DXGI) and motion vector (FastMV/AMD optical flow).
4. Three vertical tabs in a tall borderless window:
   - Magnification & frame generation: upscaler, sharpness, FG, frame limiter.
   - Neural rendering: NR enable, original model, style, strength, passes, selected advanced controls.
   - Overlay/HUD: enable, preset, position, HUD item switches, preview and scale.
5. scaleconfig.ini and profiles\Default.json persistence.

Excluded: installer package downloads, release/update UI, game library, anti-cheat cards, ReShade and unrelated installer pages.

Runtime integration:
- This MFC app is a settings front-end/launcher, not the NR/FG renderer.
- It writes NRLive-compatible settings to scaleconfig.ini and launches NRLive.exe if found.
- NRLive runtime and configuration path: %LOCALAPPDATA%\DLSS5Console\Cache\nrlive\nrlive. NRLive.exe is located recursively under this directory; scaleconfig.ini and profiles are read/written beside the discovered executable.
- If no NRLive.exe is found, the UI still works and saves settings, but Start reports the missing runtime.

Build:
powershell -NoProfile -ExecutionPolicy Bypass -File .\BUILD.ps1

Output:
bin\x64\Release\DLSS5_MFC_UI.exe

神经渲染移植说明：请同时阅读 NEURAL_RENDERING_MIGRATION_AUDIT.txt。当前版本覆盖原版 NRLive 配置表的 79 个 OpenNR / DLSSNR Bridge 设置行，包含独立分组折叠、命名下拉选项、依赖禁用、精确小数倍率和布尔值解析。尚需在 Windows 11 + VS2022 上编译并用实际 NRLive 版本验证。
