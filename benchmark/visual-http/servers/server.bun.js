// The Bun server for the visual HTTP benchmark. Same routes as server.nio,
// through Bun.serve. Running server.js under Bun measures its node:http shim.
'use strict';

const server = Bun.serve({
    hostname: '127.0.0.1',
    port: 0,
    async fetch(req) {
        // The path is sliced by hand: a `new URL(req.url)` per request is
        // measurable at this rate, and the Node server slices its path too.
        const u = req.url;
        const start = u.indexOf('/', 8); // first slash after "http://host"
        const q = u.indexOf('?', start);
        const path = q < 0 ? u.slice(start) : u.slice(start, q);
        if (req.method === 'POST' && path === '/data') {
            let p;
            try {
                p = await req.json();
            } catch (e) {
                return new Response('bad json: ' + e.message, { status: 400 });
            }
            let total = 0;
            for (const it of p.items) total += it.price;
            return new Response(JSON.stringify({ count: p.items.length, total }), {
                headers: { 'Content-Type': 'application/json' },
            });
        }
        if (req.method === 'GET' && path === '/plaintext') {
            return new Response('Hello, World!', {
                headers: { 'Content-Type': 'text/plain; charset=utf-8' },
            });
        }
        if (req.method === 'GET' && path === '/json') {
            return new Response(JSON.stringify({ message: 'Hello, World!' }), {
                headers: { 'Content-Type': 'application/json' },
            });
        }
        if (req.method === 'GET' && path.startsWith('/users/')) {
            const id = path.slice('/users/'.length);
            if (id.length === 0) {
                return new Response('missing id', { status: 400 });
            }
            return new Response('user ' + id, {
                headers: { 'Content-Type': 'text/plain; charset=utf-8' },
            });
        }
        return new Response('not found', {
            status: 404,
            headers: { 'Content-Type': 'text/plain; charset=utf-8' },
        });
    },
});

// console.log is not used here. Bun colorizes a number with ANSI escapes even
// when stdout is a pipe, and the harness cannot parse that as a port.
process.stdout.write(server.port + '\n');
