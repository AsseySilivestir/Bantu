/**
 * diagnosticsProvider.ts — live linting for Bantu files.
 *
 * Runs `bantu lint <file> --json` as the user types (debounced) and on save,
 * then publishes the results as VS Code diagnostics: errors show a red squiggle,
 * warnings a yellow one. The interpreter's compile gate refuses to `run`/`build`
 * files that still contain errors, so the editor and the toolchain agree.
 */

import * as vscode from 'vscode';
import { execFile } from 'child_process';
import * as fs from 'fs';
import * as os from 'os';
import * as path from 'path';

interface LintDiag {
    line: number;
    col: number;
    severity: string;   // "error" | "warning"
    message: string;
}

export class BantuDiagnostics {
    private collection: vscode.DiagnosticCollection;
    private timers = new Map<string, NodeJS.Timeout>();

    constructor(private context: vscode.ExtensionContext) {
        this.collection = vscode.languages.createDiagnosticCollection('bantu');
        context.subscriptions.push(this.collection);
    }

    activate(): void {
        const sub = this.context.subscriptions;
        sub.push(vscode.workspace.onDidOpenTextDocument((d) => this.lintNow(d)));
        sub.push(vscode.workspace.onDidSaveTextDocument((d) => this.lintNow(d)));
        sub.push(vscode.workspace.onDidChangeTextDocument((e) => this.debounce(e.document)));
        sub.push(vscode.workspace.onDidCloseTextDocument((d) => this.collection.delete(d.uri)));
        // Lint anything already open.
        vscode.workspace.textDocuments.forEach((d) => this.lintNow(d));
    }

    private debounce(doc: vscode.TextDocument): void {
        if (doc.languageId !== 'bantu') return;
        const key = doc.uri.toString();
        const prev = this.timers.get(key);
        if (prev) clearTimeout(prev);
        this.timers.set(key, setTimeout(() => this.lintNow(doc), 300));
    }

    private lintNow(doc: vscode.TextDocument): void {
        if (doc.languageId !== 'bantu') return;

        const interpreter = vscode.workspace
            .getConfiguration('bantu')
            .get<string>('interpreterPath', 'bantu');

        // Lint the CURRENT (possibly unsaved) buffer by writing it to a temp
        // file, so diagnostics update live while typing — not only on save.
        const hash = Buffer.from(doc.uri.toString()).toString('hex').slice(0, 16);
        const tmp = path.join(os.tmpdir(), `bantu-lint-${hash}.b`);
        try {
            fs.writeFileSync(tmp, doc.getText());
        } catch {
            return;
        }

        execFile(interpreter, ['lint', tmp, '--json'], { timeout: 5000 }, (_err, stdout) => {
            let diags: LintDiag[] = [];
            try {
                diags = JSON.parse((stdout || '').trim() || '[]');
            } catch {
                diags = [];
            }

            const out: vscode.Diagnostic[] = diags.map((d) => {
                const line = Math.max(0, (d.line || 1) - 1);
                const col = Math.max(0, (d.col || 1) - 1);
                const range = new vscode.Range(line, col, line, col + 1);
                const severity =
                    d.severity === 'warning'
                        ? vscode.DiagnosticSeverity.Warning
                        : vscode.DiagnosticSeverity.Error;
                const diag = new vscode.Diagnostic(range, d.message, severity);
                diag.source = 'bantu';
                return diag;
            });

            this.collection.set(doc.uri, out);
            try {
                fs.unlinkSync(tmp);
            } catch {
                /* ignore */
            }
        });
    }
}
