// --- Type name format documentation (shared across tool descriptions) ---

export const TYPE_NAME_DOCS = `Type name formats: C++ USTRUCTs use F-prefixed name (e.g. 'FVitals', 'FDeviceState'), BP structs (UserDefinedStruct) use asset name (e.g. 'S_Vitals'), enums use enum name (e.g. 'ELungSound').`;

// --- Warning marker for unresolved types ---

export const UNRESOLVED_TYPE_PATTERNS = ["<None>", "<unknown>", "None", "NONE"];

export function flagType(typeName: string): string {
  if (!typeName) return "\u26A0 <None>";
  for (const pat of UNRESOLVED_TYPE_PATTERNS) {
    if (typeName === pat || typeName.includes(pat)) {
      return `\u26A0 ${typeName}`;
    }
  }
  return typeName;
}

export function formatVarType(v: any): string {
  let t = v.type || "unknown";
  if (v.subtype) t = v.subtype;
  if (v.isMap) return `Map<${v.type}, ${v.subtype || "?"}>`;
  if (v.isSet) return `Set<${t}>`;
  if (v.isArray) return `${t}[]`;
  return t;
}

/** Format parameter list for functions/events/delegates with type flagging (#9) */
export function formatParams(params: any[] | undefined): string {
  if (!params || params.length === 0) return "";
  const parts = params.map((p: any) => {
    const name = p.name || "?";
    const type = p.type || p.pinType || "";
    return `${name}: ${flagType(type)}`;
  });
  return ` \u2014 Params: ${parts.join(", ")}`;
}

/** Format updated state returned by mutation tools (#11) */
export function formatUpdatedState(data: any): string[] {
  const lines: string[] = [];
  if (data.updatedState) {
    lines.push(`\nUpdated state:`);
    const state = data.updatedState;
    if (state.variables?.length) {
      lines.push(`  Variables: ${state.variables.map((v: any) => `${v.name}: ${v.type}`).join(", ")}`);
    }
    if (state.pins?.length) {
      lines.push(`  Pins:`);
      for (const pin of state.pins) {
        lines.push(`    ${pin.direction === "Output" ? "\u2192" : "\u2190"} ${pin.name}: ${pin.type}${pin.subtype ? ` (${pin.subtype})` : ""}`);
      }
    }
    if (state.nodeCount !== undefined) {
      lines.push(`  Nodes: ${state.nodeCount}`);
    }
    if (state.graphCount !== undefined) {
      lines.push(`  Graphs: ${state.graphCount}`);
    }
  }
  return lines;
}
