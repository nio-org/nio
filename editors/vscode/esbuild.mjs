// Bundling the extension into one file.
//
// VS Code loads an extension as CommonJS from Node, and `vscode` itself is
// supplied by the host rather than installed -- so it is the one thing left
// external. Everything else, which is vscode-languageclient and what it depends
// on, is folded in: an unbundled extension ships its whole node_modules inside
// the .vsix and is measurably slower to activate, since the host resolves every
// file of it at startup.
//
//   node esbuild.mjs               build once, with a source map
//   node esbuild.mjs --watch       rebuild as src/ changes
//   node esbuild.mjs --production  minified, no map (what packaging runs)
import * as esbuild from "esbuild";

const production = process.argv.includes("--production");
const watch = process.argv.includes("--watch");

const options = {
    entryPoints: ["src/extension.ts"],
    bundle: true,
    outfile: "out/extension.js",
    // The two things the host decides for us.
    format: "cjs",
    platform: "node",
    // VS Code 1.82 ships Node 18.
    target: "node18",
    external: ["vscode"],
    sourcemap: !production,
    minify: production,
    logLevel: "info",
};

if (watch) {
    const ctx = await esbuild.context(options);
    await ctx.watch();
} else {
    await esbuild.build(options);
}
