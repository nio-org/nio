# Mirrors records.nio in Python. CPython reclaims evicted segments mostly by
# refcount. It uses plain classes: no __slots__, no dataclasses, no tuples.


class Point:
    def __init__(self, x, y):
        self.x = x
        self.y = y


class Segment:
    def __init__(self, a, b):
        self.a = a
        self.b = b


def makeSegment(i):
    return Segment(
        Point(i % 1000, (i * 7) % 1000),
        Point((i * 13) % 1000, (i * 31) % 1000),
    )


def length2(s):
    dx = s.b.x - s.a.x
    dy = s.b.y - s.a.y
    return dx * dx + dy * dy


n = 2000000
recent = [makeSegment(0), makeSegment(0), makeSegment(0), makeSegment(0)]
checksum = 0
i = 0

while i < n:
    s = makeSegment(i)
    checksum = (checksum + length2(s)) % 1000000007
    recent[i % 4] = s
    i = i + 1

print(checksum)
print(length2(recent[n % 4]))
