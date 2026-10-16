package runtime.state;

import java.lang.reflect.InvocationTargetException;

// Independent JVM execution of original Java, including persistent state and
// the identity of errors observed while loading/resolving/invoking each class.
public class StateOracle {
    private static void step(String name, int value) {
        long result = 0;
        int status = 0;
        try {
            Class<?> type = Class.forName("runtime.state." + name);
            result = (Long) type.getDeclaredMethod("step", int.class).invoke(null, value);
        } catch (Throwable failure) {
            if (failure instanceof InvocationTargetException)
                failure = ((InvocationTargetException) failure).getCause();
            if (failure instanceof ExceptionInInitializerError) status = 1;
            else if (failure instanceof NoClassDefFoundError) status = 2;
            else if (failure instanceof ArithmeticException) status = 3;
            else throw new AssertionError(failure);
        }
        System.out.printf("runtime/state/%s %08x %d %016x%n", name, value, status, result);
    }
    public static void main(String[] args) {
        // Reversed first-touch order exercises both sides of recursive class
        // initialization and the prepared zero value on the reentrant edge.
        if (args.length != 0) step("CycleB", 0);
        step("Effects", 0);
        for (int value : new int[] {1, -1, 0, 31, 32, 63, 64, -32,
                                   Integer.MIN_VALUE, Integer.MAX_VALUE, 255, 65535})
            step("State", value);
        step("Effects", 0);
        step("BodyFailure", 0);
        step("BodyFailure", 2);
        step("Effects", 0);
        step("OuterBroken", 0);
        step("OuterBroken", 0);
        step("Broken", 0);
        step("Effects", 0);
    }
}
