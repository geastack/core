import type { ClassValue, Event, TouchEventHandler } from '@geastack/core'

export type VirtualListItemTemplate = (index: number, top: number) => any

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
