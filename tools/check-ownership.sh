#!/bin/sh
# GPL-3.0-or-later. Source boundary gate; the compiler enforces opaque types too.
set -eu
cd "$(dirname "$0")/.."
exec bun --eval '
const root = process.argv[1] || ".";
const failures = [];
const legacy = new Map();
// An intentional refusal test is not a legacy production authority path.
// Counts remain an upper bound: a second call in the same test still fails.
const testControls = new Map([
  ["tests/test_session.c:host_raw_cmd:0x0408", 1]
]);
const observed = new Map();
function scan(path, original, allowLegacy = true) {
  const code = original.replace(/\/\*[\s\S]*?\*\/|\/\/[^\n]*/g, x => x.replace(/[^\n]/g, " "));
  const text = code.replace(/"(?:\\.|[^"\\])*"/g, x => x.replace(/[^\n]/g, " "));
  const errors = [];
  const lineAt = index => text.slice(0, index).split("\n").length;
  function fail(index, why) { errors.push(`${path}:${lineAt(index)}: ${why}`); }
  if (path !== "src/ownership.c") {
    for (const m of code.matchAll(/^\s*#\s*include\s*["<][^">\n]*ownership\.c[">]/gm))
      fail(m.index, "private ownership implementation include");
    for (const m of text.matchAll(/\bstruct\s+own_ledger\s*\{|\bown_record\b|\bown_link\b|(?:\.|->)\s*_opaque\b/g))
      fail(m.index, "private ownership representation");
    const names = [...text.matchAll(/\bown_ledger\s*\*\s*(\w+)/g)].map(m => m[1]);
    for (const name of names) {
      const re = new RegExp(`\\b${name}\\s*->\\s*\\w+`, "g");
      for (const m of text.matchAll(re)) fail(m.index, "opaque ledger dereference");
    }
  }
  if (path !== "src/ownership.c" && path !== "src/ownership.h" && !path.startsWith("tests/")) {
    for (const m of text.matchAll(/\b(?:own_clone|own_canonical|own_model_copy)\b/g))
      fail(m.index, "test-only ledger representation outside model tests");
    for (const m of text.matchAll(/\b(own_backend_receipt|own_backend_local_result|own_backend_identity)\b/g)) {
      const allowed = m[1] === "own_backend_receipt" ? "src/hci_usb.c" : "src/ps5_vpad.c";
      if (path !== allowed) fail(m.index, `backend-only function ${m[1]}`);
    }
  }
  // Inspect all function calls, including multiline calls and locally aliased
  // A2 constants. Do not confuse a VID/PID, a comparison or a declaration with
  // a submission. Runtime classification additionally defaults to deny.
  const banned = new Set(["OP_INQUIRY_CANCEL", "OP_CREATE_CONN_CANCEL",
    "OP_CREATE_CONNECTION_CANCEL", "OP_LE_CREATE_CONN_CANCEL", "OP_RESET"]);
  const number = raw => {
    const value = raw.replace(/[uUlL]+$/, "");
    return /^0x[0-9a-f]+$/i.test(value) ? parseInt(value, 16) :
           /^[0-9]+$/.test(value) ? Number(value) : NaN;
  };
  const isBanned = value => banned.has(value) || [0x0402,0x0408,0x200e,0x0c03].includes(number(value));
  let changed;
  do {
    changed = false;
    for (const m of text.matchAll(/(?:#\s*define\s+|\b(?:const\s+)?(?:unsigned(?:\s+int)?|uint16_t|int)\s+)(\w+)\s*(?:=\s*)?(\w+)\s*[;\n]/g)) {
      if (isBanned(m[2]) && !banned.has(m[1])) { banned.add(m[1]); changed = true; }
    }
  } while (changed);
  for (const m of text.matchAll(/\b(\w+)\s*\(/g)) {
    if (["if","while","for","switch","sizeof","CHECK","number","N"].includes(m[1])) continue;
    let end = m.index + m[0].length, depth = 1;
    const start = end;
    while (end < text.length && depth) {
      if (text[end] === "(") depth++;
      if (text[end] === ")") depth--;
      end++;
    }
    const args = text.slice(start, end-1);
    const opcode = (args.match(/\b(?:0x[0-9a-f]+[uUlL]*|\w+)\b/gi) || []).find(isBanned);
    if (!opcode) continue;
    const key = `${path}:${m[1]}:${opcode}`;
    const count = (observed.get(key) || 0) + 1;
    observed.set(key, count);
    if (!allowLegacy || count > (legacy.get(key) || testControls.get(key) || 0))
      fail(m.index, `A2 command passed to ${m[1]}`);
  }
  return errors;
}
// Negative controls execute the exact scanner, not a separate approximation.
const controls = [
  ["src/foreign.c", "void f(own_ledger *l) { l->records[0] = 1; }"],
  ["src/foreign.c", "void f(void) { own_backend_receipt(l, a, 0, r); }"],
  ["src/foreign.c", "void f(void) { own_backend_identity(l, r, 1, 1); }"],
  ["src/foreign.c", "void f(void) { send_cmd(h, 0x200E, 0, 0); }"],
  ["src/foreign.c", "#define ALIAS 0x0408\nvoid f(void) { send_cmd(h, ALIAS, 0, 0); }"],
  ["src/foreign.c", "void f(void) { if (send_cmd(h, (unsigned)0x0402U, 0, 0)) return; }"],
  ["src/foreign.c", "void f(void) { token._opaque[0] = 1; }"],
  ["src/foreign.c", "#include \"ownership.c\"\n"],
  ["src/foreign.c", "void *hook = &own_backend_receipt;"],
  ["src/foreign.c", "void f(void) { own_clone(l); }"],
  ["src/foreign.c", "void f(void) { own_canonical(l, out, n); }"],
  ["src/foreign.c", "void f(void) { own_model_copy(a, b); }"],
  ["src/host.c", "void f(void) { send_cmd(h, OP_RESET, 0, 0); }"]
];
for (const [path,text] of controls) {
  if (!scan(path,text,false).length) throw new Error(`negative control survived: ${text}`);
}
observed.clear();
for await (const path of new Bun.Glob("{src,tests}/**/*.{c,h}").scan({cwd: root})) {
  const errors = scan(path, await Bun.file(`${root}/${path}`).text());
  failures.push(...errors);
}
console.log("legacy allowlist (2ce2933; remove in todos 13-15):");
for (const [key,max] of legacy) console.log(`  ${key}: observed=${observed.get(key)||0} allowed<=${max}`);
console.log("test-only negative controls:");
for (const [key,max] of testControls) console.log(`  ${key}: observed=${observed.get(key)||0} allowed<=${max}`);
if (failures.length) { console.error(failures.join("\n")); process.exit(1); }
console.log(`ownership boundary checks passed; negative-controls=${controls.length}`);
'
