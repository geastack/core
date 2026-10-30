import { jsMode } from './mode.js'
import { plain } from './plain'

export function invariant(message: string): void {
  if (process.env.NODE_ENV !== 'production') throw new Error(message)
}
export const development = process.env.NODE_ENV === 'development'
export const fromGlobal = globalThis.process.env.NODE_ENV
export const text = 'process.env.NODE_ENV'
export function shadowed(process: { env: { NODE_ENV: string } }): string {
  return process.env.NODE_ENV
}
export function write(): void {
  process.env.NODE_ENV = 'test'
}
export { jsMode, plain }
