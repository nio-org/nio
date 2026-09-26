// Mirrors strings.nio in JavaScript. V8 uses rope strings, so its
// intermediates cost less than a copy.
const n = 100000;
const target = "abababababababababababababababab";
let hits = 0;
let i = 0;

while (i < n) {
    let s = "";
    let j = 0;
    while (j < 16) {
        s = s + "ab";
        j = j + 1;
    }
    if (s === target) {
        hits = hits + 1;
    }
    i = i + 1;
}

console.log(hits);
