/**
 * BantuSymbolProvider — exposes top-level def/class/include declarations
 * to VSCode's "Go to Symbol" panel (Ctrl+Shift+O).
 */

import * as vscode from 'vscode';

const DEF_RE     = /^\s*def\s+([A-Za-z_][A-Za-z0-9_]*)/;
const CLASS_RE   = /^\s*class\s+([A-Za-z_][A-Za-z0-9_]*)/;
const INCLUDE_RE = /^\s*include\s+"([^"]+)"/;
const CONST_RE   = /^\s*(?:const\s+)?\$([A-Za-z_][A-Za-z0-9_]*)\s*=/;

export class BantuSymbolProvider implements vscode.DocumentSymbolProvider {
    provideDocumentSymbols(
        document: vscode.TextDocument,
        token: vscode.CancellationToken
    ): vscode.ProviderResult<vscode.DocumentSymbol[]> {
        const symbols: vscode.DocumentSymbol[] = [];

        for (let i = 0; i < document.lineCount; i++) {
            const line = document.lineAt(i);
            const text = line.text;

            const def = text.match(DEF_RE);
            if (def) {
                symbols.push(new vscode.DocumentSymbol(
                    def[1], 'function',
                    vscode.SymbolKind.Function,
                    line.range, line.range
                ));
                continue;
            }
            const cls = text.match(CLASS_RE);
            if (cls) {
                symbols.push(new vscode.DocumentSymbol(
                    cls[1], 'class',
                    vscode.SymbolKind.Class,
                    line.range, line.range
                ));
                continue;
            }
            const inc = text.match(INCLUDE_RE);
            if (inc) {
                symbols.push(new vscode.DocumentSymbol(
                    'include ' + inc[1], 'module',
                    vscode.SymbolKind.Module,
                    line.range, line.range
                ));
                continue;
            }
            const cnst = text.match(CONST_RE);
            if (cnst) {
                symbols.push(new vscode.DocumentSymbol(
                    '$' + cnst[1], 'variable',
                    vscode.SymbolKind.Variable,
                    line.range, line.range
                ));
            }
        }
        return symbols;
    }
}
