package runtime;

// Original executable fixture. The host JVM is an independent value oracle;
// only D8's output is provided to NeverD's workload VM.
public class Arithmetic {
    public static int mix(int x, int y) {
        return ((x + y) * (x - y)) ^ (~x | (y & 0x3f));
    }
    public static int shifts(int x, int y) {
        return (x << y) ^ (x >> y) ^ (x >>> y);
    }
    public static int divide(int x, int y) { return x / y; }
    public static int remain(int x, int y) { return x % y; }
    public static int dense(int x, int y) {
        switch (x) {
            case -1: return y + 7;
            case 0: return y * 2;
            case 1: return y ^ 37;
            case 2: return -y;
            default: return y - x;
        }
    }
    public static int sparse(int x, int y) {
        switch (x) {
            case -1000: return y + 11;
            case 13: return ~y;
            case 70000: return y >>> 3;
            default: return y - 9;
        }
    }
    public static int loop(int x, int y) {
        int s = x;
        for (int i = 0; i < (y & 15); ++i) s = Helper.step(s, i);
        return s;
    }
    public static long wide(long x, int y, long z) {
        return ((x + z) * (x - z)) ^ ((x << y) | (z >> y)) ^ (x >>> y);
    }
    public static long wideDivide(long x, int y, long z) { return x / z; }
    public static long wideRemain(long x, int y, long z) { return x % z; }
    public static long convert(long x, int y, long z) {
        return (long)((int)x + (byte)y + (short)y + (char)y) + z;
    }
    public static int compare(long x, int y, long z) { return x < z ? -1 : x == z ? 0 : 1; }
    public static byte byteValue(int x, int y) { return (byte)(x + y); }
    public static short shortValue(int x, int y) { return (short)(x + y); }
    public static char charValue(int x, int y) { return (char)(x + y); }
    public static boolean truth(int x, int y) { return x >= y; }
    public static int recurse(int x, int y) { return x == 0 ? y : recurse(x - 1, y * 3 + x); }
    public static int forever(int x, int y) { while (x == x) { y++; } return y; }
    public static int objectState(int x, int y) { return new Object().hashCode(); }
    public static int handled(int x, int y) {
        try { return x / y; } catch (ArithmeticException e) { return 77; }
    }
}
