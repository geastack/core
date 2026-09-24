import type { ClassValue, Event, TouchEventHandler } from '@geastack/core'

// A windowing list backed by the native <virtual-list> element. The element is
// an ordinary overflow:scroll container that scrolls over a virtual content
// height of itemCount * itemHeight; this component only materializes a small
// fixed pool of `slotCount` real row nodes and recycles them as `scrollTop`
// changes. Each slot is absolutely positioned in *content* space (top =
// index * itemHeight) — the element's native scroll offset shifts it on screen,
// so slot positions stay put between recycles and the fast scroll path can
// blit instead of re-rendering. Row content, height, colours and fonts all come
// from `itemTemplate` + its CSS; the framework owns none of it.

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

const SLOT_COUNT = 16

function vlWindowStart(scrollTop: number, itemHeight: number, overscan: number) {
  let start = Math.floor(scrollTop / itemHeight) - overscan
  if (start < 0) start = 0
  return start
}

// Data index shown by slot `slotIndex` for the current scroll position. Within
// the visible window [start, start + SLOT_COUNT) each index maps to slot
// index % SLOT_COUNT, so advancing the window by one row recycles exactly one
// slot (its index jumps by SLOT_COUNT) and leaves the rest untouched.
function vlIndexForSlot(slotIndex: number, scrollTop: number, itemHeight: number, overscan: number) {
  const start = vlWindowStart(scrollTop, itemHeight, overscan)
  return start + (((slotIndex - start) % SLOT_COUNT) + SLOT_COUNT) % SLOT_COUNT
}

function vlSlotTop(slotIndex: number, scrollTop: number, itemHeight: number, overscan: number) {
  return vlIndexForSlot(slotIndex, scrollTop, itemHeight, overscan) * itemHeight
}

function vlSlotVisible(slotIndex: number, scrollTop: number, itemHeight: number, overscan: number, itemCount: number) {
  return vlIndexForSlot(slotIndex, scrollTop, itemHeight, overscan) < itemCount
}

interface VirtualListSlotProps {
  slotIndex: number
  scrollTop: number
  itemHeight: number
  overscan: number
  itemCount: number
  itemClass?: ClassValue
  itemTemplate: VirtualListItemTemplate
}

function VirtualListSlot({ slotIndex, scrollTop, itemHeight, overscan, itemCount, itemClass, itemTemplate }: VirtualListSlotProps) {
  return (
    <div
      class={itemClass}
      style={{
        position: 'absolute',
        left: 0,
        top: vlSlotTop(slotIndex, scrollTop, itemHeight, overscan),
        width: '100vw',
        height: itemHeight,
        display: vlSlotVisible(slotIndex, scrollTop, itemHeight, overscan, itemCount) ? 'flex' : 'none'
      }}
    >
      {itemTemplate(
        vlIndexForSlot(slotIndex, scrollTop, itemHeight, overscan),
        vlSlotTop(slotIndex, scrollTop, itemHeight, overscan)
      )}
    </div>
  )
}

export default function VirtualList({
  itemCount,
  itemHeight,
  scrollTop,
  overscan,
  id,
  class: cls,
  itemClass,
  itemTemplate,
  onTouchStart,
  onTouchMove,
  onTouchEnd,
  onScroll
}: VirtualListProps) {
  const rowOverscan = overscan === undefined ? 2 : overscan
  return (
    <virtual-list
      id={id}
      class={cls}
      item-count={itemCount}
      onTouchStart={onTouchStart}
      onTouchMove={onTouchMove}
      onTouchEnd={onTouchEnd}
      onScroll={onScroll}
    >
      <VirtualListSlot slotIndex={0} scrollTop={scrollTop} itemHeight={itemHeight} overscan={rowOverscan} itemCount={itemCount} itemClass={itemClass} itemTemplate={itemTemplate} />
      <VirtualListSlot slotIndex={1} scrollTop={scrollTop} itemHeight={itemHeight} overscan={rowOverscan} itemCount={itemCount} itemClass={itemClass} itemTemplate={itemTemplate} />
      <VirtualListSlot slotIndex={2} scrollTop={scrollTop} itemHeight={itemHeight} overscan={rowOverscan} itemCount={itemCount} itemClass={itemClass} itemTemplate={itemTemplate} />
      <VirtualListSlot slotIndex={3} scrollTop={scrollTop} itemHeight={itemHeight} overscan={rowOverscan} itemCount={itemCount} itemClass={itemClass} itemTemplate={itemTemplate} />
      <VirtualListSlot slotIndex={4} scrollTop={scrollTop} itemHeight={itemHeight} overscan={rowOverscan} itemCount={itemCount} itemClass={itemClass} itemTemplate={itemTemplate} />
      <VirtualListSlot slotIndex={5} scrollTop={scrollTop} itemHeight={itemHeight} overscan={rowOverscan} itemCount={itemCount} itemClass={itemClass} itemTemplate={itemTemplate} />
      <VirtualListSlot slotIndex={6} scrollTop={scrollTop} itemHeight={itemHeight} overscan={rowOverscan} itemCount={itemCount} itemClass={itemClass} itemTemplate={itemTemplate} />
      <VirtualListSlot slotIndex={7} scrollTop={scrollTop} itemHeight={itemHeight} overscan={rowOverscan} itemCount={itemCount} itemClass={itemClass} itemTemplate={itemTemplate} />
      <VirtualListSlot slotIndex={8} scrollTop={scrollTop} itemHeight={itemHeight} overscan={rowOverscan} itemCount={itemCount} itemClass={itemClass} itemTemplate={itemTemplate} />
      <VirtualListSlot slotIndex={9} scrollTop={scrollTop} itemHeight={itemHeight} overscan={rowOverscan} itemCount={itemCount} itemClass={itemClass} itemTemplate={itemTemplate} />
      <VirtualListSlot slotIndex={10} scrollTop={scrollTop} itemHeight={itemHeight} overscan={rowOverscan} itemCount={itemCount} itemClass={itemClass} itemTemplate={itemTemplate} />
      <VirtualListSlot slotIndex={11} scrollTop={scrollTop} itemHeight={itemHeight} overscan={rowOverscan} itemCount={itemCount} itemClass={itemClass} itemTemplate={itemTemplate} />
      <VirtualListSlot slotIndex={12} scrollTop={scrollTop} itemHeight={itemHeight} overscan={rowOverscan} itemCount={itemCount} itemClass={itemClass} itemTemplate={itemTemplate} />
      <VirtualListSlot slotIndex={13} scrollTop={scrollTop} itemHeight={itemHeight} overscan={rowOverscan} itemCount={itemCount} itemClass={itemClass} itemTemplate={itemTemplate} />
      <VirtualListSlot slotIndex={14} scrollTop={scrollTop} itemHeight={itemHeight} overscan={rowOverscan} itemCount={itemCount} itemClass={itemClass} itemTemplate={itemTemplate} />
      <VirtualListSlot slotIndex={15} scrollTop={scrollTop} itemHeight={itemHeight} overscan={rowOverscan} itemCount={itemCount} itemClass={itemClass} itemTemplate={itemTemplate} />
    </virtual-list>
  )
}
