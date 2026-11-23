// UART console view: the serial port at the console baud rate, ESP-IDF log colours, reopened when
// the port drops (a USB-Serial-JTAG port can re-enumerate on a chip reset).

export const CONSOLE_BAUD = 115200; // CONFIG_ESP_CONSOLE_UART_BAUDRATE (no effect on USB-Serial-JTAG)
const MAX_LINES = 5000;
const RECOVERABLE = ["BufferOverrunError", "FramingError", "BreakError", "ParityError"];
const SGR_CLASS = { 31: "e", 33: "w", 32: "i", 36: "d", 35: "v" }; // ESP-IDF LOG_COLOR_*

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
const sameDevice = (firstPort, secondPort) => {
  const firstInfo = firstPort.getInfo(), secondInfo = secondPort.getInfo();
  return firstInfo.usbVendorId !== undefined && firstInfo.usbVendorId === secondInfo.usbVendorId && firstInfo.usbProductId === secondInfo.usbProductId;
};

export class Monitor {
  /**
   * @param element <pre> the log goes to
   * @param onState called with true / false when the monitor starts / stops
   * @param note    formats a status line (port lost, reconnected) for the log
   */
  constructor(element, { onState = () => {}, note = (text) => text } = {}) {
    this.element = element;
    this.onState = onState;
    this.note = note;
    this.port = null;
    this.reader = null;
    this.running = null; // promise of the read loop
    this.stopping = false;
    this.decoder = null;
    this.cls = ""; // current SGR colour class
    this.line = null; // <span> of the line being written
  }

  get active() {
    return this.running !== null;
  }

  /** Open @p port (closed) and stream it; @p reset restarts the chip into the firmware first. */
  async start(port, { reset = false } = {}) {
    await this.stop();
    this.port = port;
    this.stopping = false;
    await this.#open();
    if (reset) await this.reset();
    this.onState(true);
    this.running = this.#run().finally(() => {
      this.running = null;
      this.onState(false);
    });
  }

  async stop() {
    if (!this.running) return;
    this.stopping = true;
    await this.reader?.cancel().catch(() => {});
    await this.running;
  }

  /**
   * Hard reset into the firmware: EN low through RTS, IO0 high through DTR. Same lines on a
   * USB-UART bridge with the usual auto-reset transistors and on the USB-Serial-JTAG peripheral.
   */
  async reset() {
    if (!this.port?.writable) return;
    await this.port.setSignals({ dataTerminalReady: false, requestToSend: true });
    await sleep(200);
    await this.port.setSignals({ dataTerminalReady: false, requestToSend: false });
  }

  clear() {
    this.element.textContent = "";
    this.line = null;
  }

  text() {
    return this.element.textContent;
  }

  async #open() {
    await this.port.open({ baudRate: CONSOLE_BAUD });
    // released lines: the chip runs (some hosts assert DTR / RTS on open)
    await this.port.setSignals({ dataTerminalReady: false, requestToSend: false });
    this.decoder = new TextDecoder("utf-8", { fatal: false });
  }

  async #run() {
    try {
      while (!this.stopping) {
        if (!this.port.readable) {
          if (!(await this.#reconnect())) break;
          continue;
        }
        this.reader = this.port.readable.getReader();
        try {
          for (;;) {
            const { value, done } = await this.reader.read();
            if (done) break;
            if (value?.length) this.#feed(this.decoder.decode(value, { stream: true }));
          }
        } catch (error) {
          if (!RECOVERABLE.includes(error?.name) && !this.stopping) this.#status(this.note("lost"));
        } finally {
          this.reader.releaseLock();
          this.reader = null;
        }
      }
    } finally {
      await this.port.close().catch(() => {});
    }
  }

  /** Port gone: wait for the same USB device to come back and reopen it. */
  async #reconnect() {
    const old = this.port;
    await old.close().catch(() => {});
    while (!this.stopping) {
      for (const p of await navigator.serial.getPorts()) {
        if (p !== old && !sameDevice(p, old)) continue;
        try {
          this.port = p;
          await this.#open();
          this.#status(this.note("back"));
          return true;
        } catch { /* not back yet */ }
      }
      await sleep(500);
    }
    return false;
  }

  #status(text) {
    this.line = null;
    this.#append(`\n${text}\n`, "s");
    this.line = null;
  }

  /** Append decoded text: SGR colours to classes, other escapes and CR dropped. */
  #feed(text) {
    const parts = text.split(/\x1b\[([0-9;]*)m/);
    for (let i = 0; i < parts.length; i++) {
      if (i % 2) {
        const codes = parts[i].split(";").map(Number);
        const colorCode = codes.find((code) => SGR_CLASS[code]);
        this.cls = colorCode ? SGR_CLASS[colorCode] : "";
        this.line = null;
      } else if (parts[i]) {
        this.#append(parts[i].replace(/\r|\x1b\[[0-9;?]*[A-Za-z]/g, ""), this.cls);
      }
    }
  }

  #append(text, cls) {
    const element = this.element;
    const follow = element.scrollTop + element.clientHeight >= element.scrollHeight - 24;
    const chunks = text.split("\n");
    chunks.forEach((chunk, i) => {
      if (i > 0) {
        this.line = null;
        element.append("\n");
      }
      if (!chunk) return;
      if (!this.line || this.line.className !== cls) {
        this.line = document.createElement("span");
        this.line.className = cls;
        element.append(this.line);
      }
      this.line.textContent += chunk;
    });
    while (element.childNodes.length > MAX_LINES * 2) element.firstChild.remove();
    if (follow) element.scrollTop = element.scrollHeight;
  }
}
