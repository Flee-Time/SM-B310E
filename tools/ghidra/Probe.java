// Batch query script for the B310E stock firmware.
// Args: <commands file> <output file>
// Commands (one per line):
//   lit <lo> <hi>        functions whose literal pools hold a word in [lo,hi]
//   str <substring>      functions referencing strings containing substring
//   dec <addr>           decompile the function containing addr
//   dis <addr> <count>   disassemble count instructions from addr
//   callers <addr>       functions calling the function at addr
//   words <addr> <count> dump count words
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.data.StringDataInstance;
import java.io.*;
import java.nio.file.*;
import java.util.*;

public class Probe extends GhidraScript {
    PrintWriter out;
    DecompInterface dec;

    long num(String s) { return Long.decode(s); }
    Address addr(long v) { return currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(v); }

    String fname(Function f) { return f == null ? "<nofunc>" : f.getName() + "@" + f.getEntryPoint(); }

    void lit(long lo, long hi) throws Exception {
        Memory mem = currentProgram.getMemory();
        Map<String, Set<String>> byFunc = new TreeMap<>();
        for (MemoryBlock b : mem.getBlocks()) {
            if (!b.isInitialized()) continue;
            long start = b.getStart().getOffset(), end = b.getEnd().getOffset();
            byte[] buf = new byte[(int)(end - start + 1)];
            b.getBytes(b.getStart(), buf);
            for (int i = 0; i + 4 <= buf.length; i += 4) {
                long v = (buf[i] & 0xffL) | (buf[i+1] & 0xffL) << 8 | (buf[i+2] & 0xffL) << 16 | (buf[i+3] & 0xffL) << 24;
                if (v < lo || v > hi) continue;
                Address a = addr(start + i);
                for (Reference r : getReferencesTo(a)) {
                    Function f = getFunctionContaining(r.getFromAddress());
                    byFunc.computeIfAbsent(fname(f), k -> new TreeSet<>()).add(String.format("0x%08x", v));
                }
            }
        }
        out.printf("== lit %x..%x : %d functions%n", lo, hi, byFunc.size());
        for (var e : byFunc.entrySet()) out.println("  " + e.getKey() + " " + e.getValue());
    }

    void str(String sub) {
        out.println("== str " + sub);
        for (Data d : currentProgram.getListing().getDefinedData(true)) {
            if (!d.hasStringValue()) continue;
            String s = String.valueOf(d.getValue());
            if (!s.contains(sub)) continue;
            Set<String> fs = new TreeSet<>();
            for (Reference r : getReferencesTo(d.getAddress())) {
                fs.add(fname(getFunctionContaining(r.getFromAddress())));
                // literal pool indirection
                for (Reference r2 : getReferencesTo(r.getFromAddress()))
                    fs.add(fname(getFunctionContaining(r2.getFromAddress())));
            }
            out.println("  " + d.getAddress() + " \"" + s + "\" <- " + fs);
        }
    }

    void decompile(long a) {
        Function f = getFunctionContaining(addr(a));
        if (f == null) { out.printf("== dec %x : no function%n", a); return; }
        DecompileResults r = dec.decompileFunction(f, 120, monitor);
        out.println("== dec " + fname(f));
        out.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "  <failed: " + r.getErrorMessage() + ">");
    }

    void dis(long a, int n) {
        out.printf("== dis %x%n", a);
        Instruction ins = getInstructionAt(addr(a));
        if (ins == null) ins = getInstructionAfter(addr(a));
        for (int i = 0; i < n && ins != null; i++, ins = ins.getNext())
            out.println("  " + ins.getAddress() + "  " + ins);
    }

    void callers(long a) {
        out.printf("== callers %x%n", a);
        Set<String> fs = new TreeSet<>();
        for (Reference r : getReferencesTo(addr(a)))
            fs.add(fname(getFunctionContaining(r.getFromAddress())) + " from " + r.getFromAddress());
        for (String s : fs) out.println("  " + s);
    }

    void words(long a, int n) throws Exception {
        out.printf("== words %x%n", a);
        for (int i = 0; i < n; i++) {
            if (i % 4 == 0) out.printf("%n  %08x:", a + i * 4);
            out.printf(" %08x", getInt(addr(a + i * 4)) & 0xffffffffL);
        }
        out.println();
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        out = new PrintWriter(new FileWriter(args[1]));
        // NOR is XIP flash: mark it read-only so the decompiler folds
        // literal-pool constants (DAT_xxx) into real register addresses.
        for (MemoryBlock b : currentProgram.getMemory().getBlocks())
            if (b.getStart().getOffset() < 0x01000000L) b.setWrite(false);
        dec = new DecompInterface();
        DecompileOptions opts = new DecompileOptions();
        opts.grabFromProgram(currentProgram);
        dec.setOptions(opts);
        dec.openProgram(currentProgram);
        for (String line : Files.readAllLines(Paths.get(args[0]))) {
            line = line.trim();
            if (line.isEmpty() || line.startsWith("#")) continue;
            String[] p = line.split("\\s+", 2);
            String[] a = p.length > 1 ? p[1].split("\\s+") : new String[0];
            try {
                switch (p[0]) {
                    case "lit": lit(num(a[0]), num(a[1])); break;
                    case "str": str(p[1]); break;
                    case "strexact": {
                        // every occurrence of the NUL-terminated bytes, plus raw refs to it
                        byte[] pat = (p[1] + "\0").getBytes();
                        Address s = currentProgram.getMinAddress();
                        int found = 0;
                        while (s != null && found < 40) {
                            s = currentProgram.getMemory().findBytes(s, pat, null, true, monitor);
                            if (s == null) break;
                            found++;
                            out.println("== strexact " + p[1] + " @ " + s);
                            for (Reference r : getReferencesTo(s))
                                out.println("    ref from " + r.getFromAddress() + " " + fname(getFunctionContaining(r.getFromAddress())));
                            // raw pointer words anywhere in NOR
                            byte[] w = new byte[]{(byte)s.getOffset(), (byte)(s.getOffset()>>8), (byte)(s.getOffset()>>16), (byte)(s.getOffset()>>24)};
                            Address q = currentProgram.getMinAddress();
                            for (int k = 0; k < 8; k++) {
                                q = currentProgram.getMemory().findBytes(q, w, null, true, monitor);
                                if (q == null) break;
                                out.println("    ptr at " + q);
                                q = q.add(1);
                            }
                            s = s.add(1);
                        }
                        break;
                    }
                    case "dec": for (String x : a) decompile(num(x)); break;
                    case "range": {
                        AddressSet set = new AddressSet(addr(num(a[0])), addr(num(a[1])));
                        for (Function f : currentProgram.getFunctionManager().getFunctions(set, true))
                            decompile(f.getEntryPoint().getOffset());
                        break;
                    }
                    case "dis": dis(num(a[0]), Integer.parseInt(a[1])); break;
                    case "callers": for (String x : a) callers(num(x)); break;
                    case "words": words(num(a[0]), Integer.parseInt(a[1])); break;
                    default: out.println("?? " + line);
                }
            } catch (Exception e) { out.println("!! " + line + " : " + e); }
            out.flush();
        }
        dec.dispose();
        out.close();
    }
}
