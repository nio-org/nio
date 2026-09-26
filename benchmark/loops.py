# 100,000,000 iterations of a nested loop. Mirrors loops.nio in Python.
u = 10
r = 5000
a = [0] * 10000
i = 0

while i < 10000:
    j = 0
    while j < 10000:
        a[i] = a[i] + j % u
        j = j + 1
    a[i] = a[i] + r
    i = i + 1

print(a[r])
