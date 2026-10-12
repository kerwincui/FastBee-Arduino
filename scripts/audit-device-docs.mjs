import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import audit from './lib/device-doc-audit.js';

const args = process.argv.slice(2);
function option(name) {
    const index = args.indexOf(name);
    return index < 0 ? null : args[index + 1];
}
const docsRoot = option('--docs-root');
if (!docsRoot || !fs.existsSync(docsRoot)) {
    console.error('Usage: node scripts/audit-device-docs.mjs --docs-root <FastBee-doc/src/device> [--review-dir <directory>] [--output <report.json>]');
    process.exit(2);
}
const firmwareRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const reviewDir = option('--review-dir');
const reviews = reviewDir ? fs.readdirSync(reviewDir).filter(name => name.endsWith('-review.json')).sort()
    .map(name => JSON.parse(fs.readFileSync(path.join(reviewDir, name), 'utf8'))) : [];
if (reviewDir && !reviews.length) throw new Error('No *-review.json files in review directory');
const result = audit.auditDeviceDocs({ docsRoot: path.resolve(docsRoot), firmwareRoot, reviews });
const output = option('--output');
if (output) {
    fs.mkdirSync(path.dirname(path.resolve(output)), { recursive: true });
    fs.writeFileSync(output, JSON.stringify(result, null, 2) + '\n');
}
console.log(`Device documents: files=${result.documents}, reviewed=${result.reviewedDocuments}, json=${result.jsonBlocks}, links=${result.localLinks}, passed=${result.passed}`);
result.issues.forEach(issue => console.error(issue));
process.exitCode = result.passed ? 0 : 1;
