/**
 * BantuTaskProvider — wires `bantu run` into VSCode's task system so the
 * "Run" button (F5) launches the current .b file with the Bantu interpreter.
 */

import * as vscode from 'vscode';

export class BantuTaskProvider implements vscode.TaskProvider {
    static taskType = 'bantu';

    provideTasks(): vscode.ProviderResult<vscode.Task[]> {
        const editor = vscode.window.activeTextEditor;
        if (!editor || editor.document.languageId !== 'bantu') return [];

        const interpreter = vscode.workspace.getConfiguration('bantu')
            .get<string>('interpreterPath', 'bantu');
        const filePath = editor.document.uri.fsPath;

        const task = new vscode.Task(
            { type: BantuTaskProvider.taskType, target: 'run' },
            vscode.TaskScope.Workspace,
            'Bantu: Run current file',
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
        return [task];
    }

    resolveTask(task: vscode.Task): vscode.ProviderResult<vscode.Task> {
        return task;
    }
}
