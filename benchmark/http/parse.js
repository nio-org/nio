// HTTP request parsing in plain JavaScript. Node's llhttp parser is C, and JS
// cannot call it. This parser is written by hand, as Nio's is.
'use strict';

const ITERATIONS = 200000;

const block =
    'GET /users/42?a=1&b=2 HTTP/1.1\r\n' +
    'Host: example.com\r\n' +
    'User-Agent: bench/1\r\n' +
    'Accept: */*\r\n' +
    'Accept-Encoding: gzip, deflate\r\n' +
    'Connection: keep-alive\r\n' +
    'Content-Length: 0\r\n';

function parseOnce(text) {
    const lines = text.split('\r\n');

    const first = lines[0];
    const sp1 = first.indexOf(' ');
    const sp2 = first.indexOf(' ', sp1 + 1);
    const target = first.slice(sp1 + 1, sp2);

    const q = target.indexOf('?');
    const path = q < 0 ? target : target.slice(0, q);
    const query = q < 0 ? '' : target.slice(q + 1);

    const headers = new Map();
    for (let i = 1; i < lines.length; i++) {
        const line = lines[i];
        if (line.length === 0) continue;
        const c = line.indexOf(':');
        if (c <= 0) continue;
        const name = line.slice(0, c).toLowerCase();
        const value = line.slice(c + 1).trim();
        const had = headers.get(name);
        headers.set(name, had === undefined ? value : had + ', ' + value);
    }

    return decodeURIComponent(path).length + query.length + headers.size;
}

let sum = 0;
for (let i = 0; i < ITERATIONS; i++) {
    sum += parseOnce(block);
}
console.log(sum);
