package runtime.objects;

public final class ObjectsOracle {
    public static void main(String[] args) {
        int seed = Integer.parseInt(args[0]);
        long[] out = new long[32];
        Box box = new Box(seed);
        out[0] = box.value & 0xffffffffL;
        out[1] = box.wide;
        out[2] = box.flag ? 1 : 0;
        out[3] = box.small & 0xffffffffL;
        out[4] = box.unit;
        out[5] = box.half & 0xffffffffL;
        out[6] = box.serial;
        box.value = seed ^ 0x55555555;
        box.wide = 0xfedcba9876543210L;
        box.flag = true;
        box.small = -128;
        box.unit = 65535;
        box.half = -32768;
        out[7] = box.calculate(17);
        out[8] = box.value & 0xffffffffL;
        out[9] = box.bytes[1] & 0xff;
        Object identity = box.bytes;
        out[10] = box.echo(identity) == identity ? 1 : 0;
        out[11] = box.tag == identity ? 1 : 0;
        box.bytes[0] = 42;
        out[12] = box.bytes[0];
        Box[] peers = new Box[2];
        peers[1] = box;
        box.peers = peers;
        out[13] = box.peers[1] == box ? 1 : 0;
        Box made = Box.make(-3);
        out[14] = made.peers[0] == made ? 1 : 0;
        out[15] = Box.saved == made ? 1 : 0;
        out[16] = Box.shared == made.bytes ? 1 : 0;
        Box.count = 99;
        out[17] = new Box(0).serial;
        Box.saved = box;
        out[18] = Box.saved == box ? 1 : 0;
        out[19] = Box.arrays(seed);
        for (int mode = 0; mode < 5; ++mode) {
            try { Box.fault(mode); out[20 + mode] = 0; }
            catch (NullPointerException e) { out[20 + mode] = 1; }
            catch (ArrayIndexOutOfBoundsException e) { out[20 + mode] = 2; }
            catch (NegativeArraySizeException e) { out[20 + mode] = 3; }
            catch (ArrayStoreException e) { out[20 + mode] = 4; }
            catch (ClassCastException e) { out[20 + mode] = 5; }
        }
        out[25] = box.calculate(-21);
        out[26] = box.bytes[1] & 0xff;
        out[27] = box.peers == peers ? 1 : 0;
        out[28] = box.value & 0xffffffffL;
        out[29] = Box.count;
        byte[][] nested = {new byte[3]};
        out[30] = nested.length + nested[0].length;
        Box.staticFlag = true; Box.staticSmall = -120; Box.staticUnit = 65534;
        Box.staticHalf = -30000; Box.staticWide = 0x123456789abcdef0L;
        out[31] = Box.statics();
        for (long word : out) System.out.printf("%016x%n", word);
    }
}
