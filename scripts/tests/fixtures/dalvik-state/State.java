package runtime.state;

public class State {
    public static long step(int value) {
        return Counter.step(value) ^ ((long) CycleA.value << 32) ^ CycleB.value;
    }
}
class Counter {
    static int count = 7;
    static long wide = 0x123456789abcdef0L;
    static byte small = -127;
    static short signed = -32767;
    static char character = 65530;
    static boolean flag = true;
    static final long seed = initialize();
    static long initialize() {
        Effects.initializations++;
        return count + wide;
    }
    public static long step(int value) {
        count += value;
        wide = (wide << value) ^ count;
        small = (byte) (small + value);
        signed = (short) (signed ^ value);
        character = (char) (character + value);
        flag = !flag;
        return seed ^ wide ^ ((long) count << 32) ^ small ^ signed ^ character ^ (flag ? 1 : 0);
    }
}
class CycleA {
    public static int value = CycleB.get() + 11;
    public static int get() { return value; }
    public static long step(int ignored) { return ((long) value << 32) | CycleB.value; }
}
class CycleB {
    public static int value = CycleA.get() + 17;
    public static int get() { return value; }
    public static long step(int ignored) { return ((long) value << 32) | CycleA.value; }
}
class Effects {
    public static int initializations;
    public static int failedAttempts;
    public static int calls;
    public static long step(int ignored) {
        return ((long) initializations << 48) | ((long) failedAttempts << 32) | calls;
    }
}
class Broken {
    static int value = initialize();
    static int initialize() {
        Effects.failedAttempts++;
        int divisor = Effects.initializations - Effects.initializations;
        return 1 / divisor;
    }
    public static long step(int ignored) { return value; }
}
class OuterBroken {
    static long value = Broken.step(0);
    public static long step(int ignored) { return value; }
}
class BodyFailure {
    public static long step(int divisor) {
        Effects.calls++;
        return Effects.calls / divisor;
    }
}
class InstanceField {
    int value;
    public static long step(int ignored) { return 1; }
}
class VolatileField {
    static volatile int value;
    public static long step(int ignored) { return value; }
}
class UnsupportedInitializer {
    static int value = new Object().hashCode();
    public static long step(int ignored) { return value; }
}
class EndlessInitializer {
    static int value = initialize();
    static int initialize() { int x = 0; while (true) x++; }
    public static long step(int ignored) { return value; }
}
