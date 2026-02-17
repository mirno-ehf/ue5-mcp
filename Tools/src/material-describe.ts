export function describeMaterial(data: any): string {
  const lines: string[] = [];

  if (data.name) {
    lines.push(`# Material: ${data.name}`);
  }

  if (data.description) {
    lines.push(data.description);
    return lines.join("\n");
  }

  // Format inputs array if present
  if (data.inputs && Array.isArray(data.inputs)) {
    for (const input of data.inputs) {
      const connected = input.connected ? "" : " (disconnected)";
      lines.push(`${input.input}: ${input.chain || "empty"}${connected}`);
    }
  }

  if (lines.length <= 1) {
    lines.push("No material input data available.");
  }

  return lines.join("\n");
}
