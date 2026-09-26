// 10,000 tasks sleep one millisecond at the same time. This is sleepers.nio in
// JavaScript.
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

let woken = 0;

async function sleeper() {
    await sleep(1);
    woken = woken + 1;
}

async function main() {
    const fs = [];
    for (let i = 0; i < 10000; i++) {
        fs.push(sleeper());
    }
    for (const f of fs) {
        await f;
    }
    console.log(woken);
}

main();
