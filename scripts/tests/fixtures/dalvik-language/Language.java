package runtime.language;
public final class Language {
    public static final String TEXT = "A\0\ud83d\ude00\ud800\uffffZ";
    public static int side;
    public static Throwable saved;
    public static String literal() { return TEXT; }
    public static void escape() { Problem p = new Problem("detail", 37); saved = p; throw p; }
    public static void rethrow(int n) {
        try { Problem p = new Problem(TEXT, n); saved = p; throw p; }
        catch (RuntimeException e) { side += 7; throw e; }
        finally { side += 11; }
    }
    public static long run(int mode, int seed) {
        switch (mode) {
        case 0: return ((long)TEXT.hashCode() << 32) ^ TEXT.length() ^ TEXT.charAt(4);
        case 1: {
            String copy = new String(TEXT);
            String joined = "A".concat(copy.substring(1));
            return (copy == TEXT ? 1 : 0) + (joined.equals(TEXT) ? 2 : 0)
                + (joined.intern() == TEXT ? 4 : 0) + (TEXT.substring(0) == TEXT ? 8 : 0)
                + (TEXT.concat("") == TEXT ? 16 : 0);
        }
        case 2: {
            char[] a = {'x', 0, '\ud800', '\udfff', '\uffff', 'y'};
            String s = new String(a, 1, 4);
            a[1] = 'q';
            char[] out = new char[8];
            s.getChars(0, s.length(), out, 2);
            return s.hashCode() + (long)String.valueOf(out).hashCode() * 31 + s.toCharArray()[2];
        }
        case 3: return TEXT.indexOf("\ud83d\ude00", -9) + TEXT.indexOf("", 99) * 31L
                    + TEXT.indexOf("none") * 7L + TEXT.compareTo("A\0\ud83d\ude00\ud801")
                    + (TEXT.startsWith("A", -1) ? 1000 : 0) + (TEXT.endsWith("Z") ? 17 : 0);
        case 4: return String.valueOf(seed).hashCode() + String.valueOf(Long.MIN_VALUE).hashCode() * 31L
                    + String.valueOf((char)seed).charAt(0) + String.valueOf(true).length();
        case 5:
            try { side = seed; return 123 / seed; }
            catch (ArithmeticException e) { side += 3; return side; }
            finally { side += 9; }
        case 6: {
            long result = 0;
            for (int i=0;i<180;i++) try { rethrow(i); }
            catch (Problem p) { result += p.code + (p == saved ? 1 : 0); }
            return result + side;
        }
        case 7:
            try { rethrow(seed); }
            catch (Problem p) { return p.code + (long)side * 31 + (p.getMessage() == TEXT ? 999 : 0) + (p == saved ? 3000 : 0); }
            return -1;
        case 8:
            try { throw (RuntimeException)null; }
            catch (NullPointerException p) { return 11; }
            catch (RuntimeException p) { return 22; }
        case 9: { Base b = new Child(seed); return b.compute(17) * 31 + ((Child)b).parent(-9) + b.value; }
        case 10: { Operation op = new Child(seed); return op.compute(-13); }
        case 11: {
            Base[] a = new Child[2]; a[0] = new Child(seed);
            try { a[1] = new Base(2); }
            catch (ArrayStoreException e) { return a[0].compute(5) + (a instanceof Child[] ? 27 : 0); }
            return -1;
        }
        case 12: return Child.class == new Child(seed).getClass() ? Child.class.getName().hashCode() : -1;
        case 13: { Child c = new Child(seed, 21); return c.value + c.extra + Child.shared + Child.stage + Child.offset(seed); }
        case 14: { Base b = new Child(seed); return b.name().equals("child") && b.equals(new Child(seed)) ? 1 : 0; }
        case 15:
            try { TEXT.charAt(-1); return -1; }
            catch (IndexOutOfBoundsException e) { return e instanceof StringIndexOutOfBoundsException ? 7 : 0; }
        case 16:
            try { TEXT.substring(5, 2); return -1; }
            catch (RuntimeException e) { return e.getClass().getName().hashCode(); }
        case 17: return new String(new char[]{'\ud800'}).intern() == "\ud800" ? 1 : 0;
        case 18: { String[] a = new String[]{"abc", ""}; return a[0].length() + (a[1].isEmpty() ? 1 : 0); }
        case 19:
            try { return 3 / seed; }
            catch (NullPointerException e) { return -2; }
            catch (Exception e) { return 21; }
        case 20: { String s = null; try { return s.length(); } catch (NullPointerException e) { return 99; } }
        case 21: { try { escape(); } catch (Throwable e) { return e == saved && e.getMessage().equals("detail") ? 37 : 0; } return -1; }
        default: return -123;
        }
    }
    public static long unknown(int x, int y) { try { return "x".trim().length(); } catch (Throwable e) { return 42; } }
    public static synchronized long monitor(int x, int y) { return x; }
    public static long unsupportedCaught(int x, int y) { try { return monitor(x,y); } catch (Throwable e) { return 42; } }
}
