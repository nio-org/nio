// Counts primes below 2,000,000 by trial division. Mirrors primes.nio in
// JavaScript.
const limit = 2000000;
let count = 1; // 2 is prime
let i = 3;

while (i < limit) {
    let isPrime = true;
    let j = 3;
    while (j * j <= i && isPrime) {
        if (i % j === 0) {
            isPrime = false;
        }
        j = j + 2;
    }
    if (isPrime) {
        count = count + 1;
    }
    i = i + 2;
}

console.log(count);
