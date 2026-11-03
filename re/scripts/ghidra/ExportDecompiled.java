// Decompiles every function in the program and writes pseudo-C grouped by C++ class,
// plus an index, a string->function xref table and the recovered class data types.
// usage (headless): -postScript ExportDecompiled.java <output_dir>
//@category Export

import java.io.*;
import java.util.*;

import ghidra.app.decompiler.*;
import ghidra.app.decompiler.parallel.*;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.data.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.util.task.TaskMonitor;

public class ExportDecompiled extends GhidraScript {

	static final int GLOBAL_CHUNK = 0x10000; // global functions are grouped by 64KB address range

	static class Result {
		Function func;
		String code;
		Result(Function f, String c) { func = f; code = c; }
	}

	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		File outDir = new File(args.length > 0 ? args[0] : askDirectory("Output", "Select").getPath());
		File classDir = new File(outDir, "classes");
		File globalDir = new File(outDir, "global");
		classDir.mkdirs();
		globalDir.mkdirs();

		FunctionManager fm = currentProgram.getFunctionManager();
		List<Function> funcs = new ArrayList<>();
		for (Function f : fm.getFunctions(true)) {
			if (!f.isExternal() && !f.isThunk()) {
				funcs.add(f);
			}
		}
		println("Decompiling " + funcs.size() + " functions");

		DecompilerCallback<Result> cb = new DecompilerCallback<>(currentProgram, decomp -> {
			DecompileOptions opts = new DecompileOptions();
			opts.grabFromProgram(currentProgram);
			decomp.setOptions(opts);
			decomp.toggleCCode(true);
			decomp.toggleSyntaxTree(true);
			decomp.setSimplificationStyle("decompile");
		}) {
			@Override
			public Result process(DecompileResults res, TaskMonitor m) throws Exception {
				Function f = res.getFunction();
				DecompiledFunction df = res.getDecompiledFunction();
				String code = df != null ? df.getC()
						: "/* decompilation failed: " + res.getErrorMessage() + " */\n";
				return new Result(f, code);
			}
		};
		cb.setTimeout(180);
		List<Result> results = ParallelDecompiler.decompileFunctions(cb, funcs, monitor);
		cb.dispose();
		results.sort(Comparator.comparing(r -> r.func.getEntryPoint()));

		// group into files
		Map<String, StringBuilder> files = new TreeMap<>();
		StringBuilder index = new StringBuilder("address\tsize\tnamespace\tname\tfile\tsignature\n");
		for (Result r : results) {
			Function f = r.func;
			Namespace ns = f.getParentNamespace();
			String fileName;
			if (ns != null && !ns.isGlobal() && !(ns instanceof Library)) {
				fileName = "classes/" + ns.getName(true).replaceAll("[^A-Za-z0-9_.-]+", "_") + ".c";
			}
			else {
				long off = f.getEntryPoint().getOffset() & ~(long) (GLOBAL_CHUNK - 1);
				fileName = String.format("global/%08X.c", off);
			}
			StringBuilder sb = files.computeIfAbsent(fileName, k -> new StringBuilder());
			sb.append("// ==== ").append(f.getEntryPoint()).append("  ")
					.append(f.getName(true)).append("  (")
					.append(f.getBody().getNumAddresses()).append(" bytes)\n");
			sb.append(r.code).append("\n");
			index.append(f.getEntryPoint()).append('\t')
					.append(f.getBody().getNumAddresses()).append('\t')
					.append(ns == null ? "" : ns.getName(true)).append('\t')
					.append(f.getName()).append('\t')
					.append(fileName).append('\t')
					.append(f.getPrototypeString(false, false).replace('\t', ' ')).append('\n');
		}
		for (Map.Entry<String, StringBuilder> e : files.entrySet()) {
			write(new File(outDir, e.getKey()), e.getValue().toString());
		}
		write(new File(outDir, "index.tsv"), index.toString());
		println("Wrote " + files.size() + " source files");

