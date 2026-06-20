/**
 * BantuCompletionProvider — context-aware autocomplete for the Bantu language.
 *
 * Provides:
 *  - Keyword suggestions (if, def, class, include, etc.)
 *  - Type annotations (number, string, bool, list, dict, any, func)
 *  - Sua framework namespaces (sua.server, sua.sqlite, sua.postgres,
 *    sua.mysql, sua.json, sua.http, sua.webrtc, sua.include)
 *  - Method suggestions when typing after `sua.<ns>.`
 *  - Snippet items for common idioms
 */

import * as vscode from 'vscode';

const KEYWORDS: { name: string; snippet?: string; doc: string }[] = [
    { name: 'if',        snippet: 'if (${1:condition}) {\n\t${2:// body}\n}',                       doc: 'Conditional branch' },
    { name: 'else',      snippet: 'else {\n\t${1:// body}\n}',                                     doc: 'Else branch' },
    { name: 'while',     snippet: 'while (${1:condition}) {\n\t${2:// body}\n}',                    doc: 'While loop' },
    { name: 'for',       snippet: 'for ($${1:i} = 0; $${1:i} < ${2:10}; $${1:i} += 1) {\n\t${3:// body}\n}', doc: 'C-style for loop' },
    { name: 'each',      snippet: 'each ($${1:item} in ${2:list}) {\n\t${3:// body}\n}',            doc: 'Iterate over a list' },
    { name: 'def',       snippet: 'def ${1:name}(${2:args}) {\n\t${3:// body}\n\treturn ${4:null};\n}', doc: 'Function definition' },
    { name: 'return',    snippet: 'return ${1:value};',                                            doc: 'Return from function' },
    { name: 'class',     snippet: 'class ${1:Name} {\n\tpublic def ${2:method}() {\n\t\t${3:// body}\n\t}\n}', doc: 'Class definition' },
    { name: 'try',       snippet: 'try {\n\t${1:// risky}\n} catch {\n\t${2:// handler}\n}',        doc: 'Try-catch block' },
    { name: 'include',   snippet: 'include "./${1:module}.b"${2: as ${3:alias}};',                  doc: 'Module include (v1.2.2)' },
    { name: 'import',    snippet: 'import ${1:module};',                                           doc: 'Import (alias of include)' },
    { name: 'print',     snippet: 'print(${1:"hello"});',                                          doc: 'Print to stdout' },
    { name: 'const',     snippet: 'const $${1:NAME} = ${2:value};',                                doc: 'Constant declaration' },
    { name: 'new',       snippet: 'new ${1:Class}(${2:args})',                                     doc: 'Instantiate a class' },
];

const TYPES: { name: string; doc: string }[] = [
    { name: 'number', doc: '64-bit floating-point numeric type' },
    { name: 'string', doc: 'UTF-8 string type' },
    { name: 'bool',   doc: 'Boolean type (true/false)' },
    { name: 'list',   doc: 'Ordered list (array)' },
    { name: 'dict',   doc: 'Key-value dictionary (object)' },
    { name: 'any',    doc: 'Any dynamic type' },
    { name: 'func',   doc: 'Function reference' },
];

const SUA_NAMESPACES: { name: string; doc: string }[] = [
    { name: 'server',   doc: 'sua.server — Express-like HTTP server (GET/POST/PUT/DELETE/PATCH/HEAD/OPTIONS)' },
    { name: 'http',     doc: 'sua.http — HTTP client (libcurl-backed)' },
    { name: 'sqlite',   doc: 'sua.sqlite — Embedded SQLite driver (connect/exec/query/close)' },
    { name: 'postgres', doc: 'sua.postgres — PostgreSQL driver (libpq-backed when available)' },
    { name: 'mysql',    doc: 'sua.mysql — MySQL/MariaDB driver (mysqlclient-backed when available)' },
    { name: 'json',     doc: 'sua.json — JSON parse/stringify' },
    { name: 'webrtc',   doc: 'sua.webrtc — WebRTC peer/data-channel API (libdatachannel-backed when available, v1.2.2)' },
    { name: 'include',  doc: 'sua.include(path) — Runtime module loader (v1.2.2)' },
    { name: 'channel',  doc: 'sua.channel(name) — Real-time channel primitive' },
    { name: 'stun',     doc: 'sua.stun() — STUN NAT discovery' },
    { name: 'broadcast',doc: 'sua.broadcast(channel, message)' },
    { name: 'signal',   doc: 'sua.signal(target) — WebRTC signaling primitive' },
];

