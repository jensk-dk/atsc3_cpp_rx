<script setup lang="ts">
// Virtual remote control for the hosted Broadcaster Application (spec 5.18.4). Every key goes through the KeyRouter:
// granted keys (highlighted) are delivered into the application, the others act on the receiver. The physical keyboard
// maps the same keys (arrows, Enter, Escape/Backspace, digits, PageUp/PageDown for channels, F2 for the app key).
import type { KeyRouter } from '../a344/keys'

const props = defineProps<{ router: KeyRouter; granted: string[] }>()

function has(name: string): boolean {
  return props.granted.includes(name) || (/^[0-9]$/.test(name) && props.granted.includes('Numeric'))
}
function press(name: string) {
  props.router.press(name)
}
const digits = ['1', '2', '3', '4', '5', '6', '7', '8', '9']
</script>

<template>
  <div class="remote" aria-label="virtual remote control">
    <div class="rrow">
      <button class="key wide" :class="{ app: has('BAAppear') }" title="BAAppear: the broadcaster application key (keyboard: F2 or Insert)" @click="press('BAAppear')">App</button>
      <button class="key" :class="{ app: has('ChannelUp') }" title="Channel up (keyboard: PageUp)" @click="press('ChannelUp')">CH+</button>
      <button class="key" :class="{ app: has('ChannelDown') }" title="Channel down (keyboard: PageDown)" @click="press('ChannelDown')">CH−</button>
    </div>
    <div class="dpad">
      <button class="key up" :class="{ app: has('ArrowUp') }" title="ArrowUp" @click="press('ArrowUp')">▲</button>
      <button class="key left" :class="{ app: has('ArrowLeft') }" title="ArrowLeft" @click="press('ArrowLeft')">◀</button>
      <button class="key ok" :class="{ app: has('Select') }" title="Select (keyboard: Enter)" @click="press('Select')">OK</button>
      <button class="key right" :class="{ app: has('ArrowRight') }" title="ArrowRight" @click="press('ArrowRight')">▶</button>
      <button class="key down" :class="{ app: has('ArrowDown') }" title="ArrowDown" @click="press('ArrowDown')">▼</button>
    </div>
    <div class="rrow">
      <button class="key wide" :class="{ app: has('Back') }" title="Back (keyboard: Escape or Backspace)" @click="press('Back')">Back</button>
    </div>
    <div class="rrow colors">
      <button class="key red" :class="{ app: has('ColorF0Red') }" title="ColorF0Red" @click="press('ColorF0Red')" />
      <button class="key green" :class="{ app: has('ColorF1Green') }" title="ColorF1Green" @click="press('ColorF1Green')" />
      <button class="key yellow" :class="{ app: has('ColorF2Yellow') }" title="ColorF2Yellow" @click="press('ColorF2Yellow')" />
      <button class="key blue" :class="{ app: has('ColorF3Blue') }" title="ColorF3Blue" @click="press('ColorF3Blue')" />
    </div>
    <div class="numpad">
      <button v-for="d in digits" :key="d" class="key" :class="{ app: has(d) }" :title="`Numeric ${d}`" @click="press(d)">{{ d }}</button>
      <span />
      <button class="key" :class="{ app: has('0') }" title="Numeric 0" @click="press('0')">0</button>
      <span />
    </div>
    <div class="muted hint">highlighted keys go to the application</div>
  </div>
</template>

<style scoped>
.remote { display: inline-flex; flex-direction: column; gap: 8px; padding: 10px; border: 1px solid var(--border); border-radius: 12px; background: var(--bg); width: 190px; }
.rrow { display: flex; gap: 6px; justify-content: center; }
.key { min-width: 44px; height: 36px; padding: 0 8px; font-size: 13px; border-radius: 8px; }
.key.wide { min-width: 70px; }
.key.app { border-color: var(--ok); color: var(--ok); box-shadow: inset 0 0 0 1px var(--ok); }
.dpad { display: grid; grid-template-columns: repeat(3, 50px); grid-template-rows: repeat(3, 40px); gap: 4px; justify-content: center; }
.dpad .up { grid-column: 2; grid-row: 1; }
.dpad .left { grid-column: 1; grid-row: 2; }
.dpad .ok { grid-column: 2; grid-row: 2; font-weight: 700; }
.dpad .right { grid-column: 3; grid-row: 2; }
.dpad .down { grid-column: 2; grid-row: 3; }
.numpad { display: grid; grid-template-columns: repeat(3, 50px); gap: 4px; justify-content: center; }
.colors .key { min-width: 36px; height: 14px; padding: 0; border-radius: 7px; }
.colors .red { background: #c0392b; } .colors .green { background: #27ae60; } .colors .yellow { background: #f1c40f; } .colors .blue { background: #2980b9; }
.hint { font-size: 11px; text-align: center; }
</style>
