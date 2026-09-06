# Windows Installer Reference (v1.2.1)

This directory holds reference material for the `bantu build-windows` command. The command itself generates the NSIS script at `./dist/installer.nsi` in your project — you don't need to copy anything from here.

## What `bantu build-windows` does

1. Resolves your entry file (defaults to `main.b` / `app.b` / `index.b`)
2. Creates `./dist/`
3. Writes `./dist/installer.nsi` (NSIS script)
4. Invokes `makensis dist/installer.nsi`
5. Outputs `./dist/<AppName>-Setup-<Version>.exe`

## Prerequisites

You need NSIS (Nullsoft Scriptable Install System) on PATH:

| OS | Install command |
|---|---|
| Linux (Debian/Ubuntu) | `sudo apt install nsis` |
| macOS (Homebrew) | `brew install nsis` |
| Windows | Download from https://nsis.sourceforge.io/Download |

## Usage

```bash
# Defaults: AppName=BantuApp, Version=1.0.0, entry=main.b
bantu build-windows

# Custom name + version
bantu build-windows --name "MyBlog" --version "2.1.0"

# Custom entry file
bantu build-windows app.b --name "Shop" --version "1.0.0"
```

## Customizing the installer

After `bantu build-windows` writes `dist/installer.nsi`, you can edit it by hand to add:
- Registry entries (file associations, app paths)
- Custom install pages (license agreement, components)
- Desktop shortcuts
- File type associations (e.g. associate `.bantuapp` with your app)
- Post-install actions (run a config script, download assets, etc.)

Then re-run `makensis dist/installer.nsi` to regenerate the installer.

## Example: full customization

```nsi
; Custom installer.nsi excerpt — add after the default Section

; Desktop shortcut
CreateShortcut "$DESKTOP\\${APPNAME}.lnk" \\
    "$INSTDIR\\run-${APPNAME}.bat" "" "$INSTDIR\\bantu.exe"

; File association
${RegisterExtension} "$INSTDIR\\run-${APPNAME}.bat" ".myapp" "${APPNAME} File"

; Auto-start on login (optional)
CreateShortcut "$SMSTARTUP\\${APPNAME}.lnk" \\
    "$INSTDIR\\run-${APPNAME}.bat"
```

## See also

- [`docs/Bantu-Programming-Language-v1.2.1.pdf`](../docs/Bantu-Programming-Language-v1.2.1.pdf) — Chapter 9 covers `bantu build-windows` in detail
- [NSIS documentation](https://nsis.sourceforge.io/Docs/) — full reference for `.nsi` scripts
