package runtime.language;
public final class LanguageOracle {
    public static void main(String[] args) {
        int mode = Integer.parseInt(args[0]), seed = Integer.parseInt(args[1]);
        System.out.printf("%016x%n", Language.run(mode,seed));
    }
}
