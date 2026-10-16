package runtime.language;
public class Base {
    public static int shared = initialize();
    private static int initialize() { Events.value = Events.value * 10 + 1; return 19; }
    public static int offset(int n) { return n + shared; }
    public int value;
    public Base(int n) { value = n; }
    public long compute(int n) { return value + (long)n; }
    public String name() { return "base"; }
}
