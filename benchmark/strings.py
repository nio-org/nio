# Mirrors strings.nio in Python. CPython has an in-place fast path when the
# left operand's refcount is 1, which this loop hits.
n = 100000
target = "abababababababababababababababab"
hits = 0
i = 0

while i < n:
    s = ""
    j = 0
    while j < 16:
        s = s + "ab"
        j = j + 1
    if s == target:
        hits = hits + 1
    i = i + 1

print(hits)
