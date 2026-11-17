# VibeBackup

VibeBackup saves ZIP backups of a project folder so you can return to an earlier version of your files.

## Download

[Download VibeBackup.exe for Windows](https://github.com/ricekitti/VibeBackup-CPP/releases/latest/download/VibeBackup.exe)

## How to use it

1. Open VibeBackup and choose a project folder.
2. Click **Back up now** to save a snapshot.
3. To recover files, select a snapshot and click **Restore selected**. The app restores it into a new folder beside your project and leaves the original folder alone.

You can also turn on **Auto-backup**. While the app is open, it checks every five minutes and saves a snapshot when files have changed.

Backups are stored in `%LOCALAPPDATA%\VibeBackup\snapshots`. The app skips common generated folders such as `.git`, `node_modules`, `build`, and `dist`.

## Build from source

On Windows, install Visual Studio with C++ desktop tools and CMake, then run:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

The executable will be at `build\Release\VibeBackup.exe`.

## License

MIT. See [LICENSE](LICENSE).