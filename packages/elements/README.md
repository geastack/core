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
- `InlineAlert`: a persistent info, success, warning or error message built
  from plain views and native `<button>` elements, with an optional title,
  action and dismiss button. A visible severity label (`severityLabel`, English
  by default) backs up the colors. Dismissing only calls `onDismiss`; the app
  removes the alert. The root has `role="status"` (`"alert"` for errors) for
  screen readers on the browser target; native targets ignore it. With the
  built-in bitmap font, pixel targets draw ASCII only, so a character such as
  `ü` or `ş` draws as `?` unless the app enables runtime TTF fonts or a
  `--gea-font-charset`. Right-to-left layout is not handled. In a native build,
  an alert inside a conditional branch keeps the props it was first rendered
  with; the InlineAlert demo in geastack/examples shows a shape that updates:
  https://github.com/geastack/examples/tree/main/apps/inline-alert

```tsx
import { VirtualList } from '@geastack/elements'

<VirtualList items={rows} template={(row) => <Text>{row.title}</Text>} />
```

The native backends are compiled into a target through `@geastack/core`'s
source manifest; apps only import the components.

## License

Apache-2.0. See `LICENSE`.
