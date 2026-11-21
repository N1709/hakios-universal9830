// Run only with analyzeHeadless -readOnly -noanalysis. Does not patch the ELF.
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import java.io.*;
import java.util.*;

public class DecompileOutlined extends GhidraScript {
    private Address target(Instruction i) {
        if (i == null || !i.getMnemonicString().equals("b")) return null;
        Object[] objects = i.getOpObjects(0);
        return objects.length == 1 && objects[0] instanceof Address ? (Address)objects[0] : null;
    }
    public void run() throws Exception {
        String output = System.getenv("RE_OUT");
        if (output == null) throw new IllegalArgumentException("Set RE_OUT to a private output directory");
        DecompInterface di = new DecompInterface();
        for (String range : getScriptArgs()) {
            String[] parts = range.split(":");
            if (parts.length != 2) throw new IllegalArgumentException("Use hex-start:hex-exclusive-end");
            Address start = toAddr(Long.parseLong(parts[0], 16));
            Address end = toAddr(Long.parseLong(parts[1], 16));
            if (start.compareTo(end) >= 0) throw new IllegalArgumentException("Empty/reversed range");
            Function function = getFunctionAt(start);
            if (function == null) throw new IllegalArgumentException("No function at " + start);
            AddressSet body = new AddressSet(start, end.subtract(1));
            InstructionIterator instructions = currentProgram.getListing().getInstructions(body, true);
            while (instructions.hasNext()) {
                Address stub = target(instructions.next());
                if (stub == null || stub.getOffset() < 0x10f000 || stub.getOffset() >= 0x110000) continue;
                Instruction first = getInstructionAt(stub);
                if (first == null || first.getLength() != 4) continue;
                Instruction second = getInstructionAt(stub.add(4));
                Address back = target(second);
                // Join only the two-instruction outline that branches back into this exact function.
                if (back != null && back.compareTo(start) >= 0 && back.compareTo(end) < 0)
                    body.add(stub, stub.add(7));
            }
            List<Address> remove = new ArrayList<>();
            FunctionIterator functions = currentProgram.getFunctionManager().getFunctions(body, true);
            while (functions.hasNext()) {
                Function other = functions.next();
                if (!other.equals(function)) remove.add(other.getEntryPoint());
            }
            for (Address address : remove) currentProgram.getFunctionManager().removeFunction(address);
            function.setBody(body);
            instructions = currentProgram.getListing().getInstructions(body, true);
            while (instructions.hasNext()) {
                Instruction instruction = instructions.next();
                Address branch = target(instruction);
                if (branch != null && body.contains(branch)) instruction.setFlowOverride(FlowOverride.BRANCH);
            }
            di.openProgram(currentProgram);
            DecompileResults result = di.decompileFunction(function, 120, monitor);
            if (!result.decompileCompleted()) throw new IOException(result.getErrorMessage());
            try (PrintWriter writer = new PrintWriter(new FileWriter(new File(output,
                    currentProgram.getName() + "_" + start + "_joined.c")))) {
                writer.print(result.getDecompiledFunction().getC());
            }
            println("Decompiled joined function " + start);
            di.closeProgram();
        }
        di.dispose();
    }
}
