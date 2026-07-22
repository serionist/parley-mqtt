import { readdirSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { join } from 'node:path';
import { describe, test } from 'vitest';
import { runVector } from './support/vectorRunner';

// Layer-2 conformance: replay every docs/conformance/*.json vector (discovered dynamically, one
// test per file) against the protocol core, for both roles (protocol.md §10 / testing.md).
const conformanceDir = fileURLToPath(new URL('../../docs/conformance', import.meta.url));
const files = readdirSync(conformanceDir)
  .filter((f) => f.endsWith('.json'))
  .sort();

describe('conformance vectors', () => {
  if (files.length === 0) {
    throw new Error(`No conformance vectors found in ${conformanceDir}`);
  }

  for (const file of files) {
    test(file, async () => {
      await runVector(join(conformanceDir, file));
    });
  }
});
