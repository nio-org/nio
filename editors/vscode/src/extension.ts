// The VS Code client starts and stops `nio lsp`. All language support is in
// the server, in src/lsp/.
import { execFile } from "node:child_process";
import { constants as fsConstants } from "node:fs";
import { access } from "node:fs/promises";
import { join } from "node:path";
import {
    ConfigurationTarget,
    ExtensionContext,
    LogOutputChannel,
    commands,
    window,
    workspace,
} from "vscode";
import {
    LanguageClient,
    LanguageClientOptions,
    ServerOptions,
} from "vscode-languageclient/node";

let client: LanguageClient | undefined;

// ---- finding the compiler ----

// The configured command, with ${workspaceFolder} expanded. The default is a
// bare `nio` on PATH, so the server is the compiler the user builds with.
function serverCommand(): string {
    const configured = workspace
        .getConfiguration("nio")
        .get<string>("server.path", "nio")
        .trim();
    const command = configured === "" ? "nio" : configured;
    const folder = workspace.workspaceFolders?.[0]?.uri.fsPath;
    if (folder === undefined) {
        return command;
    }
    return command.replace(/\$\{workspaceFolder\}/g, folder);
}

const WORKSPACE_NIO = "${workspaceFolder}/nio";

// Answers whether a runnable `nio` is in the workspace root. The client only
// offers it to the user, because opening a folder must not run a program in it.
async function builtInWorkspace(): Promise<string | undefined> {
    const folder = workspace.workspaceFolders?.[0]?.uri.fsPath;
    if (folder === undefined) {
        return undefined;
    }
    const candidate = join(folder, "nio");
    try {
        await access(candidate, fsConstants.X_OK);
        return candidate;
    } catch {
        return undefined;
    }
}

type Probe =
    | { ok: true }
    | { ok: false; missing: boolean; reason: string };

// Answers whether `command lsp` is a language server, before the client uses
// it. The test reads only the status code: `nio lsp` with no input exits 0,
// and a compiler without the command exits 2.
function probe(command: string): Promise<Probe> {
    return new Promise((resolve) => {
        const child = execFile(
            command,
            ["lsp"],
            { timeout: 10_000 },
            (err, _stdout, stderr) => {
                if (err === null) {
                    resolve({ ok: true });
                    return;
                }
                const code = (err as NodeJS.ErrnoException).code;
                if (code === "ENOENT") {
                    resolve({
                        ok: false,
                        missing: true,
                        reason: `no such program: ${command}`,
                    });
                    return;
                }
                if (code === "EACCES") {
                    resolve({
                        ok: false,
                        missing: true,
                        reason: `not executable: ${command}`,
                    });
                    return;
                }
                resolve({
                    ok: false,
                    missing: false,
                    reason:
                        typeof code === "number"
                            ? `${command} lsp exited ${code}: ${stderr.trim() || "no output"}`
                            : `${command} lsp could not be started: ${err.message}`,
                });
            },
        );
        // End of input, so the probed server stops at once.
        child.stdin?.end();
    });
}

// ---- the client ----

async function start(log: LogOutputChannel): Promise<void> {
    const command = serverCommand();
    const result = await probe(command);
    if (!result.ok) {
        log.error(`not starting: ${result.reason}`);
        const nearby = await builtInWorkspace();
        if (nearby !== undefined && command !== nearby) {
            log.info(`there is a nio at ${nearby}`);
            const use = "Use the one in this workspace";
            const chosen = await window.showWarningMessage(
                `Nio language server: ${result.reason}. There is a compiler at ${nearby}.`,
                use,
            );
            if (chosen === use) {
                // The configuration listener sees this write and restarts.
                await workspace
                    .getConfiguration("nio")
                    .update("server.path", WORKSPACE_NIO, ConfigurationTarget.Workspace);
            }
            return;
        }
        const hint = result.missing
            ? "Put `nio` on your PATH, or set `nio.server.path` to where it is."
            : "This looks like a `nio` older than the `lsp` command. Rebuild it, or set `nio.server.path`.";
        window.showWarningMessage(`Nio language server: ${result.reason}. ${hint}`);
        return;
    }

    const serverOptions: ServerOptions = {
        command,
        // The server accepts and ignores --stdio. It has no other transport.
        args: ["lsp", "--stdio"],
    };
    const clientOptions: LanguageClientOptions = {
        // Files on disk only: the server resolves a document's imports against
        // its directory, and an untitled buffer has none.
        documentSelector: [{ scheme: "file", language: "nio" }],
        outputChannel: log,
    };

    client = new LanguageClient("nio", "Nio Language Server", serverOptions, clientOptions);
    log.info(`starting: ${command} lsp`);
    await client.start();
}

async function stop(): Promise<void> {
    const running = client;
    client = undefined;
    if (running !== undefined) {
        await running.stop();
    }
}

export async function activate(context: ExtensionContext): Promise<void> {
    const log = window.createOutputChannel("Nio Language Server", { log: true });
    context.subscriptions.push(log);

    // A rebuilt `nio` needs a restart. The running server is the old binary.
    context.subscriptions.push(
        commands.registerCommand("nio.restartServer", async () => {
            log.info("restarting");
            await stop();
            await start(log);
        }),
    );

    context.subscriptions.push(
        workspace.onDidChangeConfiguration(async (e) => {
            if (e.affectsConfiguration("nio.server.path")) {
                log.info("nio.server.path changed, restarting");
                await stop();
                await start(log);
            }
        }),
    );

    await start(log);
}

export function deactivate(): Promise<void> {
    return stop();
}
