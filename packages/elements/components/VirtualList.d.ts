import type { ClassValue, Event, GeaJsxElement, TouchEventHandler } from '@geastack/core'

/** Renders row `index`, placed at `top`. Return an element; wrap plain text in one, such as a `<span>`. */
export type VirtualListItemTemplate = (index: number, top: number) => GeaJsxElement

export interface VirtualListProps {
  itemCount: number
  itemHeight: number
  scrollTop: number
  slotCount?: number
  overscan?: number
  id?: string
  class?: ClassValue
  itemClass?: ClassValue
  itemTemplate: VirtualListItemTemplate
  onTouchStart?: TouchEventHandler
  onTouchMove?: TouchEventHandler
  onTouchEnd?: TouchEventHandler
  onScroll?: (event: Event) => void
}

export default function VirtualList(props: VirtualListProps): any
