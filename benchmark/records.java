// The Java version of records.nio. Each point and segment is a separate heap
// object. A segment that leaves the ring becomes garbage for the collector.
class Main {
    static class Point {
        long x;
        long y;

        Point(long x, long y) {
            this.x = x;
            this.y = y;
        }
    }

    static class Segment {
        Point a;
        Point b;

        Segment(Point a, Point b) {
            this.a = a;
            this.b = b;
        }
    }

    static Segment makeSegment(long i) {
        return new Segment(
            new Point(i % 1000, (i * 7) % 1000),
            new Point((i * 13) % 1000, (i * 31) % 1000));
    }

    static long length2(Segment s) {
        long dx = s.b.x - s.a.x;
        long dy = s.b.y - s.a.y;
        return dx * dx + dy * dy;
    }

    public static void main(String[] args) {
        int n = 2000000;
        Segment[] recent = {makeSegment(0), makeSegment(0), makeSegment(0), makeSegment(0)};
        long checksum = 0;
        int i = 0;

        while (i < n) {
            Segment s = makeSegment(i);
            checksum = (checksum + length2(s)) % 1000000007;
            recent[i % 4] = s;
            i = i + 1;
        }

        System.out.println(checksum);
        System.out.println(length2(recent[n % 4]));
    }
}
