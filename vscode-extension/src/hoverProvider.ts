/**
 * BantuHoverProvider — shows hover hints (documentation) for keywords,
 * types, and `sua.*` API surfaces.
 */

import * as vscode from 'vscode';

const HOVERS: Record<string, string> = {
    'if':       '**if** — Conditional branch.\n\n```\nif (condition) {\n  // then\n} else {\n  // else\n}\n```',
    'else':     '**else** — Else branch of an `if`.',
    'while':    '**while** — Loop while a condition is truthy.',
    'for':      '**for** — C-style for loop: `for ($i = 0; $i < N; $i += 1) { ... }`',
    'each':     '**each** — Iterate over a list: `each ($item in list) { ... }`',
    'def':      '**def** — Define a function:\n```\ndef name(args) {\n  // body\n  return value;\n}\n```',
    'return':   '**return** — Return a value from a function.',
    'class':    '**class** — Define a class with methods:\n```\nclass Name {\n  public def method() { ... }\n}\n```',
    'try':      '**try** — Try-catch block:\n```\ntry { ... } catch { ... }\n```',
    'include':  '**include** (v1.2.2) — Load a Bantu module:\n```\ninclude "./routes.b";            // direct\ninclude "./controller.b" as ctrl; // namespaced\n```\nModules execute in a child scope and their symbols are imported.',
    'import':   '**import** — Alias for `include`.',
    'print':    '**print** — Print to stdout.',
    'const':    '**const** — Constant declaration.',
    'new':      '**new** — Instantiate a class.',
    'number':   '**number** — 64-bit floating-point type.',
    'string':   '**string** — UTF-8 string type.',
    'bool':     '**bool** — Boolean (true/false).',
    'list':     '**list** — Ordered array type.',
    'dict':     '**dict** — Key-value object type.',
    'any':      '**any** — Dynamic type.',
    'func':     '**func** — Function reference type.',
    'sua':      '**sua** — Bantu\'s built-in web framework. Namespaces: `server`, `http`, `sqlite`, `postgres`, `mysql`, `json`, `webrtc`, `include`.',
};

const SUA_METHOD_HOVERS: Record<string, string> = {
    'sua.server.get':    '**sua.server.get(path, handler)** — Register an HTTP GET route.\n\nHandler signature: `def($req, $res) { ... }`',
    'sua.server.post':   '**sua.server.post(path, handler)** — Register an HTTP POST route.',
    'sua.server.put':    '**sua.server.put(path, handler)** — Register an HTTP PUT route.',
    'sua.server.delete': '**sua.server.delete(path, handler)** — Register an HTTP DELETE route.',
    'sua.server.listen': '**sua.server.listen(port)** — Start the HTTP server on the given port.',
    'sua.server.static': '**sua.server.static(dir)** — Serve static files from the directory.',
    'sua.server.use':    '**sua.server.use(middleware)** — Register middleware.',
    'sua.sqlite.connect':'**sua.sqlite.connect(path)** — Open an SQLite database file.',
    'sua.sqlite.exec':   '**sua.sqlite.exec(sql)** — Execute SQL (no rows returned).',
    'sua.sqlite.query':  '**sua.sqlite.query(sql)** — Run SELECT and return rows as a list of dicts.',
    'sua.postgres.connect':'**sua.postgres.connect(connStr)** — Connect to PostgreSQL using a libpq-style connection string.',
    'sua.postgres.query':'**sua.postgres.query(sql)** — Run SQL and return rows.',
    'sua.mysql.connect': '**sua.mysql.connect(host, user, password, db, port=3306)** — Connect to MySQL/MariaDB.',
    'sua.mysql.query':   '**sua.mysql.query(sql)** — Run SQL and return rows.',
    'sua.json.parse':    '**sua.json.parse(str)** — Parse a JSON string to a dict.',
    'sua.json.stringify':'**sua.json.stringify(value)** — Serialize a value to a JSON string.',
    'sua.webrtc.peer':   '**sua.webrtc.peer(id)** (v1.2.2) — Create a WebRTC peer.',
    'sua.webrtc.createOffer':  '**sua.webrtc.createOffer(peerId)** (v1.2.2) — Generate an SDP offer.',
    'sua.webrtc.createAnswer': '**sua.webrtc.createAnswer(peerId)** (v1.2.2) — Generate an SDP answer.',
    'sua.webrtc.dataChannel':  '**sua.webrtc.dataChannel(label)** (v1.2.2) — Open a data channel.',
    'sua.webrtc.send':   '**sua.webrtc.send(channel, message)** (v1.2.2) — Send a message over a data channel.',
    'sua.include':       '**sua.include(path)** (v1.2.2) — Runtime module loader. Returns the module as a dict (does not pollute scope).',
};

export class BantuHoverProvider implements vscode.HoverProvider {
    provideHover(
        document: vscode.TextDocument,
        position: vscode.Position,
        token: vscode.CancellationToken
    ): vscode.ProviderResult<vscode.Hover> {
        const range = document.getWordRangeAtPosition(position, /[\w$.]+/);
        if (!range) return null;
        const word = document.getText(range);

        // Check direct matches
        if (HOVERS[word]) {
            return new vscode.Hover(new vscode.MarkdownString(HOVERS[word]), range);
        }

        // Check `sua.<namespace>.<method>` form
        // Strip leading `$` if any
        const cleaned = word.replace(/^\$/, '');
        if (SUA_METHOD_HOVERS[cleaned]) {
            return new vscode.Hover(new vscode.MarkdownString(SUA_METHOD_HOVERS[cleaned]), range);
        }

        // Check `sua.<namespace>` form
        const nsMatch = cleaned.match(/^(sua\.[a-zA-Z_]+)$/);
        if (nsMatch && SUA_METHOD_HOVERS[nsMatch[1]]) {
            return new vscode.Hover(new vscode.MarkdownString(SUA_METHOD_HOVERS[nsMatch[1]]), range);
        }

        return null;
    }
}
