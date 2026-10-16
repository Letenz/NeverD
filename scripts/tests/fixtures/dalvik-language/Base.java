package runtime.language;
public class Base {
    public static int shared = 19;
    public int value;
    public Base(int n) { value = n; }
    public long compute(int n) { return value + (long)n; }
    public String name() { return "base"; }
}