		writeStrings(new File(outDir, "strings.tsv"));
		writeVftables(new File(outDir, "vftables.tsv"));
		writeTypes(new File(outDir, "types.h"));
	}

	private void writeStrings(File out) throws IOException {
		StringBuilder sb = new StringBuilder("address\tvalue\treferenced_from\n");
		ReferenceManager rm = currentProgram.getReferenceManager();
		FunctionManager fm = currentProgram.getFunctionManager();
		DataIterator it = currentProgram.getListing().getDefinedData(true);
		while (it.hasNext() && !monitor.isCancelled()) {
			Data d = it.next();
			if (!d.hasStringValue()) {
				continue;
			}
			Object v = d.getValue();
			String s = String.valueOf(v).replace("\\", "\\\\").replace("\n", "\\n")
					.replace("\r", "\\r").replace("\t", "\\t");
			Set<String> refs = new TreeSet<>();
			for (Reference ref : rm.getReferencesTo(d.getAddress())) {
				Function f = fm.getFunctionContaining(ref.getFromAddress());
				refs.add(f != null ? f.getName(true) + "@" + f.getEntryPoint()
						: ref.getFromAddress().toString());
			}
			sb.append(d.getAddress()).append('\t').append(s).append('\t')
					.append(String.join(", ", refs)).append('\n');
		}
		write(out, sb.toString());
	}

	private void writeVftables(File out) throws IOException {
		StringBuilder sb = new StringBuilder("class\tvftable\tslot\tfunction\n");
		SymbolTable st = currentProgram.getSymbolTable();
		FunctionManager fm = currentProgram.getFunctionManager();
		for (Symbol sym : st.getDefinedSymbols()) {
			if (!sym.getName().startsWith("vftable")) {
				continue;
			}
			Address a = sym.getAddress();
			String cls = sym.getParentNamespace().getName(true) + (sym.getName().equals("vftable") ? "" : "::" + sym.getName());
			for (int slot = 0; slot < 512; slot++) {
				Address entry = a.add(slot * 4L);
				if (slot > 0 && st.getPrimarySymbol(entry) != null
						&& st.getPrimarySymbol(entry).getSource() != SourceType.DEFAULT
						&& !st.getPrimarySymbol(entry).getName().startsWith("vftable")) {
					// stop at the next labelled object
					break;
				}
				int ptr;
				try {
					ptr = currentProgram.getMemory().getInt(entry);
				}
				catch (Exception ex) {
					break;
				}
				Address target = a.getNewAddress(ptr & 0xffffffffL);
				Function f = fm.getFunctionAt(target);
				if (f == null) {
					break;
				}
				sb.append(cls).append('\t').append(a).append('\t').append(slot).append('\t')
						.append(f.getName(true)).append('@').append(target).append('\n');
			}
		}
		write(out, sb.toString());
	}

	private void writeTypes(File out) throws Exception {
		DataTypeManager dtm = currentProgram.getDataTypeManager();
		List<DataType> types = new ArrayList<>();
		Category root = dtm.getCategory(new CategoryPath("/ClassDataTypes"));
		if (root != null) {
			collect(root, types);
		}
		try (PrintWriter pw = new PrintWriter(new FileWriter(out))) {
			DataTypeWriter w = new DataTypeWriter(dtm, pw);
			w.write(types, monitor);
		}
		println("Wrote " + types.size() + " class data types");
	}

	private void collect(Category c, List<DataType> out) {
		out.addAll(Arrays.asList(c.getDataTypes()));
		for (Category sub : c.getCategories()) {
			collect(sub, out);
		}
	}

	private static void write(File f, String s) throws IOException {
		f.getParentFile().mkdirs();
		try (Writer w = new OutputStreamWriter(new FileOutputStream(f), "UTF-8")) {
			w.write(s);
		}
	}
}
