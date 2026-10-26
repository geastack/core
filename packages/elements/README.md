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
import { VirtualList } from '@geastack/elements'

<VirtualList items={rows} template={(row) => <Text>{row.title}</Text>} />
```

The native backends are compiled into a target through `@geastack/core`'s
source manifest; apps only import the components.

To add a component, follow the
[component contribution contract](https://github.com/geastack/core/blob/main/docs/COMPONENTS.md).

## License

Apache-2.0. See `LICENSE`.
