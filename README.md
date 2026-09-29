# Palimpsest Windows

Windows-first implementation of the Palimpsest technical design.

## Current architecture

- `src/Palimpsest.App`: C# / WinUI 3 desktop shell.
- `native`: C++20 reusable core.
- `native/tools/paper_inspector`: phase-one CLI/debugger.
- `build/native/bin`: native build output (`paper_inspector.exe`, `palimpsest_core.dll`, `pdfium.dll`).

The native core exposes a narrow C ABI so Android can later reuse the same C++ implementation through JNI without duplicating PaperIR or layout logic.

## Toolchain detected on this machine

- Windows 11 x64
- .NET SDK 8.0.425 / C# Roslyn compiler
- Microsoft WinUI 3 project templates
- CMake 4.4.3
- Ninja 1.13.2
- GCC/G++ 16.2.0 (MSYS2 UCRT64)
- SQLite 3.53.4

Visual Studio/MSVC and a standalone Windows SDK are not currently installed; the WinUI project uses NuGet-delivered Windows App SDK/BuildTools, while the native bootstrap uses the existing MinGW toolchain.

PDFium is pinned under `third_party/pdfium` from the public `bblanchon/pdfium-binaries` Windows x64 release (`chromium/8066`, SHA256 `739a57d597d864297909cc40a2411eba728490c76a0fa25e3ea299c7f6b07020`).

## Build

```powershell
cd D:\project\Palimpsest\windows
.\scripts\build.ps1
```

## Paper Inspector

```powershell
.\scripts\run-inspector.ps1 -Pdf "D:\path\paper.pdf"
```

It writes:

```text
debug/
  page_001_original.png
  page_001_regions.png
  page_001_clean.png
  ...
  paper_ir.json
```

The first-pass semantic classifier is deliberately conservative and heuristic. Its purpose is to expose layout/reading-order mistakes visibly before translation providers are connected.
