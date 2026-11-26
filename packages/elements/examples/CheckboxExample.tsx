import { Component, Store } from '@geastack/core'
import { Checkbox } from '../components/Checkbox'

class CheckboxExampleStore extends Store {
  emailUpdates = false
  productUpdates = false
  productUpdatesIndeterminate = true

  setEmailUpdates(value: boolean) {
    this.emailUpdates = value
  }

  setProductUpdates(value: boolean) {
    this.productUpdates = value
    this.productUpdatesIndeterminate = false
  }
}

const settings = new CheckboxExampleStore()

/** Runnable component example for the @geastack/elements Checkbox. */
export class CheckboxExample extends Component {
  template() {
    return (
      <div style={{ display: 'flex', flexDirection: 'column', gap: 12, padding: 16 }}>
        <Checkbox
          label="Email notifications"
          checked={settings.emailUpdates}
          onChange={(value) => settings.setEmailUpdates(value)}
        />
        <Checkbox
          label="Product updates"
          checked={settings.productUpdates}
          indeterminate={settings.productUpdatesIndeterminate}
          onChange={(value) => settings.setProductUpdates(value)}
        />
        <Checkbox label="Disabled option" checked disabled />
      </div>
    )
  }
}
