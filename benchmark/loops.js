// 100,000,000 iterations of a nested loop. Mirrors loops.nio in JavaScript.
let u = 10;
let r = 5000;
let a = new Array(10000).fill(0);
let i = 0;

while (i < 10000) {
    let j = 0;
    while (j < 10000) {
        a[i] = a[i] + j % u;
        j = j + 1;
    }
    a[i] = a[i] + r;
    i = i + 1;
}

console.log(a[r]);