const SUA_METHODS: Record<string, { name: string; snippet?: string; doc: string }[]> = {
    server: [
        { name: 'get',    snippet: 'get("/${1:path}", def($req, $res) {\n\t${2:// handler}\n});',           doc: 'Register GET route' },
        { name: 'post',   snippet: 'post("/${1:path}", def($req, $res) {\n\t${2:// handler}\n});',          doc: 'Register POST route' },
        { name: 'put',    snippet: 'put("/${1:path}", def($req, $res) {\n\t${2:// handler}\n});',           doc: 'Register PUT route' },
        { name: 'delete', snippet: 'delete("/${1:path}", def($req, $res) {\n\t${2:// handler}\n});',        doc: 'Register DELETE route' },
        { name: 'patch',  snippet: 'patch("/${1:path}", def($req, $res) {\n\t${2:// handler}\n});',         doc: 'Register PATCH route' },
        { name: 'static', snippet: 'static("${1:./public}");',                                              doc: 'Serve static files from directory' },
        { name: 'use',    snippet: 'use(def($req, $res, $next) {\n\t${2:// middleware}\n});',               doc: 'Register middleware' },
        { name: 'listen', snippet: 'listen(${1:3000});',                                                    doc: 'Start HTTP server on port' },
        { name: 'routes', snippet: 'routes();',                                                             doc: 'List registered routes' },
    ],
    http: [
        { name: 'get',    snippet: 'get("${1:https://example.com}")',                  doc: 'HTTP GET request' },
        { name: 'post',   snippet: 'post("${1:url}", "${2:body}")',                    doc: 'HTTP POST request' },
        { name: 'put',    snippet: 'put("${1:url}", "${2:body}")',                     doc: 'HTTP PUT request' },
        { name: 'delete', snippet: 'delete("${1:url}")',                               doc: 'HTTP DELETE request' },
        { name: 'patch',  snippet: 'patch("${1:url}", "${2:body}")',                   doc: 'HTTP PATCH request' },
        { name: 'head',   snippet: 'head("${1:url}")',                                 doc: 'HTTP HEAD request' },
    ],
    sqlite: [
        { name: 'connect', snippet: 'connect("${1:app.db}")',                          doc: 'Open SQLite database file' },
        { name: 'exec',    snippet: 'exec("${1:CREATE TABLE ...}")',                   doc: 'Execute SQL (no return)' },
        { name: 'query',   snippet: 'query("${1:SELECT * FROM ...}")',                 doc: 'Run SELECT, return rows' },
        { name: 'close',   snippet: 'close()',                                         doc: 'Close the connection' },
    ],
    postgres: [
        { name: 'connect', snippet: 'connect("host=${1:localhost} dbname=${2:app} user=${3:postgres} password=${4:secret}")', doc: 'Open PostgreSQL connection' },
        { name: 'query',   snippet: 'query("${1:SELECT * FROM users}")',               doc: 'Run SQL, return rows' },
        { name: 'close',   snippet: 'close()',                                         doc: 'Close PostgreSQL connection' },
    ],
    mysql: [
        { name: 'connect', snippet: 'connect("${1:localhost}", "${2:root}", "${3:pw}", "${4:db}", 3306)', doc: 'Open MySQL connection' },
        { name: 'query',   snippet: 'query("${1:SELECT * FROM users}")',               doc: 'Run SQL, return rows' },
        { name: 'close',   snippet: 'close()',                                         doc: 'Close MySQL connection' },
    ],
    json: [
        { name: 'parse',     snippet: 'parse("${1:{\\\"k\\\":1}}")',                   doc: 'Parse JSON string to dict' },
        { name: 'stringify', snippet: 'stringify(${1:$obj})',                          doc: 'Serialize value to JSON' },
    ],
    webrtc: [
        { name: 'peer',            snippet: 'peer("${1:alice}")',                      doc: 'Create a WebRTC peer (v1.2.2)' },
        { name: 'createOffer',     snippet: 'createOffer("${1:alice}")',               doc: 'Generate SDP offer' },
        { name: 'createAnswer',    snippet: 'createAnswer("${1:alice}")',              doc: 'Generate SDP answer' },
        { name: 'addIceCandidate', snippet: 'addIceCandidate("${1:alice}", "${2:candidate}")', doc: 'Add remote ICE candidate' },
        { name: 'dataChannel',     snippet: 'dataChannel("${1:chat}")',                doc: 'Open a data channel' },
        { name: 'send',            snippet: 'send("${1:chat}", "${2:hello}")',         doc: 'Send a message over a data channel' },
        { name: 'close',           snippet: 'close("${1:alice}")',                     doc: 'Close peer connection' },
    ],
};

