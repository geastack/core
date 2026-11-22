# @geastack/elements

Non-intrinsic GeaStack JSX elements, each with its TypeScript component and the
native backend that renders it.

```sh
npm install @geastack/elements
```

- `VirtualList`: a windowed list that materializes only the visible rows from
  an item template.
- `CameraView`: a live camera preview bound to the host camera service. The
  `CameraController` types are re-exported from `@geastack/core`.

```tsx
import { Component, Store, type VirtualListElement } from '@geastack/core'
import { VirtualList } from '@geastack/elements'
import { Text } from '@geastack/engine'

class ListScroll extends Store {
  scrollTop = 0
  sync() {
    const list = document.getElementById('rows') as VirtualListElement | null
    if (list) this.scrollTop = list.scrollTop
  }
}
const scroll = new ListScroll()

export class App extends Component {
  template() {
    return (
      <VirtualList
        id="rows"
        itemCount={1000}
        itemHeight={48}
        scrollTop={scroll.scrollTop}
        onScroll={() => scroll.sync()}
        itemTemplate={(index) => <Text>{`Row ${index}`}</Text>}
      />
    )
  }
}
```

`VirtualList` renders a native `<virtual-list>` scrolling over
`itemCount * itemHeight` and recycles a fixed pool of row slots. The app owns
`scrollTop`: read it from the element in `onScroll` and pass it back in.
`itemTemplate(index, top)` renders the row at `index`; `slotCount`, `overscan`,
`class`, `itemClass` and the touch handlers are optional
(`components/VirtualList.d.ts`).

The native backends are compiled into a target through `@geastack/core`'s
source manifest; apps only import the components.

## License

Apache-2.0. See `LICENSE`.
