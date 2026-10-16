package runtime.language;
public class Child extends Base implements Operation {
    public int extra;
    public Child(int n) { this(n, 7); }
    public Child(int n, int e) { super(n); extra = e; }
    @Override public long compute(int n) { return super.compute(n) * 3 + extra; }
    @Override public String name() { return "child"; }
    @Override public boolean equals(Object o) { return o instanceof Child && ((Child)o).value == value; }
    public long parent(int n) { return super.compute(n); }
}
