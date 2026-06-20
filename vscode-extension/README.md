# Bantu — VSCode Extension (v1.2.2)

First-class VSCode support for the [Bantu](https://github.com/AsseySilivestir/Bantu) programming language.

## Features

- **Syntax highlighting** for `.b` files (keywords, types, strings, comments, sua framework)
- **Blue-B file icon** — every `.b` file in the explorer gets the Bantu blue "B" icon (light + dark variants)
- **Autocomplete** — context-aware completions for:
  - Language keywords (`if`, `def`, `class`, `include`, …)
  - Type annotations (`number`, `string`, `bool`, `list`, `dict`, `any`, `func`)
  - `sua.*` namespaces (`sua.server`, `sua.sqlite`, `sua.postgres`, `sua.mysql`, `sua.webrtc`, …)
  - Method suggestions after `sua.<ns>.`
  - Variable name suggestions after `$`
- **Hover hints** — hover over any keyword or `sua.*` API to see its signature and short docs
- **Go to Symbol** (`Ctrl+Shift+O`) — jump to any `def`, `class`, `include`, or top-level variable
- **Snippets** — `def`, `class`, `if`, `each`, `include`, `sua.server`, `sua.sqlite`, `sua.postgres`, `sua.mysql`, `sua.webrtc.peer`, …
- **Commands**
  - `Bantu: Run File` (`F5`) — run the current `.b` file with the interpreter
  - `Bantu: Initialize Project` — `bantu init <name>`
  - `Bantu: Initialize Sua Web Project` — `bantu init --web <name>`
  - `Bantu: Build Windows Installer` — `bantu build-windows --name … --version …`
  - `Bantu: Run Benchmarks` — `bantu bench`

## Install

### From VSIX
```bash
cd vscode-extension
npm install
npm run compile
npx vsce package
code --install-extension bantu-vscode-1.2.1.vsix
```

### From source (dev)
```bash
cd vscode-extension
npm install
npm run compile
# Press F5 in VSCode to launch an Extension Development Host
```

## Configuration

| Setting | Default | Description |
|---|---|---|
| `bantu.interpreterPath` | `bantu` | Path to the `bantu` interpreter. Override if it's not on PATH. |
| `bantu.enableLanguageServer` | `true` | Enable hover + autocomplete providers. |
| `bantu.formatOnSave` | `false` | Reserved for future formatter integration. |

## File Icon

Every `.b` file in VSCode's explorer gets the Bantu blue "B" icon. The icon ships in two variants (light + dark theme-aware) and is declared via `contributes.fileIcons` in `package.json` plus `contributes.languages[].icon`.

## Language Server Roadmap

The current extension implements completion + hover in-process. A full LSP server (with diagnostics, jump-to-definition, rename refactoring) is on the v1.3 roadmap.