export class BantuCompletionProvider implements vscode.CompletionItemProvider {

    provideCompletionItems(
        document: vscode.TextDocument,
        position: vscode.Position,
        token: vscode.CancellationToken,
        context: vscode.CompletionContext
    ): vscode.ProviderResult<vscode.CompletionItem[]> {
        const lineUpToCursor = document.lineAt(position).text.substr(0, position.character);
        const items: vscode.CompletionItem[] = [];

        // Detect `sua.<namespace>.` context
        const nsMatch = lineUpToCursor.match(/sua\.([a-zA-Z_]+)\.$/);
        if (nsMatch) {
            const ns = nsMatch[1];
            const methods = SUA_METHODS[ns];
            if (methods) {
                for (const m of methods) {
                    const item = new vscode.CompletionItem(m.name, vscode.CompletionItemKind.Method);
                    item.detail = `sua.${ns}.${m.name}`;
                    item.documentation = new vscode.MarkdownString(m.doc);
                    if (m.snippet) {
                        item.insertText = new vscode.SnippetString(m.snippet);
                    }
                    items.push(item);
                }
                return items;
            }
        }

        // Detect `sua.` context — list namespaces
        if (/sua\.$$/.test(lineUpToCursor)) {
            for (const ns of SUA_NAMESPACES) {
                const item = new vscode.CompletionItem(ns.name, vscode.CompletionItemKind.Module);
                item.detail = `sua.${ns.name}`;
                item.documentation = new vscode.MarkdownString(ns.doc);
                items.push(item);
            }
            return items;
        }

        // Detect `$` context — suggest variable name pattern
        if (/\$$/.test(lineUpToCursor)) {
            // Suggest common variable names
            const common = ['i', 'j', 'k', 'name', 'value', 'item', 'key', 'result', 'count', 'sum', 'req', 'res', 'err'];
            for (const v of common) {
                const item = new vscode.CompletionItem(v, vscode.CompletionItemKind.Variable);
                item.detail = 'variable';
                items.push(item);
            }
            return items;
        }

        // Default: keywords + type annotations
        for (const kw of KEYWORDS) {
            const item = new vscode.CompletionItem(kw.name, vscode.CompletionItemKind.Keyword);
            item.documentation = new vscode.MarkdownString(kw.doc);
            if (kw.snippet) {
                item.insertText = new vscode.SnippetString(kw.snippet);
            }
            items.push(item);
        }
        for (const t of TYPES) {
            const item = new vscode.CompletionItem(t.name, vscode.CompletionItemKind.TypeParameter);
            item.documentation = new vscode.MarkdownString(t.doc);
            items.push(item);
        }

        // Add `sua` itself when not preceded by `.`
        if (!/sua\b/.test(lineUpToCursor)) {
            const suaItem = new vscode.CompletionItem('sua', vscode.CompletionItemKind.Module);
            suaItem.detail = 'Sua Framework';
            suaItem.documentation = new vscode.MarkdownString(
                'Bantu\'s built-in web framework. Provides:\n' +
                '- `sua.server` — HTTP server\n' +
                '- `sua.http`   — HTTP client\n' +
                '- `sua.sqlite` / `sua.postgres` / `sua.mysql` — database drivers\n' +
                '- `sua.json`   — JSON helpers\n' +
                '- `sua.webrtc` — WebRTC peer/data channels (v1.2.2)\n' +
                '- `sua.include(path)` — runtime module loader (v1.2.2)'
            );
            items.push(suaItem);
        }

        return items;
    }
}
