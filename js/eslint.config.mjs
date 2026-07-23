// @ts-check
import js from '@eslint/js';
import globals from 'globals';
import tseslint from 'typescript-eslint';

export default tseslint.config(
  {
    // Replacement for .eslintignore.
    // testpeer/ is the e2e interop harness, not shipped code: it is exercised end-to-end by
    // the docker matrix rather than type-linted here (it has never been under a tsconfig
    // project). Linting what ships — src/ and test/ — is the gate that matters.
    ignores: ['dist/**', 'node_modules/**', 'testpeer/**'],
  },
  js.configs.recommended,
  ...tseslint.configs.recommendedTypeChecked,
  {
    languageOptions: {
      parserOptions: {
        projectService: {
          // Root-level tool configs are not part of the src/test tsconfig program.
          allowDefaultProject: ['*.config.ts', '*.config.mjs'],
        },
        tsconfigRootDir: import.meta.dirname,
      },
    },
    rules: {
      // The unhandled-rejection / no-floating-promises discipline is load-bearing for this library.
      '@typescript-eslint/no-floating-promises': 'error',
      // Fire-and-forget dispatch (handler.runCommand) is deliberate and guarded; allow explicit voiding.
      '@typescript-eslint/no-confusing-void-expression': 'off',
    },
  },
  {
    // Core must not touch Node built-ins (edge-engine floor).
    files: ['src/**/*.ts'],
    languageOptions: {
      globals: { ...globals.browser },
    },
    rules: {
      'no-restricted-imports': [
        'error',
        {
          patterns: [
            {
              group: ['node:*', 'fs', 'path', 'crypto', 'util', 'os', 'stream', 'buffer'],
              message: 'The Parley core must not depend on Node built-ins (edge-engine floor).',
            },
          ],
        },
      ],
    },
  },
  {
    files: ['test/**/*.ts', '*.config.ts', 'eslint.config.mjs'],
    languageOptions: {
      globals: { ...globals.node },
    },
  },
);
