package runtime.language;
public final class BridgeOracle {
    public static void main(String[] args) {
        int seed = Integer.parseInt(args[0]);
        long[] out = new long[32];
        Child a = new Child(seed), b = new Child(seed);
        out[0]=((Base)a).compute(17); out[1]=a.parent(17); out[2]=((Operation)a).compute(17);
        out[3]=a.value & 0xffffffffL; out[4]=Child.class.getSuperclass()==Base.class?1:0;
        out[5]=Operation.class.isAssignableFrom(Child.class)?1:0; out[6]=Child.shared;
        out[7]=a.equals(b)?1:0; out[8]=a==b?1:0;
        String text=Language.literal(), nativeText=new String(new char[]{'A',0,'\ud83d','\ude00','\ud800','\uffff','Z'});
        out[9]=nativeText.intern()==text?1:0; out[10]=nativeText.hashCode()&0xffffffffL; out[11]=a.name().length();
        Throwable thrown=null;
        try { Language.escape(); } catch (Throwable t) { thrown=t; }
        out[12]=thrown==Language.saved?1:0; out[13]=((Problem)thrown).code;
        out[14]=thrown.getMessage().equals("detail")?1:0;
        try { throw thrown; } catch (Throwable again) { out[15]=again==thrown?1:0; }
        out[16]=thrown.getClass()==Problem.class?1:0;
        String message=new String("detail"); RuntimeException ex=new RuntimeException(message);
        out[17]=ex instanceof Throwable?1:0; out[18]=ex.getMessage()==message?1:0;
        out[19]=text==Language.literal()?1:0;
        for (long v:out) System.out.printf("%016x%n",v);
    }
}
