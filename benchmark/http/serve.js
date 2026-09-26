// The Node.js side of the HTTP server benchmark. Same three routes as
// serve.nio, on one thread, with the linear route scan Nio's router uses.
'use strict';

const http = require('node:http');

const routes = [
    ['GET', '/plaintext', () => ({
        status: 200,
        type: 'text/plain; charset=utf-8',
        body: 'Hello, World!',
    })],
    ['GET', '/json', () => ({
        status: 200,
        type: 'application/json',
        body: JSON.stringify({ message: 'Hello, World!' }),
    })],
    ['GET', '/users/:id', (params) => ({
        status: 200,
        type: 'text/plain; charset=utf-8',
        body: 'user ' + params.id,
    })],
];

const compiled = routes.map(([method, pattern, handler]) => ({
    method,
    segments: pattern.split('/').filter((s) => s.length > 0),
    handler,
}));

function match(route, method, segments) {
    if (route.method !== method) return null;
    if (route.segments.length !== segments.length) return null;
    const params = {};
    for (let i = 0; i < route.segments.length; i++) {
        const want = route.segments[i];
        if (want.startsWith(':')) {
            params[want.slice(1)] = segments[i];
        } else if (want !== segments[i]) {
            return null;
        }
    }
    return params;
}

const server = http.createServer((req, res) => {
    const q = req.url.indexOf('?');
    const path = q < 0 ? req.url : req.url.slice(0, q);
    const segments = path.split('/').filter((s) => s.length > 0);

    for (const route of compiled) {
        const params = match(route, req.method, segments);
        if (params === null) continue;
        const out = route.handler(params);
        // Content-Length is explicit: without it Node sends chunked encoding,
        // and the other two servers frame by length.
        res.writeHead(out.status, {
            'Content-Type': out.type,
            'Content-Length': Buffer.byteLength(out.body),
        });
        res.end(out.body);
        return;
    }
    const missing = 'not found';
    res.writeHead(404, {
        'Content-Type': 'text/plain; charset=utf-8',
        'Content-Length': Buffer.byteLength(missing),
    });
    res.end(missing);
});

server.listen(0, '127.0.0.1', () => {
    process.stdout.write(server.address().port + '\n');
});
