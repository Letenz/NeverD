package runtime.language;
public final class SequenceOracle {
    public static void main(String[] args) {
        for (int mode=0;mode<22;mode++) {
            Language.side=0;
            System.out.printf("%016x%n",Language.run(mode,1));
        }
    }
}
