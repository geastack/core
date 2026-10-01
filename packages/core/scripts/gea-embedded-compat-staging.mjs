export const COMPAT_STAGING_IGNORED_DIRECTORIES = Object.freeze([
  'node_modules',
  'dist',
  'build',
  '.build',
  '.build-test',
  '.vite',
  '.scratch',
  '.test-tmp',
  'generated-output',
])

const ignoredDirectorySet = new Set(COMPAT_STAGING_IGNORED_DIRECTORIES)

export function shouldIgnoreCompatStagingDirectory(name) {
  return ignoredDirectorySet.has(name)
}

// The browser JSX runtime declares Element as `any`. Embedded components are
// lowered through @geastack/core's typed global JSX namespace instead. Match
// the Component/Store import conversion without altering custom JSX providers
// or the application's original web tsconfig.
export function normalizeEmbeddedJsxOptions(options) {
  if (options.jsxImportSource === '@geajs/core') delete options.jsxImportSource
}
