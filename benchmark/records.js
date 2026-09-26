// Mirrors records.nio in JavaScript. Evicted segments become garbage for V8's
// generational GC.
function makeSegment(i) {
    return {
        a: { x: i % 1000, y: (i * 7) % 1000 },
        b: { x: (i * 13) % 1000, y: (i * 31) % 1000 }
    };
}

function length2(s) {
    const dx = s.b.x - s.a.x;
    const dy = s.b.y - s.a.y;
    return dx * dx + dy * dy;
}

const n = 2000000;
const recent = [makeSegment(0), makeSegment(0), makeSegment(0), makeSegment(0)];
let checksum = 0;
let i = 0;

while (i < n) {
    const s = makeSegment(i);
    checksum = (checksum + length2(s)) % 1000000007;
    recent[i % 4] = s;
    i = i + 1;
}

console.log(checksum);
console.log(length2(recent[n % 4]));
