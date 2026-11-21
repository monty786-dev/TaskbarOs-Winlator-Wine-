# WinShell – Windows 7 style taskbar & start panel for Winlator

Pure Win32 C, one source file: `src/winshell.c`. Config is stored in `winshell.ini` next to the exe.

## Build the .exe on GitHub (no PC needed, works from a phone browser)
1. Create a GitHub account → **New repository** (e.g. `WinShell`, Public).
2. Upload the project keeping this layout:
   ```
   src/winshell.c
   .github/workflows/build.yml
   README.md
   ```
   (Add file → Upload files. For the hidden `.github` folder use "Create new file" and type the path `.github/workflows/build.yml`.)
3. Commit to `main`. Open the **Actions** tab → "Build WinShell" runs automatically
   (or press **Run workflow**).
4. When it is green, open the run → **Artifacts → WinShell** → download the zip
   containing `WinShell_x64.exe` and `WinShell_x86.exe`.
5. Optional release: Releases → Draft new release → tag `v1.0` → publish
   (or `git tag v1.0 && git push --tags`) and the exes are attached automatically.
6. If the run is red, open the failed step, copy the error text and fix the line it names.

Local build (Linux/Termux/MSYS2):
```
x86_64-w64-mingw32-gcc -O2 -municode -mwindows -static-libgcc -o WinShell_x64.exe src/winshell.c \
  -lgdi32 -luser32 -lshell32 -lole32 -luuid -lcomctl32 -lcomdlg32 -lshlwapi -ladvapi32 -lgdiplus -lkernel32
```

## Use in Winlator
1. Copy `WinShell_x64.exe` (or x86) to e.g. `C:\WinShell\` inside the container (Winlator file manager / drive D: share).
2. Winlator → container settings → **Startup selection: Normal** and run it from the *Wine Menu → Run*,
   or put it in the container's `Startup` folder:
   `C:\ProgramData\Microsoft\Windows\Start Menu\Programs\StartUp\` (shortcut to the exe).
3. Right-click taskbar / tray button → **WinShell Settings**.

## Features
- Taskbar: height, width %, top/bottom, pinned shortcuts (right-click → pin/unpin), running apps, titles on/off.
- Right side: app-tray button (tools menu + toggles), clock/date (en-IN formats, custom format strings,
  fixed UTC offset – default IST +5:30), show-desktop button.
- Start panel: all `.lnk/.exe/.url` from Start Menu + Desktop with icons, live search, profile name & picture,
  folders, Control Panel, Task Manager, power menu.
- Full desktop mode: wallpaper + desktop icons.
- Auto-hide with a **reveal bar** (length %, thickness, position %).
- Windows 7 style title bars: Wine's caption is removed and replaced with a Win7-style bar (min/max/close, drag, double-click maximize).
- Settings window with 4 tabs.

## Known limitations
- Tray icons from apps (Shell_NotifyIcon) are not hosted.
- Title-bar skinning is done externally (style change + companion bar window); some apps with custom frames may misbehave – disable it in Settings.
- Zorin theme is not included (Win7 only).
