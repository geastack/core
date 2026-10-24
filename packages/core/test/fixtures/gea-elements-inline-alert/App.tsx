import { Component } from '@geastack/core'
import { Alerts } from './Alerts'

export class App extends Component {
  template() {
    return (
      <div class="app">
        <Alerts />
      </div>
    )
  }
}
