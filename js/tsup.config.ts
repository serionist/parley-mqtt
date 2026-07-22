import { defineConfig } from 'tsup';

// Dual ESM + CJS build with generated .d.ts (protocol core is dependency-free).
// Target ES2020 for the edge-engine floor (Node >= 18, Deno/Bun, workerd, QuickJS-ng).
export default defineConfig({
  entry: ['src/index.ts'],
  format: ['esm', 'cjs'],
  target: 'es2020',
  dts: true,
  sourcemap: true,
  clean: true,
  splitting: false,
  treeshake: true,
  outExtension({ format }) {
    return { js: format === 'cjs' ? '.cjs' : '.js' };
  },
});
