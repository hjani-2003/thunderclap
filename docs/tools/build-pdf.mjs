// Prints docs/guide/guide.html to docs/thunderclap-user-guide.pdf with headless Chrome.
//   node docs/tools/build-pdf.mjs [input.html] [output.pdf]
import { writeFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { launch } from './cdp.mjs';

const input = resolve(process.argv[2] || 'docs/guide/guide.html');
const output = resolve(process.argv[3] || 'docs/thunderclap-user-guide.pdf');

const b = await launch();
await b.goto(pathToFileURL(input).href, 500);
await b.eval('document.fonts.ready.then(() => true)');
// Wait for every screenshot to finish decoding before printing.
await b.eval(`Promise.all([...document.images].map((i) => i.decode().catch(() => null))).then(() => true)`);
writeFileSync(output, await b.pdf({ generateDocumentOutline: true, generateTaggedPDF: true }));
await b.close();
console.log('wrote', output);
