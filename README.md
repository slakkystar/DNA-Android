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

By default the prebuilt `assets/tools/dna` is packaged. To compile it from
`assets/tools/dna.c` and omit the source from the APK:

```bash
./gradlew assembleRelease -PdnaFromSource=true
```

The repository keeps both the prebuilt binary and the source; they are only
changed inside the staged build assets. An Android NDK is required for this mode.
Alternatively pass `-PdnaCc=/path/to/aarch64-linux-android-clang` or put such a
compiler on `PATH` (for example, a Termux Android clang wrapper).

Source builds also include xz-embedded (0BSD) and decode `REPLACE_XZ` payload
operations in-process. Payload extraction uses a worker pool sized from RAM:
8 workers at 5.5 GiB+ `MemTotal`, 4 at 3 GiB+, 2 below that, dropping to 1 on
very low RAM; the count is capped by CPU count. Set
`DNA_PAYLOAD_WORKERS=<1-8>` to override it. Bzip2, Brotli and Zstd operations
still use the bundled helpers through non-blocking pipes.

To build the superchunk helpers from source instead of using the prebuilt
`merge_superchunk`, `seprate_superchunk` and `chunk_split`:

```bash
./gradlew assembleRelease -PsuperchunkFromSource=true
```

This stages `merge_superchunk.sh` and `seprate_superchunk.sh` as executable
helpers and compiles `chunk_split.c` into `chunk_split`. The source files are
not packaged in either mode. Without the flag the prebuilt binaries are used.

`assets/tools/patch_vbmeta` is also staged from `assets/tools/patch_vbmeta.sh`
by default; the shell source replaces the shc-compiled prebuilt inside the APK.
To keep the original prebuilt instead:

```bash
./gradlew assembleRelease -PpatchVbmetaFromSource=false
```

The shell source is not packaged in either mode.

To replace the bundled f2fs helpers with clean universal builds inside the APK:

```bash
./gradlew assembleRelease -Pf2fsClean=true
```

This stages `assets/tools-f2fs` over `assets/tools`: a clean `extract.f2fs`
wrapper backed by the static `f2fs-extractor`, and an AOSP `sload_f2fs` built
with explicit `-s` file_contexts and `-t` mount-point handling. Its shared
libraries (including `libandroid-selinux`) are bundled via `$ORIGIN`. The
original assets are not modified. The clean extractor handles uncompressed
f2fs images; the bundled extractor remains available when `f2fsClean` is not
enabled.

The GitHub Actions workflows expose `dna_from_source`, `superchunk_from_source`,
`f2fs_clean` and `upload_to_telegram` inputs. Telegram upload requires the
`TELEGRAM_TOKEN` and `TELEGRAM_CHAT_ID` secrets and prefers the signed APK when
one is built.

## 📄 License

See the `LICENSE` file in the repository root.

