const fs = require('fs');
const f = 'd:\\project\\gitee\\FastBee-Arduino\\data\\www\\js\\state.js';
let c = fs.readFileSync(f, 'utf8');

// 1. saveExecRule: enabled .value -> .checked
c = c.replace(
  "enabled: document.getElementById('exec-rule-enabled').value,",
  "enabled: document.getElementById('exec-rule-enabled').checked ? '1' : '0',"
);

// 2. saveExecRule: remove sourceId line
c = c.replace(
  "            sourceId: document.getElementById('exec-rule-source-id').value.trim(),\n",
  ""
);

// 3. editExecRule: enabled .value -> .checked
c = c.replace(
  "document.getElementById('exec-rule-enabled').value = rule.enabled ? '1' : '0';",
  "document.getElementById('exec-rule-enabled').checked = !!rule.enabled;"
);

// 4. editExecRule: remove sourceId line
c = c.replace(
  "                document.getElementById('exec-rule-source-id').value = rule.sourceId || '';\n",
  ""
);

// 5. loadPeriphModalExecRules: remove r.sourceId from trigger text
c = c.replace(
  "(r.sourceId || '') + ' ' + (opLabels",
  "(opLabels"
);

fs.writeFileSync(f, c, 'utf8');

// Verify
const after = fs.readFileSync(f, 'utf8');
const remaining = after.match(/sourceId|source-id|exec-rule-source/g);
console.log('Remaining sourceId refs:', remaining ? remaining.length : 0);
const enabledValue = (after.match(/exec-rule-enabled.*\.value/g) || []);
console.log('Remaining .value on enabled:', enabledValue.length);
