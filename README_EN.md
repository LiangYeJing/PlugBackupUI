# PlugBackup — Automatic Backup to Local/External Drives

> A cross-platform **Qt/C++** tool that automatically backs up folders to an external drive/NAS, with **deduplication, verification, versioning, soft-delete retention, and one-click restore**.
>  Great for individuals and small teams, and a solid building block for **in-company file sharing/archiving**.

## ✨ Features

- **Select multiple source folders + one destination** (external drive/NAS mount)
  - Safety checks: destination must be writable, not inside any source, sources must not contain each other
- **Auto triggers**
  - **Interval-based** backups (configurable minutes)
  - **After “file closed / stable window”** (avoid copying while files are being written)
  - **Device online check** every *N* minutes; resumes automatically when back online
- **Smart mode (optional)**: auto-pause on high system load, resume when idle (CPU threshold & polling interval)
- **Dedup + verification**
  - Content-hash deduplication
  - Post-copy verification; auto retries; `.part` temp files are cleaned up on failure
- **Versioning & soft-delete retention with restore**
  - Previous versions in `.plugbackup_meta/versions`
  - Deleted files in `.plugbackup_meta/deleted`
  - **One-click restore** back to the original path (and keep a copy in destination if needed)
  - **Retention days** configurable; old versions get purged automatically
- **Ignore rules (glob)** like `*.tmp; node_modules/*; *.log`
- **Speed limit** in MB/s (optional)
- **Progress board** per source: speed/ETA/state, with **pause/resume/cancel** per row
- **Unplug-safe**: detects offline status, pauses, shows non-modal warning, and resumes on reconnect

------

## 🖥 Platform & Build

- C++17, Qt 6.x (Core/Gui/Widgets), CMake ≥ 3.16
- Windows (tested with MinGW); macOS/Linux expected to work with Qt6

### Build (CLI)

```
cmake -S . -B build -DCMAKE_PREFIX_PATH="/path/to/Qt/6.x.x/<kit>"
cmake --build build -j
```

### Build (Qt Creator)

Open the CMake project with a Qt 6 kit and Run.

------

## 🧑‍💻 Usage

1. Add one or more **source** folders; choose a **destination** on your external drive/NAS
2. Configure **ignore patterns**, **auto triggers** (interval/stable window/device check), and **advanced options** (retention/speed limit/smart mode)
3. Click **Start Backup** or wait for the automatic trigger
4. If unplugged, the app pauses and warns; it resumes when the device is back
5. Use the **Versions & Deleted** panel to **scan** and **restore** items

> **Note**: Never place the destination inside any source folder to avoid recursive copies.

------

## 📁 Destination Layout

```
<dest>/
  ├─ <source1>/
  ├─ <source2>/
  └─ .plugbackup_meta/
       ├─ versions/
       └─ deleted/
         (each data file comes with a .json metadata: origAbs/rel/srcRoot, etc.)
```

------

## 🔐 Safety

- Uses `.part` temp files and final rename after verification (atomicity)
- Fails fast and cleans up partial files
- **No writes** when destination is offline (workers pause & notify)

------

## 🗺 Roadmap (Great for Teams)

- **Company use**
  - Better SMB/NFS/NAS support & robust resume
  - **Team spaces** with shared index, permissions, and audit logs
  - **Cross-user dedup** (global content addressing)
  - Exportable audit report (CSV/HTML)
- **Service & Platform**
  - **Daemon/service** + tray UI
  - **Web dashboard** for multi-client monitoring, alerts, analytics
  - CLI & JSON-RPC/gRPC control
- **Advanced storage**
  - Chunking (CDC/fixed) + **object storage** backends (S3/OSS/OBS/MinIO)
  - **End-to-end encryption** (optional zero-knowledge)
  - Rich include/exclude rules; conflict handling & locking
- **UX & Engineering**
  - Dark theme, i18n (tr() ready), collapsible panels
  - Installers (MSI/DMG/AppImage)
  - Unit & integration tests

------

## 🤝 Contributing

- File **Issues** with clear repro steps and scenarios
- Create branches: `feature/<topic>` / `fix/<topic>`
- Commit style (suggested): `feat: ...`, `fix: ...`, `refactor: ...`, `docs: ...`
- Before PR:
  1. Build passes locally (at least on Windows + Qt6)
  2. Follow code style (ClangFormat/Clang-Tidy optional)
  3. Describe changes, risks, and test plan

------

## 📄 License

(Choose one and add a `LICENSE` file: **MIT** / **Apache-2.0** / **GPLv3**.)
 *Example*: Licensed under **MIT**.

------

## 🙏 Thanks

Star the repo, open Issues, and send PRs!
 If you adopt this inside your company, please share your experience and best practices.