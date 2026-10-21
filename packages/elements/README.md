# @geastack/elements

Non-intrinsic GeaStack JSX elements, each with its TypeScript component and the
native backend that renders it.

```sh
npm install @geastack/elements
```

- `VirtualList`: a windowed list that renders a fixed pool of 16 rows from an
  item template and recycles them as the list scrolls. The pool starts
  `overscan` rows (default 2) above the first visible row, so with the default
  a list taller than 13 rows shows a blank band at the bottom.
- `CameraView`: a live camera preview bound to the host camera service. The
  `CameraController` types are re-exported from `@geastack/core`.

The native element owns the scroll offset. `VirtualList` does not track it, so
copy the element's `scrollTop` into the `scrollTop` prop from `onScroll`; the
component uses it to place the recycled rows. The rows are absolutely
positioned and do not size the list, so give it a height through `class`
(here `.rows { height: 100vh; }` in the app's stylesheet); without one the list
is 0 px tall.

```tsx
import { Store, type VirtualListElement } from '@geastack/core'
import { VirtualList } from '@geastack/elements'

class RowsStore extends Store {
  titles = ['First', 'Second', 'Third']
  scrollTop = 0
}

const rows = new RowsStore()

export function Rows() {
  return (
    <VirtualList
      id="rows"
      class="rows"
      itemCount={rows.titles.length}
      itemHeight={48}
      scrollTop={rows.scrollTop}
      itemTemplate={(index) => <span>{rows.titles[index]}</span>}
      onScroll={() => {
        const list = document.getElementById('rows') as VirtualListElement | null
        if (list) rows.scrollTop = list.scrollTop
      }}
    />
  )
}
```

The native backends are compiled into a target through `@geastack/core`'s
source manifest; apps only import the components.

## License

Apache-2.0. See `LICENSE`.
