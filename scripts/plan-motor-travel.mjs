import fs from 'node:fs/promises';
import { pathToFileURL } from 'node:url';
import { calculateMotorTravel } from './lib/motor-travel-calibration.mjs';

export async function planMotorTravel(args) {
  const input = args.find(arg => arg.startsWith('--measurements='))?.slice(15);
  const output = args.find(arg => arg.startsWith('--out='))?.slice(6);
  if (!input || !output) throw new Error('Use --measurements=<measured-input.json> --out=<unused-review.json>; no device I/O');
  const result = calculateMotorTravel(JSON.parse(await fs.readFile(input, 'utf8')));
  await fs.writeFile(output, JSON.stringify(result, null, 2) + '\n', { flag: 'wx' });
  return result;
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  planMotorTravel(process.argv.slice(2)).then(result => console.log(JSON.stringify(result))).catch(error => {
    console.error(error.message);
    process.exitCode = 1;
  });
}
