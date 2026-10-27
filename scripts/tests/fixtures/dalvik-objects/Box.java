package runtime.objects;

public final class Box {
    public int value;
    public long wide;
    public boolean flag;
    public byte small;
    public char unit;
    public short half;
    public Object tag;
    public byte[] bytes;
    public Box[] peers;
    public final int serial;
    public static int count = 3;
    public static Box saved = new Box(123);
    public static byte[] shared;
    public static boolean staticFlag;
    public static byte staticSmall;
    public static char staticUnit;
    public static short staticHalf;
    public static long staticWide;
    public static long statics() {
        return staticWide + staticSmall + staticUnit + staticHalf + (staticFlag ? 17 : 0);
    }

    public Box(int seed) {
        value = seed;
        wide = ((long) seed << 33) ^ 0x123456789abcdef0L;
        flag = (seed & 1) != 0;
        small = (byte) seed;
        unit = (char) seed;
        half = (short) seed;
        bytes = new byte[] {-128, 1, 3, 7, 127};
        peers = new Box[2];
        serial = ++count;
    }
    private int twice(int n) { return n * 2; }
    public long calculate(int n) {
        value += twice(n);
        bytes[1] = (byte) (bytes[1] + n);
        return wide + value + small + unit + half + (flag ? 11 : 0) + bytes[1];
    }
    public Object echo(Object value) { tag = value; return tag; }
    public static Box make(int n) {
        Box result = new Box(n);
        result.peers[0] = result;
        saved = result;
        shared = result.bytes;
        return result;
    }
    public static long arrays(int seed) {
        int[] ints = {seed, seed + 1, seed - 1};
        long[] longs = {0x123456789abcdef0L, (long) seed, -1L};
        short[] shorts = { (short) seed, -32768, 32767, -1, 12 };
        char[] chars = { (char) seed, 65535, 0, 1, 2 };
        boolean[] flags = {true, false, true, false, true};
        int[][] nested = {ints, new int[] {2, 3, 5}};
        longs[1] += ints[1];
        shorts[0]++;
        chars[0]++;
        flags[1] = !flags[0];
        return longs[0] + longs[1] + shorts[0] + chars[0] + nested[0][2]
            + nested[1].length + (flags[1] ? 5 : 9);
    }
    public static int fault(int mode) {
        if (mode == 0) { int[] a = null; return a.length; }
        if (mode == 1) { int[] a = new int[2]; return a[2]; }
        if (mode == 2) { int[] a = new int[-1]; return a.length; }
        if (mode == 3) { Object[] a = new Box[1]; a[0] = new int[1]; return 1; }
        if (mode == 4) { Object a = new int[1]; return ((Box) a).value; }
        return 9;
    }
    public static Object exhaust() { return new int[Integer.MAX_VALUE]; }
}
