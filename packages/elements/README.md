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
- `ProgressBar`: a determinate or indeterminate progress track built from plain
  views, with a `value` clamped to `min`..`max` (0-100 by default), an optional
  label and a `reducedMotion` switch that stills the indeterminate sweep. The
  clamping is exported as `progressFraction(value, min, max)`. The sweep
  distance is fixed in pixels when the animation starts, so a bar resized
  while busy keeps the old distance until `indeterminate` is toggled. The bar
  exposes no role or value to assistive technology and takes no keyboard
  input, so put the value in `label` when the user needs to read it.

```tsx
import { VirtualList } from '@geastack/elements'

<VirtualList items={rows} template={(row) => <Text>{row.title}</Text>} />
```

The native backends are compiled into a target through `@geastack/core`'s
source manifest; apps only import the components.

## License

Apache-2.0. See `LICENSE`.
