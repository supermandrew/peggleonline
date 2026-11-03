// Prints the callers of each given address, following thunk/jump-only functions and tail jumps.
// usage (headless, -readOnly): -postScript Xrefs.java <out.txt> <hexaddr> [<hexaddr> ...]
//@category Analysis

import java.io.*;
import java.util.*;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;

public class Xrefs extends GhidraScript {

	private PrintWriter out;
	private ReferenceManager rm;
	private FunctionManager fm;

	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		rm = currentProgram.getReferenceManager();
		fm = currentProgram.getFunctionManager();
		try (PrintWriter pw = new PrintWriter(new FileWriter(args[0]))) {
			out = pw;
			for (int i = 1; i < args.length; i++) {
				Address a = toAddr(Long.parseLong(args[i].replaceFirst("^0x", ""), 16));
				Function f = fm.getFunctionContaining(a);
				out.println("== " + a + " " + (f != null ? f.getName(true) : "?"));
				Set<Address> seen = new HashSet<>();
				walk(a, 1, seen);
			}
		}
	}

	private void walk(Address target, int depth, Set<Address> seen) {
		if (!seen.add(target) || depth > 6) {
			return;
		}
		for (Reference ref : rm.getReferencesTo(target)) {
			Address from = ref.getFromAddress();
			Function caller = fm.getFunctionContaining(from);
			String who = caller != null ? caller.getName(true) + "@" + caller.getEntryPoint() : "(no function)";
			out.println("  ".repeat(depth) + ref.getReferenceType() + " from " + from + " in " + who);
			Instruction ins = getInstructionAt(from);
			boolean tailJump = ins != null && ins.getFlowType().isJump() && !ins.getFlowType().isConditional();
			boolean thunkLike = caller != null && (caller.isThunk() || caller.getBody().getNumAddresses() <= 8);
			if (caller != null && (tailJump || thunkLike)) {
				walk(caller.getEntryPoint(), depth + 1, seen);
				if (tailJump && !caller.getEntryPoint().equals(from)) {
					walk(from, depth + 1, seen);
				}
			}
			else if (caller == null && tailJump) {
				walk(from, depth + 1, seen);
			}
		}
	}
}
