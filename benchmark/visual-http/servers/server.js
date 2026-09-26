// The Node.js server for the visual HTTP benchmark. It has the same routes as
// server.nio, runs on one thread, and scans routes linearly as Nio's router
// does.
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

function reply(res, status, type, body) {
    res.writeHead(status, {
        'Content-Type': type,
        'Content-Length': Buffer.byteLength(body),
    });
    res.end(body);
}

const server = http.createServer((req, res) => {
    const q = req.url.indexOf('?');
    const path = q < 0 ? req.url : req.url.slice(0, q);

    // POST /data reads a body, so it is not in the synchronous route table.
    if (req.method === 'POST' && path === '/data') {
        const chunks = [];
        req.on('data', (c) => chunks.push(c));
        req.on('end', () => {
            let p;
            try {
                p = JSON.parse(Buffer.concat(chunks));
            } catch (e) {
                reply(res, 400, 'text/plain; charset=utf-8', 'bad json: ' + e.message);
                return;
            }
            let total = 0;
            for (const it of p.items) total += it.price;
            reply(res, 200, 'application/json',
                JSON.stringify({ count: p.items.length, total }));
        });
        return;
    }

    const segments = path.split('/').filter((s) => s.length > 0);

    for (const route of compiled) {
        const params = match(route, req.method, segments);
        if (params === null) continue;
        const out = route.handler(params);
        // Set Content-Length. Without it, Node sends chunked encoding, and
        // the other servers frame by length.
        reply(res, out.status, out.type, out.body);
        return;
    }
    reply(res, 404, 'text/plain; charset=utf-8', 'not found');
});

server.listen(0, '127.0.0.1', () => {
    process.stdout.write(server.address().port + '\n');
});
