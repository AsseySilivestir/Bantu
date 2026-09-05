/**
 * Bantu VSCode Extension v1.2.2
 * Entry point — registers commands, completion provider, hover provider,
 * and the file icon (declared in package.json).
 */

import * as vscode from 'vscode';
import { BantuCompletionProvider } from './completionProvider';
import { BantuHoverProvider } from './hoverProvider';
import { BantuSymbolProvider } from './symbolProvider';
import { BantuTaskProvider } from './taskProvider';
import { BantuDiagnostics } from './diagnosticsProvider';

export function activate(context: vscode.ExtensionContext) {
    const bantuSel: vscode.DocumentSelector = [
        { scheme: 'file', language: 'bantu' }
    ];

    // ─── Completion provider (autocomplete) ───
    const completionProvider = new BantuCompletionProvider();
    context.subscriptions.push(
        vscode.languages.registerCompletionItemProvider(
            bantuSel,
            completionProvider,
            '.', '$', '(', '"', "'"
        )
    );

    // ─── Hover provider (hints) ───
    const hoverProvider = new BantuHoverProvider();
    context.subscriptions.push(
        vscode.languages.registerHoverProvider(bantuSel, hoverProvider)
    );

    // ─── Symbol provider (Go to Symbol) ───
    const symbolProvider = new BantuSymbolProvider();
    context.subscriptions.push(
        vscode.languages.registerDocumentSymbolProvider(bantuSel, symbolProvider)
    );

    // ─── Task provider (Run file) ───
    const taskProvider = new BantuTaskProvider();
    context.subscriptions.push(
        vscode.tasks.registerTaskProvider('bantu', taskProvider)
    );

    // ─── Live diagnostics (linter) ───
    // Runs `bantu lint --json` as you type; errors → red, warnings → yellow.
    const diagnostics = new BantuDiagnostics(context);
    diagnostics.activate();

    // ─── Commands ───
    context.subscriptions.push(
        vscode.commands.registerCommand('bantu.runFile', async () => {
            const editor = vscode.window.activeTextEditor;
            if (!editor || editor.document.languageId !== 'bantu') {
                vscode.window.showWarningMessage('Open a .b file to run.');
                return;
            }
            const interpreter = vscode.workspace.getConfiguration('bantu')
                .get<string>('interpreterPath', 'bantu');
            const filePath = editor.document.uri.fsPath;

            const task = new vscode.Task(
                { type: 'bantu', target: 'run' },
                vscode.TaskScope.Workspace,
                'Bantu: Run',
                'bantu',
                new vscode.ShellExecution(`${interpreter} run "${filePath}"`),
                ['$bantu']
            );
            task.presentationOptions = {
                reveal: vscode.TaskRevealKind.Always,
                panel: vscode.TaskPanelKind.Dedicated,
                showReuseMessage: false,
                clear: true
            };
            await vscode.tasks.executeTask(task);
        }),

        vscode.commands.registerCommand('bantu.initProject', async () => {
            const name = await vscode.window.showInputBox({
                prompt: 'Project name',
                placeHolder: 'myproject'
            });
            if (!name) return;
            const terminal = vscode.window.createTerminal('Bantu init');
            terminal.show();
            terminal.sendText(`bantu init ${name}`);
        }),

        vscode.commands.registerCommand('bantu.initWebProject', async () => {
            const name = await vscode.window.showInputBox({
                prompt: 'Web project name',
                placeHolder: 'myapp'
            });
            if (!name) return;
            const terminal = vscode.window.createTerminal('Bantu init --web');
            terminal.show();
            terminal.sendText(`bantu init --web ${name}`);
        }),

        vscode.commands.registerCommand('bantu.buildWindows', async () => {
            const name = await vscode.window.showInputBox({
                prompt: 'Installer app name',
                value: 'BantuApp'
            });
            const version = await vscode.window.showInputBox({
                prompt: 'App version',
                value: '1.0.0'
            });
            const terminal = vscode.window.createTerminal('Bantu build-windows');
            terminal.show();
            terminal.sendText(
                `bantu build-windows --name "${name || 'BantuApp'}" --version "${version || '1.0.0'}"`
            );
        }),

        vscode.commands.registerCommand('bantu.bench', () => {
            const terminal = vscode.window.createTerminal('Bantu bench');
            terminal.show();
            terminal.sendText('bantu bench');
        })
    );

    console.log('[Bantu] Extension activated (v1.2.2).');
}

export function deactivate() {
    console.log('[Bantu] Extension deactivated.');
}
