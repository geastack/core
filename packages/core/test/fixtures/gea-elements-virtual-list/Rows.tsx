import { Store, type VirtualListElement } from '@geastack/core'
import { VirtualList } from '@geastack/elements'

// The @geastack/elements VirtualList used the way its README shows: the app
// keeps scrollTop in a Store and copies the element's offset back on scroll.
class RowsStore extends Store {
  scrollTop = 0
}

const rows = new RowsStore()

export function Rows() {
  return (
    <VirtualList
      id="rows"
      class="rows"
      itemCount={100}
      itemHeight={40}
      scrollTop={rows.scrollTop}
      itemTemplate={(index) => <span class="row-label">{'Row ' + index}</span>}
      onScroll={() => {
        const list = document.getElementById('rows') as VirtualListElement | null
        if (list) rows.scrollTop = list.scrollTop
      }}
    />
  )
}
