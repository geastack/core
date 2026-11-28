import type { PointerEvent } from '@geastack/core'
const formatterKey = 'prepareStackTrace'
if (Object.hasOwn(Error, formatterKey) || Error.prepareStackTrace !== undefined) {
  throw new Error('native Error installed an initial formatter')
}
Error.prepareStackTrace = 'formatter token'
const savedFormatter = Error.prepareStackTrace
if (savedFormatter !== 'formatter token' || !Object.hasOwn(Error, formatterKey) || !Object.keys(Error).includes(formatterKey)) {
  throw new Error('native Error formatter was not installed as an ordinary own property')
}
Error.prepareStackTrace = undefined
if (!Object.hasOwn(Error, formatterKey) || Error.prepareStackTrace !== undefined) {
  throw new Error('clearing the native Error formatter removed its property')
}
Error.prepareStackTrace = savedFormatter
if (Error.prepareStackTrace !== 'formatter token') throw new Error('native Error formatter was not restored')
delete Error.prepareStackTrace
if (Object.hasOwn(Error, formatterKey) || Object.keys(Error).includes(formatterKey)) {
  throw new Error('native Error formatter survived deletion')
}

const plainListener = { capture: false }
let rejectedListener = false
try {
  window.addEventListener('click', plainListener, plainListener)
} catch (error) {
  rejectedListener = error instanceof TypeError
}
if (!rejectedListener) throw new Error('native window accepted a non-function listener')
window.removeEventListener('click', plainListener, plainListener)
let rejectedEvent = false
try {
  window.addEventListener('unavailable-event' as 'click', () => {})
} catch (error) {
  rejectedEvent = error instanceof TypeError
}
if (!rejectedEvent) throw new Error('native window accepted an unavailable event type')

const view = document.defaultView
if (view.document.nodeType !== 9) throw new Error('native window did not return its document')
view.scrollTo(12, 18)
if (window.scrollX !== 0 || view.scrollY !== 0) throw new Error('a viewport without scrollable overflow changed its offsets')
const selection = window.getSelection()
const hasSelection = (value: typeof selection) => value.anchorNode !== null && value.focusNode !== null
if (hasSelection(selection)) throw new Error('native selection started with a range')
const selected = document.createElement('div')
selected.appendChild(document.createElement('span'))
document.body.appendChild(selected)
const liveSelection = view.getSelection()
liveSelection.selectAllChildren(selected)
const anchorOffset = selection.anchorOffset
const focusOffset = selection.focusOffset
if (!hasSelection(selection) || anchorOffset !== 0 || focusOffset !== 1) {
  throw new Error('native selection did not track the selected children')
}
liveSelection.removeAllRanges()
if (hasSelection(selection)) throw new Error('native selection retained its cleared range')
const inline = document.createElement('span')
const inlineStyle = getComputedStyle(inline)
const initialPosition = inlineStyle.position
if (inlineStyle.display !== 'inline' || initialPosition !== 'static') throw new Error('incorrect native defaults')
inline.style.setProperty('padding-top', '7px')
inline.style.setProperty('padding-bottom', '11px')
inline.style.setProperty('position', 'relative')
if (inlineStyle.paddingTop !== '7px' || inlineStyle.paddingBottom !== '11px' || inlineStyle.position !== 'relative') {
  throw new Error('computed style view did not follow native writes')
}
const pseudoStyle = getComputedStyle(inline, ':BEFORE')
const missingPadding = pseudoStyle.paddingTop
if (missingPadding !== '') throw new Error('an ungenerated pseudo-element returned declarations')
inline.className = 'native-style-probe'
if (inline.getAttribute('class') !== inline.className) throw new Error('className disagrees with the native class attribute')
const generatedPadding = pseudoStyle.paddingTop
if (generatedPadding !== '19px') throw new Error('generated pseudo-element style did not use its own cascade')
if (getComputedStyle(inline, '::view-transition-old(counter)').position !== '') throw new Error('unavailable pseudo-element returned a style')
if (getComputedStyle(inline, null).paddingTop !== '7px') throw new Error('null pseudo-element did not select the element')
inline.className = ''
if (pseudoStyle.paddingTop !== '') throw new Error('retired pseudo-element retained a style')
const measured = document.createElement('div')
document.body.appendChild(measured)
measured.style.setProperty('width', '120px')
measured.style.setProperty('height', '31px')
measured.style.setProperty('padding-left', '15px')
measured.style.setProperty('padding-right', '15px')
const dimensions = getComputedStyle(measured)
const initialWidth = dimensions.width
if (initialWidth !== '120px' || dimensions.height !== '31px') throw new Error('resolved size included content-box padding')
if (typeof measured.animate !== 'undefined' || typeof measured.getAnimations !== 'undefined') {
  throw new Error('native element reported an unavailable Web Animations method')
}
if ('animate' in measured || 'getAnimations' in measured) {
  throw new Error('native element claimed an unavailable Web Animations property')
}
const bounds = measured.getBoundingClientRect()
if (bounds.width !== 150 || bounds.height !== 31) throw new Error('bounding rectangle excluded padding')
measured.style.setProperty('transform', 'translateX(20px)')
const movedBounds = measured.getBoundingClientRect()
if (movedBounds.left !== bounds.left + 20) throw new Error('bounding rectangle omitted the transform')
measured.style.setProperty('width', '50%')
const percentWidth = dimensions.width
if (percentWidth !== '160px') throw new Error('computed width did not flush layout')
measured.style.setProperty('display', 'none')
if (dimensions.width !== '50%') throw new Error('hidden element did not retain its computed percentage')
measured.style.setProperty('overflow', 'visible hidden')
measured.style.setProperty('border-radius', '5px 12.5%')
if (dimensions.overflow !== 'auto hidden') throw new Error('computed overflow did not couple its axes')
if (dimensions.borderRadius !== '5px 12.5%') throw new Error('computed radius did not preserve native units')
measured.style.setProperty('filter', 'blur(4px)')
const blurred = dimensions.filter
if (blurred !== 'blur(4px)') throw new Error('computed filter did not read the renderer blur')
measured.style.removeProperty('filter')
if (dimensions.filter !== 'none') throw new Error('computed filter retained a removed blur')
let released = 0
const release = (event: PointerEvent) => {
  released++
  if (view.innerWidth === 320 && view.innerHeight === 240 && event.clientX === 41) event.preventDefault()
  console.log(event.clientX >= 0 ? 'native release' : 'native release outside')
}
window.addEventListener('pointerup', release, true)
window.removeEventListener('pointerup', release, true)
window.addEventListener('pointerup', release, { capture: true, once: true })
window.addEventListener('pointermove', release)
window.removeEventListener('pointermove', release)
const blur = () => { console.log('native window blur') }
window.addEventListener('blur', blur, { once: true })
window.removeEventListener('blur', blur)
window.addEventListener('resize', () => {
  window.addEventListener('pointerdown', (event: PointerEvent) => {
    if (view.innerWidth === 640 && view.innerHeight === 480 && event.clientX === 41) event.preventDefault()
  }, { once: true })
})
console.log(released === 0 ? 'native listeners registered' : 'unexpected native event')
