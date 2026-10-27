package runtime.language;
public class Problem extends RuntimeException {
    public int code;
    public Problem(String text, int n) { super(text); code = n; }
}
