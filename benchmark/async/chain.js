// A million sequential awaits. Mirrors chain.nio in JavaScript.
async function bump(x) {
    return x + 1;
}

async function run(n) {
    let acc = 0;
    for (let i = 0; i < n; i++) {
        acc = (acc + await bump(i)) % 1000000;
    }
    return acc;
}

run(1000000).then((r) => console.log(r));
