===============================================================
Bnlang Runtime
===============================================================

Version:   1.0.0
Publisher: Bnlang | Mamun
Website:   https://bnlang.dev

---------------------------------------------------------------
Overview
---------------------------------------------------------------
Bnlang is a bilingual programming language and runtime designed
for developers in Bangladesh and beyond. It accepts Bangla and
English keywords side by side, so the same program can be
written in either language without sacrificing modern
engineering features.

This package includes the Bnlang runtime (bnl executable), the
BPM package manager, and supporting tools. It is provided free
of charge in binary form.

---------------------------------------------------------------
System Requirements
---------------------------------------------------------------
- Windows 10 or later (x64 and x86 builds available)
- macOS 12.0 or later (Apple Silicon arm64)
- Linux kernel 4.15 or later (libstdc++ / libgcc are linked
  statically, so any modern glibc works — no specific minimum
  version required for typical distributions)
- Minimum 2 GB RAM
- 200 MB free disk space

---------------------------------------------------------------
Installation
---------------------------------------------------------------
Windows:
  1. Run the installer (Bnlang-Setup-x64.exe or Bnlang-Setup-x86.exe).
  2. Follow the on-screen instructions.
  3. The installer will add Bnlang to your system PATH.
  4. Verify installation:
       bnl --version

Linux:
  1. Extract the archive (e.g., tar -xzf bnlang-v1.0.0-linux-x64.tar.gz).
  2. Move the 'bnl' binary somewhere on your PATH (e.g., /usr/local/bin).
  3. Make it executable if necessary:
       chmod +x bnl
  4. Verify installation:
       bnl --version

macOS:
  1. Mount the DMG or extract the archive.
  2. Copy 'bnl' into /usr/local/bin or another directory on your PATH.
  3. On first run you may need to approve execution in System Settings
     (Gatekeeper prompt).
  4. Verify installation:
       bnl --version

---------------------------------------------------------------
Getting Started
---------------------------------------------------------------
- To run a Bnlang program:

   bnl myfile.bnl

- To explore documentation and examples:
  Visit https://bnlang.dev/docs

- To install packages with BPM:

   bpm install <package-name>

---------------------------------------------------------------
Uninstallation
---------------------------------------------------------------
Windows:
  1. Open "Apps & Features" in Windows Settings.
  2. Locate "Bnlang" in the list.
  3. Click "Uninstall" and follow the prompts.

Linux/macOS:
  1. Delete the 'bnl' binary from your PATH.
  2. Remove any related files such as ~/.bnlang and ~/.bpm if present.

---------------------------------------------------------------
License
---------------------------------------------------------------
Bnlang binaries are free to use for personal, educational, and
commercial projects. Source code remains private property of the
authors. See LICENSE.txt for full details, including third-party
licenses (fmt, libuv, llhttp, nlohmann-json, OpenSSL, SQLite).

---------------------------------------------------------------
Support
---------------------------------------------------------------
- Website:  https://bnlang.dev
- Docs:     https://bnlang.dev
- Issues:   https://github.com/bnlang/issues
- Email:    support@bnlang.dev

---------------------------------------------------------------
Credits
---------------------------------------------------------------
Bnlang is built on top of proven open source components
including fmt, libuv, llhttp, nlohmann-json, OpenSSL, and
SQLite. See LICENSE.txt for full acknowledgments and license
texts of these projects.

The Bnlang runtime itself is distributed as free binaries. The
main source code is currently private but is planned to be made
open source in the future.

===============================================================
Thank you for installing Bnlang!
===============================================================
