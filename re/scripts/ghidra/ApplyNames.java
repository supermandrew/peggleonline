// Applies names from a TSV file: address <TAB> name [<TAB> comment]
// Names may be qualified (e.g. Sexy::Board::UpdateShot). Function entries are renamed and moved
// into the namespace; other addresses get a primary label. Lines starting with # are ignored.
// usage (headless): -postScript ApplyNames.java <names.tsv>
//@category Symbol

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;

public class ApplyNames extends GhidraScript {

	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		Path path = Paths.get(args[0]);
		SymbolTable st = currentProgram.getSymbolTable();
		int applied = 0;
		for (String line : Files.readAllLines(path, StandardCharsets.UTF_8)) {
			line = line.strip();
			if (line.isEmpty() || line.startsWith("#")) {
				continue;
			}
			String[] cols = line.split("\t");
			if (cols.length < 2) {
				continue;
			}
			Address addr = toAddr(Long.parseLong(cols[0].replaceFirst("^0x", ""), 16));
			String qualified = cols[1].strip();
			int split = qualified.lastIndexOf("::");
			String name = split < 0 ? qualified : qualified.substring(split + 2);
			Namespace ns = split < 0 ? currentProgram.getGlobalNamespace()
					: getOrCreateNamespace(qualified.substring(0, split));

			Function f = getFunctionAt(addr);
			if (f != null) {
				f.setParentNamespace(ns);
				f.setName(name, SourceType.USER_DEFINED);
				if (cols.length > 2 && !cols[2].isBlank()) {
					f.setComment(cols[2].strip());
				}
			}
			else {
				Symbol s = st.createLabel(addr, name, ns, SourceType.USER_DEFINED);
				s.setPrimary();
				if (cols.length > 2 && !cols[2].isBlank()) {
					setPlateComment(addr, cols[2].strip());
				}
			}
			applied++;
		}
		println("Applied " + applied + " names from " + path);
	}

	private Namespace getOrCreateNamespace(String qualified) throws Exception {
		SymbolTable st = currentProgram.getSymbolTable();
		Namespace parent = currentProgram.getGlobalNamespace();
		for (String part : qualified.split("::")) {
			Namespace next = st.getNamespace(part, parent);
			if (next == null) {
				next = st.createNameSpace(parent, part, SourceType.USER_DEFINED);
			}
			parent = next;
		}
		return parent;
	}
}
