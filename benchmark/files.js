// Mirrors files.nio in JavaScript. V8's regexp engine is a backtracker where
// Nio's is an NFA simulation.
const fs = require("fs");
const os = require("os");
const path = require("path");

const n = 1000;
const lines = 400;

let doc = "";
let k = 0;
while (k < lines) {
    doc = doc + "field_" + k + " = value_" + k + "\n";
    k = k + 1;
}
doc = doc + "counter = 0000\n";

const dir = fs.mkdtempSync(path.join(os . tmpdir(), "nio-bench-files-"));
const file = path.join(dir, "data.txt");
fs.writeFileSync(file, doc);

const re = /counter = [0-9]{4}/;
let checksum = 0;
let i = 0;

while (i < n) {
    const text = fs.readFileSync(file, "latin1");
    const at = text.search(re);
    checksum = checksum + at;

    const v = parseInt(text.substring(at + 10, at + 14), 10) + 1;
    const num = String(v).padStart(4, "0");
    const edited = text.substring(0, at + 10) + num + text.substring(at + 14);

    fs.writeFileSync(file, edited, "latin1");
    i = i + 1;
}

const text = fs.readFileSync(file, "latin1");
const at = text.search(re);
const counter = parseInt(text.substring(at + 10, at + 14), 10);

fs.rmSync(dir, { recursive: true });
console.log(checksum + counter);
