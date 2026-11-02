A Windows editor

## Building

Requirements: Windows with Visual Studio (2022 / v143 toolset) including the "Desktop development with C++" workload.

Run from the repository root:

```
build.bat            # Release build (default)
build.bat Debug      # Debug build
```

The script locates Visual Studio with `vswhere`, sets up the x64 build environment and runs MSBuild on `editor.vcxproj`.

The binary is written to `bin\Release\editor.exe` (or `bin\Debug\editor.exe`). Close any running instance before rebuilding, otherwise the linker fails with `LNK1104` because the exe is locked.
