# Counts primes below 2,000,000 by trial division. Mirrors primes.nio in Python.
limit = 2000000
count = 1  # 2 is prime
i = 3

while i < limit:
    isPrime = True
    j = 3
    while j * j <= i and isPrime:
        if i % j == 0:
            isPrime = False
        j = j + 2
    if isPrime:
        count = count + 1
    i = i + 2

print(count)
