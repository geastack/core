// These declarations identify package-local CommonJS wrapper cells to geatsc.
// Static package loading is implemented by the compiler's module records.
export {};

declare global {
  var require: (specifier: string) => any;
  var exports: any;
  var module: { exports: any };
}
