// 100 waves of 10,000 concurrent tasks. Mirrors spawn.nio in JavaScript.
async function work(x) {
    return x % 7;
}

async function main() {
    let total = 0;
    for (let w = 0; w < 100; w++) {
        const fs = [];
        for (let i = 0; i < 10000; i++) {
            fs.push(work(w + i));
        }
        for (const f of fs) {
            total = (total + await f) % 1000000;
        }
    }
    console.log(total);
}

main();
