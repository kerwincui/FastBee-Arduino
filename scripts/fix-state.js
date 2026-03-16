const fs = require('fs');
const filePath = 'data/www/js/state.js';
let content = fs.readFileSync(filePath, 'utf8');

// ===== Fix 1: Replace appState.xxxFromPeriphModal with app.xxx in onclick =====
content = content.replace(/appState[.]editExecRuleFromPeriphModal/g, 'app.editExecRule');
content = content.replace(/appState[.]toggleExecRuleFromPeriphModal/g, 'app.toggleExecRule');
content = content.replace(/appState[.]deleteExecRuleFromPeriphModal/g, 'app.deleteExecRule');
console.log('Fix 1: onclick replacements done');

// ===== Fix 2: Fix saveExecRule - replace closeExecRuleModal+loadExecRules =====
content = content.replace(
  /this\.closeExecRuleModal\(\);\s*\n\s*this\.loadExecRules\(\);/,
  "const periphId = document.getElementById('peripheral-original-id').value;\n                    this.closeExecRuleModal();\n                    if (periphId) this.loadPeriphModalExecRules(periphId);"
);
console.log('Fix 2: saveExecRule done');

// ===== Fix 3: Fix toggleExecRule - replace loadExecRules+_execRuleContext block =====
content = content.replace(
  /this\.loadExecRules\(\);\s*\n\s*if \(this\._execRuleContext\) \{\s*\n\s*this\.loadPeriphModalExecRules\(this\._execRuleContext\.periphId\);\s*\n\s*this\._execRuleContext = null;\s*\n\s*\}/,
  "const periphId = document.getElementById('peripheral-original-id').value;\n                    if (periphId) this.loadPeriphModalExecRules(periphId);"
);
console.log('Fix 3: toggleExecRule done');

// ===== Fix 4: Delete FromPeriphModal methods block =====
let lines = content.split('\n');
let startIdx = -1, endIdx = -1;
for (let i = 0; i < lines.length; i++) {
  if (lines[i].includes('openExecRuleFromPeriphModal()')) {
    startIdx = i;
  }
  if (startIdx >= 0 && lines[i].includes('toggleExecRuleFromPeriphModal(id, enable)')) {
    for (let j = i; j < lines.length; j++) {
      if (lines[j].trim() === '},') { endIdx = j; break; }
    }
    break;
  }
}
if (startIdx >= 0 && endIdx >= 0) {
  if (startIdx > 0 && lines[startIdx - 1].trim() === '') startIdx--;
  lines.splice(startIdx, endIdx - startIdx + 1);
  console.log('Fix 4: Deleted FromPeriphModal methods, lines', startIdx + 1, '-', endIdx + 1);
} else {
  console.log('Fix 4: FromPeriphModal block not found (may already be deleted)');
}

// ===== Fix 5: Remove trailing garbage after final }; =====
content = lines.join('\n');
const lastValid = content.lastIndexOf('\n};');
if (lastValid >= 0) {
  content = content.substring(0, lastValid + 3) + '\n';
  console.log('Fix 5: Truncated at final };');
}

// Write and verify
fs.writeFileSync(filePath, content);

const remaining = content.match(/FromPeriphModal|_execRuleContext|this\.loadExecRules\b|appState\./g);
if (remaining) {
  console.log('WARNING remaining:', [...new Set(remaining)]);
} else {
  console.log('ALL FIXES VERIFIED: no old patterns remaining');
}
