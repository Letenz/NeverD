package runtime;
import java.lang.reflect.Method;

public class Oracle {
    static void emit(String name, int kind, long x, long y, long z) throws Exception {
        boolean wide = kind == 1;
        Method method = wide ? Arithmetic.class.getMethod(name, long.class, int.class, long.class)
                             : Arithmetic.class.getMethod(name, int.class, int.class);
        Object value = wide ? method.invoke(null, x, (int)y, z)
                            : method.invoke(null, (int)x, (int)y);
        long bits = value instanceof Boolean ? ((boolean)value ? 1 : 0)
                  : value instanceof Character ? (char)value : ((Number)value).longValue();
        if (method.getReturnType() != long.class) bits &= 0xffffffffL;
        char result = method.getReturnType() == long.class ? 'J'
                    : method.getReturnType() == byte.class ? 'B'
                    : method.getReturnType() == short.class ? 'S'
                    : method.getReturnType() == char.class ? 'C'
                    : method.getReturnType() == boolean.class ? 'Z' : 'I';
        System.out.println(name + " " + (wide ? "(JIJ)" : "(II)") + result + " " +
            Long.toUnsignedString(x, 16) + " " + Long.toUnsignedString(y, 16) + " " +
            Long.toUnsignedString(z, 16) + " " + Long.toUnsignedString(bits, 16));
    }
    public static void main(String[] args) throws Exception {
        int[] inputs = {Integer.MIN_VALUE, -1000, -1, 0, 1, 2, 13, 70000, Integer.MAX_VALUE};
        String[] ops = {"mix", "shifts", "divide", "remain", "dense", "sparse", "loop",
                        "byteValue", "shortValue", "charValue", "truth"};
        for (String op : ops) for (int x : inputs)
            for (int y : new int[]{-1, 1, 33}) emit(op, 0, x, y, 0);
        for (int x : new int[]{0, 1, 7, 15}) emit("recurse", 0, x, -3, 0);
        long[] longs = {Long.MIN_VALUE, -123456789012345L, -1, 0, 1, Long.MAX_VALUE};
        for (String op : new String[]{"wide", "wideDivide", "wideRemain", "convert", "compare"})
            for (long x : longs) for (int shift : new int[]{-1, 0, 65})
                emit(op, 1, x, shift, -1);
    }
}
