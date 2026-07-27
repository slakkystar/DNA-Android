<div align="center">

# DNA-Android

**A framework for building tools that work with ROOTed devices, using `XML + Shell`**

</div>

---

## 📖 About

**DNA-Android** is powered by the **krscript** engine, which lets you quickly build tools for ROOTed devices by describing the logic through `XML` markup and `shell` scripts — no need to write or compile any Java/Kotlin code.

The app ships with a ready-made set of guides and auto-scripts for **porting custom ROMs** (ColorOS, HyperOS, OriginOS, and more) between devices.

## ✨ Features

- 🛠 **krscript engine** — describe UI and logic through `assets/*.xml`
- 📜 **Built-in porting guides** — step-by-step instructions right inside the app
- ⚡ **Auto-port scripts** — automate the routine porting steps (build.prop, phh_init, apex, etc.)
- 📦 **Image handling** — unpack/repack `system`, `system_ext`, `product`, `vendor`, `odm` and more
- 🚫 **No APK rebuild needed** — new tools are added as static files in `assets`, without touching the app's code

## 🖼 Interface

Before you start, it's worth taking a look at the interface first. Install the latest build and try it out on a ROOT-enabled device or emulator.

## 📥 Installation

Download the latest release here:

➡️ **[Releases](https://github.com/slakkystar/DNA-Android/releases)**

## 🧩 Requirements

- Android 5.0+ (API 21) or higher
- Root access on the device (required for most features)

## 🏗 Project Structure

```
DNA-Android/
├── pio/            # Main app module (UI, activities)
├── krscript/        # krscript engine (XML parsing, shell execution)
├── common/         # Shared components
└── pio/src/main/assets/kr-script/
    ├── instructions/   # XML guides (ColorOS, HyperOS, OriginOS, etc.)
    └── samples/        # Auto-port shell scripts
```

## 🔧 Building from Source

```bash
git clone https://github.com/slakkystar/DNA-Android.git
cd DNA-Android
./gradlew assembleRelease
```

The built APK will be located at `pio/build/outputs/apk/release/`.

## 📄 License

See the `LICENSE` file in the repository root.

