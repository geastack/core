import { Store } from '@geastack/core'
import { InlineAlert, type InlineAlertVariant } from '@geastack/elements'

// One alert per behavior the native test checks. The app owns visibility:
// only the error alert is removed when it reports onDismiss.
class AlertsStore extends Store {
  installs = 0
  dismissCalls = 0
  errorOpen = true
  plainVariant: InlineAlertVariant = 'error'
}

const alerts = new AlertsStore()

export function Alerts() {
  return (
    <div class="alerts">
      <InlineAlert
        id="alert-info"
        class="alert-info"
        title="Update ready"
        message="Version 2 is ready to install."
        actionLabel="Install"
        onAction={() => {
          alerts.installs = alerts.installs + 1
        }}
      />
      <InlineAlert
        id="alert-success"
        class="alert-success"
        variant="success"
        title={undefined}
        message="Settings saved."
        dismissible={true}
        onDismiss={() => {
          alerts.dismissCalls = alerts.dismissCalls + 1
        }}
      />
      <InlineAlert
        id="alert-warning"
        class="alert-warning narrow"
        variant="warning"
        message="The battery is low. Connect the charger soon or the device will turn off."
      />
      {alerts.errorOpen ? (
        <InlineAlert
          id="alert-error"
          class="alert-error"
          variant="error"
          title="Yükleme başarısız"
          severityLabel="Hata"
          message="Dosya gönderilemedi."
          dismissible={true}
          dismissLabel="Kapat"
          onDismiss={() => {
            alerts.errorOpen = false
          }}
        />
      ) : null}
      <InlineAlert id="alert-plain" class="alert-plain" variant={alerts.plainVariant} message="No connection." />
      <InlineAlert
        id="alert-empty"
        class="alert-empty"
        variant="info"
        title=""
        message=""
        actionLabel=""
        severityLabel=""
        dismissible={true}
        dismissLabel=""
      />
      <InlineAlert
        id="alert-pair"
        class="alert-pair"
        variant="success"
        message="Item deleted."
        actionLabel="Undo"
        onAction={() => {
          alerts.installs = alerts.installs + 100
        }}
        dismissible={true}
        dismissLabel="Close"
      />
      <InlineAlert
        id="alert-both"
        class="alert-both narrow"
        variant="warning"
        title="Storage is almost full on this device"
        message="Free some space."
        actionLabel="Free up space"
        onAction={() => {
          alerts.installs = alerts.installs + 10
        }}
        dismissible={true}
        dismissLabel="Later"
      />
      <button
        class="downgrade"
        onClick={() => {
          alerts.plainVariant = 'warning'
        }}
      >
        Downgrade
      </button>
      <span class="status">{'installs=' + alerts.installs + ' dismissCalls=' + alerts.dismissCalls}</span>
    </div>
  )
}
