// zsys in a page (docs/zsys.md, "In a browser"): fetches a sim image and
// a card image, runs the machine (zsys.js, built from web/zsys_web.c),
// draws its screen on a canvas, and gives it the page's keyboard and
// mouse.
//
//   index.html                         zeitlos-sim.img.gz and zeitlos.img.gz, beside it
//   index.html?flash=F&card=C          others (card=none: no card)
//
// An image ending in .gz is unpacked in the browser
// (DecompressionStream): a 64MB card is mostly empty and goes over the
// network at a tenth of that. `zrelease web` makes the directory.
//
// The machine's changes -- the card's written blocks and the flash's
// key/value store -- are kept in this browser (IndexedDB) only if the
// visitor ticks the box, and forgotten with the button beside it.

"use strict";

// KeyboardEvent.code -> USB HID usage: the key's place, not its letter,
// as a USB keyboard reports it; Zeitlos applies its own layout
const HID = (() => {
	const m = {};
	for (let i = 0; i < 26; i++) m["Key" + String.fromCharCode(65 + i)] = 4 + i;
	for (let i = 1; i <= 9; i++) m["Digit" + i] = 29 + i;
	m.Digit0 = 39;
	Object.assign(m, {
		Enter: 40, Escape: 41, Backspace: 42, Tab: 43, Space: 44, Minus: 45, Equal: 46,
		BracketLeft: 47, BracketRight: 48, Backslash: 49, IntlHash: 50, Semicolon: 51, Quote: 52,
		Backquote: 53, Comma: 54, Period: 55, Slash: 56, CapsLock: 57,
		PrintScreen: 70, ScrollLock: 71, Pause: 72, Insert: 73, Home: 74, PageUp: 75, Delete: 76,
		End: 77, PageDown: 78, ArrowRight: 79, ArrowLeft: 80, ArrowDown: 81, ArrowUp: 82,
		NumLock: 83, NumpadDivide: 84, NumpadMultiply: 85, NumpadSubtract: 86, NumpadAdd: 87,
		NumpadEnter: 88, Numpad0: 98, NumpadDecimal: 99, IntlBackslash: 100, ContextMenu: 101,
		ControlLeft: 224, ShiftLeft: 225, AltLeft: 226, MetaLeft: 227,
		ControlRight: 228, ShiftRight: 229, AltRight: 230, MetaRight: 231,
	});
	for (let i = 1; i <= 12; i++) m["F" + i] = 57 + i;
	for (let i = 1; i <= 9; i++) m["Numpad" + i] = 88 + i;
	return m;
})();

const $ = (id) => document.getElementById(id);
const say = (t) => { $("status").textContent = t; };

// -- the console pane --
const lines = [];
function consoleLine(t) {
	lines.push(t);
	if (lines.length > 400) lines.splice(0, lines.length - 400);
	const el = $("console");
	el.textContent = lines.join("\n");
	el.scrollTop = el.scrollHeight;
	if (window.zsysSeen !== undefined) window.zsysSeen += t + "\n";	// for tests
}

// -- keeping changes: IndexedDB, one store of card blocks, one of the flash --
const DB = "zsys", KEEP = "zsys.keep";
function db() {
	return new Promise((ok, no) => {
		const r = indexedDB.open(DB, 1);
		r.onupgradeneeded = () => { r.result.createObjectStore("blocks"); r.result.createObjectStore("flash"); };
		r.onsuccess = () => ok(r.result);
		r.onerror = () => no(r.error);
	});
}
async function dbAll(store) {
	const d = await db();
	return new Promise((ok) => {
		const out = [], c = d.transaction(store).objectStore(store).openCursor();
		c.onsuccess = () => { const k = c.result; if (!k) return ok(out); out.push([k.key, k.value]); k.continue(); };
		c.onerror = () => ok(out);
	});
}
async function dbPut(store, pairs) {
	const d = await db();
	return new Promise((ok) => {
		const t = d.transaction(store, "readwrite");
		for (const [k, v] of pairs) t.objectStore(store).put(v, k);
		t.oncomplete = ok; t.onerror = ok;
	});
}
async function dbForget() {
	const d = await db();
	return new Promise((ok) => {
		const t = d.transaction(["blocks", "flash"], "readwrite");
		t.objectStore("blocks").clear(); t.objectStore("flash").clear();
		t.oncomplete = ok; t.onerror = ok;
	});
}

// Which images these are. What a visitor kept is laid over the images
// it was made on -- the card's blocks over that card's filesystem -- and
// over any other image it would be corruption, not a saved machine: so
// it is kept with the images' identity, and dropped when they change
// (a new release on the server, a card with more sets).
async function imageId(flash, card) {
	const parts = [];
	for (const img of [flash, card]) {
		if (!img) { parts.push("none"); continue; }
		if (crypto.subtle) {
			const d = new Uint8Array(await crypto.subtle.digest("SHA-256", img));
			parts.push(Array.from(d, (b) => b.toString(16).padStart(2, "0")).join(""));
		} else {				// no SubtleCrypto (plain http): the length and a sample
			let h = 0x811c9dc5;
			for (let i = 0; i < img.length; i += 509) h = Math.imul(h ^ img[i], 0x01000193) >>> 0;
			parts.push(img.length + "-" + h.toString(16));
		}
	}
	return parts.join("/");
}

async function fetchImage(url) {
	const r = await fetch(url);
	if (!r.ok) throw new Error(url + ": " + r.status);
	if (url.split("?")[0].endsWith(".gz") && r.body && typeof DecompressionStream !== "undefined")
		return new Uint8Array(await new Response(r.body.pipeThrough(new DecompressionStream("gzip"))).arrayBuffer());
	return new Uint8Array(await r.arrayBuffer());
}

async function start() {
	const q = new URLSearchParams(location.search);
	const flashUrl = q.get("flash") || "zeitlos-sim.img.gz", cardUrl = q.get("card") || "zeitlos.img.gz";
	const keep = (() => { try { return localStorage.getItem(KEEP) === "1"; } catch (e) { return false; } })();
	$("keep").checked = keep;

	say("loading the machine ...");
	// the build's stamp (zrelease web): on every URL the page fetches, so a
	// new deploy is never mixed with what the browser kept of the last
	const V = window.ZSYS_V || "";
	const stamped = (u) => V && !/^[a-z]+:/i.test(u) ? u + (u.includes("?") ? "&" : "?") + "v=" + V : u;
	if (V) consoleLine("zsys page: build " + V);
	const M = await ZsysModule({ onConsole: consoleLine, locateFile: (p) => stamped(p) });
	say("fetching " + flashUrl + " ...");
	const flash = await fetchImage(stamped(flashUrl));
	let card = null;
	if (cardUrl !== "none") {
		say("fetching " + cardUrl + " ...");
		try { card = await fetchImage(stamped(cardUrl)); } catch (e) { consoleLine("zsys page: no card (" + e.message + ")"); }
	}
	const id = await imageId(flash, card);
	if (keep) {				// what this browser kept from earlier visits -- of these images
		try {
			const kept = new Map(await dbAll("flash"));
			const blocks = card ? await dbAll("blocks") : [];
			// kept without an identity (an earlier page) is of unknown images: dropped too
			if ((kept.size || blocks.length) && kept.get("id") !== id) {
				await dbForget();
				consoleLine("zsys page: the images have changed since your last visit -- starting from the new ones");
			} else {
				if (kept.has(0) && kept.get(0).length === flash.length) flash.set(kept.get(0));
				for (const [n, v] of blocks) if ((n + 1) * 512 <= card.length) card.set(v, n * 512);
			}
		} catch (e) { consoleLine("zsys page: could not read what was kept: " + e.message); }
	}

	const fp = M._zw_alloc(flash.length);
	M.HEAPU8.set(flash, fp);
	let cp = 0;
	if (card) { cp = M._zw_alloc(card.length); M.HEAPU8.set(card, cp); }
	const r = M._zw_init(fp, flash.length, cp, card ? card.length : 0, 1);
	if (r) { say("could not start the machine (" + r + ")"); return; }
	say("running");

	// -- the screen --
	const cv = $("screen"), ctx = cv.getContext("2d");
	const img = ctx.createImageData(640, 480);
	function draw() {
		M._zw_render();
		const lo = M._zw_dirty_lo(), hi = M._zw_dirty_hi();
		if (hi < lo) return;
		const src = new Uint8ClampedArray(M.HEAPU8.buffer, M._zw_rgba(), 640 * 480 * 4);
		img.data.set(src.subarray(lo * 640 * 4, (hi + 1) * 640 * 4), lo * 640 * 4);
		ctx.putImageData(img, 0, 0, 0, lo, 640, hi - lo + 1);
	}

	// -- input: the canvas takes the keyboard while it has the focus --
	//
	// Queued, and handed to the machine with 8ms of its time between one
	// and the next: a browser can deliver a press and its release in
	// the same frame, and a USB device reports them a poll apart -- the
	// kernel must see the button down before it sees it up.
	const queue = [];
	const ev = (f) => queue.push(f);
	let buttons = 0;
	// the screen's pixels under the pointer -- in full screen the canvas
	// is letterboxed (object-fit: contain), so the picture is the largest
	// 4:3 box in the middle of it, not the whole element
	const pos = (e) => {
		const b = cv.getBoundingClientRect(), k = Math.min(b.width / 640, b.height / 480);
		const ox = b.left + (b.width - 640 * k) / 2, oy = b.top + (b.height - 480 * k) / 2;
		return [Math.floor((e.clientX - ox) / k), Math.floor((e.clientY - oy) / k)];
	};
	$("full").addEventListener("click", () => {
		const f = cv.requestFullscreen || cv.webkitRequestFullscreen;
		if (f) f.call(cv).then(() => cv.focus(), () => {}); else say("this browser has no full screen");
	});
	cv.addEventListener("mousemove", (e) => {
		let [x, y] = pos(e);
		const b = buttons;
		x = Math.max(0, Math.min(639, x)); y = Math.max(0, Math.min(479, y));
		// moves coalesce, as the hardware's cursor does: only the latest waits
		if (queue.length && queue[queue.length - 1].move) queue.pop();
		const f = () => M._zw_mouse(x, y, b);
		f.move = true;
		ev(f);
	});
	cv.addEventListener("mousedown", (e) => {
		cv.focus();
		buttons |= e.button === 0 ? 1 : e.button === 2 ? 2 : 4;
		let [x, y] = pos(e);
		const b = buttons;
		x = Math.max(0, Math.min(639, x)); y = Math.max(0, Math.min(479, y));
		ev(() => M._zw_mouse(x, y, b));
		e.preventDefault();
	});
	window.addEventListener("mouseup", (e) => {
		buttons &= ~(e.button === 0 ? 1 : e.button === 2 ? 2 : 4);
		const [x, y] = pos(e), b = buttons;
		ev(() => M._zw_mouse(Math.max(0, Math.min(639, x)), Math.max(0, Math.min(479, y)), b));
	});
	cv.addEventListener("contextmenu", (e) => e.preventDefault());
	// the wheel: whole notches, up (away) positive as HID has it; a
	// touchpad's small steps add up to one (100 pixels, or 3 lines, a notch)
	let wheelAcc = 0;
	cv.addEventListener("wheel", (e) => {
		e.preventDefault();
		wheelAcc += e.deltaMode === 1 ? e.deltaY * 100 / 3 : e.deltaMode === 2 ? e.deltaY * 400 : e.deltaY;
		const n = Math.trunc(wheelAcc / 100);
		if (!n) return;
		wheelAcc -= n * 100;
		ev(() => M._zw_wheel(-n));
	}, { passive: false });
	// -- a finger (docs/touch.md) --
	//
	// One finger is the machine's touch screen: the virtual mouse with
	// its TOUCH bit, which wm reads as tap, drag and hold. Two are this
	// page's: dragged together they turn the wheel (a notch every 30
	// pixels), and pinched they zoom the canvas, panned by their middle
	// -- everything is small on a phone, and this costs the machine
	// nothing. A device whose pointer is coarse has no keyboard until a
	// key is pressed: the machine sees port 0 empty and opens its own.
	let keyboard = !matchMedia("(pointer: coarse)").matches;
	if (!keyboard) ev(() => M._zw_keyboard(0));
	const fingers = new Map();
	let two = null, zoom = 1, panX = 0, panY = 0;
	const clamp = ([x, y]) => [Math.max(0, Math.min(639, x)), Math.max(0, Math.min(479, y))];
	const mid = () => { const p = [...fingers.values()]; return [(p[0][0] + p[1][0]) / 2, (p[0][1] + p[1][1]) / 2, Math.hypot(p[0][0] - p[1][0], p[0][1] - p[1][1])]; };
	const view = () => { cv.style.transform = zoom === 1 ? "" : "translate(" + panX + "px," + panY + "px) scale(" + zoom + ")"; };
	cv.addEventListener("touchstart", (e) => {
		e.preventDefault();
		cv.focus();
		for (const t of e.changedTouches) fingers.set(t.identifier, [t.clientX, t.clientY]);
		if (fingers.size === 1) {
			const [x, y] = clamp(pos(e.changedTouches[0]));
			ev(() => M._zw_touch(x, y, 1));
		} else if (fingers.size === 2 && !two) {
			// the first finger was not a touch after all: let wm forget
			// it rather than see it lift (which would be a tap)
			const [x, y] = clamp(pos(e.changedTouches[0]));
			ev(() => M._zw_touch_off());
			ev(() => M._zw_touch(x, y, 0));
			const [mx, my, d] = mid();
			two = { mx, my, d, zoom, panX, panY, acc: 0, pinch: false };
		}
	}, { passive: false });
	cv.addEventListener("touchmove", (e) => {
		e.preventDefault();
		for (const t of e.changedTouches) if (fingers.has(t.identifier)) fingers.set(t.identifier, [t.clientX, t.clientY]);
		if (two && fingers.size >= 2) {
			const [mx, my, d] = mid();
			if (!two.pinch && Math.abs(d - two.d) > 30) two.pinch = true;
			if (two.pinch) {
				// the point under the fingers' middle stays under it
				const fr = cv.parentNode.getBoundingClientRect();
				zoom = Math.max(1, Math.min(4, two.zoom * d / two.d));
				const k = zoom / two.zoom;
				panX = (mx - fr.left) - ((two.mx - fr.left) - two.panX) * k;
				panY = (my - fr.top) - ((two.my - fr.top) - two.panY) * k;
				if (zoom === 1) panX = panY = 0;
				view();
			} else {
				two.acc += my - two.my;
				two.my = my;
				const n = Math.trunc(two.acc / 30);
				if (n) { two.acc -= n * 30; ev(() => M._zw_touch_wheel(n)); }
			}
			return;
		}
		if (fingers.size === 1 && !two) {
			const [x, y] = clamp(pos(e.changedTouches[0]));
			if (queue.length && queue[queue.length - 1].move) queue.pop();
			const f = () => M._zw_touch(x, y, 1);
			f.move = true;
			ev(f);
		}
	}, { passive: false });
	const lift = (e) => {
		e.preventDefault();
		for (const t of e.changedTouches) {
			if (fingers.size === 1 && !two && fingers.has(t.identifier)) {
				const [x, y] = clamp(pos(t));
				ev(() => M._zw_touch(x, y, 0));
			}
			fingers.delete(t.identifier);
		}
		if (!fingers.size) two = null;
	};
	cv.addEventListener("touchend", lift, { passive: false });
	cv.addEventListener("touchcancel", lift, { passive: false });

	const key = (down) => (e) => {
		const u = HID[e.code];
		if (!u || e.repeat) { if (u) e.preventDefault(); return; }	// a USB keyboard does not repeat
		if (!keyboard) { keyboard = true; ev(() => M._zw_keyboard(1)); }	// there is one after all
		ev(() => M._zw_key(u, down ? 1 : 0));
		e.preventDefault();
	};
	cv.addEventListener("keydown", key(true));
	cv.addEventListener("keyup", key(false));
	cv.addEventListener("blur", () => { for (const u of Object.values(HID)) M._zw_key(u, 0); });
	$("type").addEventListener("keydown", (e) => {
		if (e.key !== "Enter") return;
		for (const c of e.target.value + "\r") M._zw_uart_in(c.charCodeAt(0));
		e.target.value = "";
	});

	// -- keeping changes --
	let flashWrites = M._zw_flash_writes();
	async function save() {
		if (!$("keep").checked) return;
		const map = M.HEAPU8.subarray(M._zw_card_written(), M._zw_card_written() + Math.ceil(M._zw_card_blocks() / 8));
		const pairs = [];
		if (M._zw_card_written()) for (let i = 0; i < map.length; i++) {
			if (!map[i]) continue;
			for (let b = 0; b < 8; b++) if (map[i] & (1 << b)) {
				const n = i * 8 + b, at = M._zw_card() + n * 512;
				pairs.push([n, M.HEAPU8.slice(at, at + 512)]);
			}
			map[i] = 0;
		}
		await dbPut("flash", [["id", id]]);
		if (pairs.length) await dbPut("blocks", pairs);
		const fw = M._zw_flash_writes();
		if (fw !== flashWrites) {
			flashWrites = fw;
			await dbPut("flash", [[0, M.HEAPU8.slice(M._zw_flash(), M._zw_flash() + M._zw_flash_size())]]);
		}
	}
	setInterval(save, 3000);
	$("keep").addEventListener("change", (e) => {
		try { localStorage.setItem(KEEP, e.target.checked ? "1" : "0"); } catch (err) { /* a private window */ }
		if (e.target.checked) save();
	});
	$("forget").addEventListener("click", async () => { await dbForget(); say("forgotten: the next visit starts from the images"); });

	// -- sound: an AudioWorklet playing what the machine played --
	//
	// Browsers start sound only after the visitor does something, so the
	// AudioContext is made at the first press on the page. The worklet
	// keeps at most a fifth of a second: a page that fell behind (a tab in
	// the background) skips ahead instead of lagging from then on.
	const WORKLET = `class Z extends AudioWorkletProcessor {
		constructor() { super(); this.q = []; this.n = 0; this.at = 0;
			this.port.onmessage = (e) => { this.q.push(e.data); this.n += e.data.length / 2;
				const cap = sampleRate / 5;
				while (this.n > cap && this.q.length > 1) { this.n -= (this.q[0].length / 2) - this.at; this.q.shift(); this.at = 0; } }; }
		process(_i, outputs) {
			const l = outputs[0][0], r = outputs[0][1] || outputs[0][0];
			for (let i = 0; i < l.length; i++) {
				if (!this.q.length) { l[i] = 0; r[i] = 0; continue; }
				const b = this.q[0]; l[i] = b[2 * this.at]; r[i] = b[2 * this.at + 1];
				if (++this.at * 2 >= b.length) { this.q.shift(); this.at = 0; } this.n--;
			}
			return true;
		} }
		registerProcessor("zsys", Z);`;
	let actx = null, anode = null, soundOn = true;
	const abuf = M._zw_alloc(4096 * 8);
	async function soundStart() {
		if (actx || !soundOn || !window.AudioContext) return;
		try {
			actx = new AudioContext();
			const url = URL.createObjectURL(new Blob([WORKLET], { type: "application/javascript" }));
			await actx.audioWorklet.addModule(url);
			anode = new AudioWorkletNode(actx, "zsys", { outputChannelCount: [2] });
			anode.connect(actx.destination);
		} catch (e) { consoleLine("zsys page: no sound (" + e.message + ")"); actx = null; }
	}
	function soundFeed() {
		if (!anode || actx.state !== "running") {
			while (M._zw_audio_take(abuf, 4096, 48000)) ;	// not heard: not kept either
			return;
		}
		let n;
		while ((n = M._zw_audio_take(abuf, 4096, actx.sampleRate)) > 0) {
			const f = new Float32Array(M.HEAPU8.buffer.slice(abuf, abuf + n * 8));
			if (window.zsysAudioPeak !== undefined)	// for tests: the loudest sample sent
				for (let i = 0; i < f.length; i++) window.zsysAudioPeak = Math.max(window.zsysAudioPeak, Math.abs(f[i]));
			anode.port.postMessage(f);
		}
	}
	for (const t of ["pointerdown", "keydown"]) window.addEventListener(t, () => {
		soundStart();
		if (actx && actx.state === "suspended") actx.resume();
	});
	$("sound").addEventListener("change", (e) => {
		soundOn = e.target.checked;
		if (actx) soundOn ? actx.resume() : actx.suspend();
		else soundStart();
	});

	// -- time: the machine's milliseconds follow the page's, at most 50 a frame --
	let last = performance.now(), instr = 0, meterAt = last;
	function frame(t) {
		let ms = Math.min(50, Math.max(1, Math.round(t - last)));
		last = t;
		// queued input, 8ms of the machine's time apart (at most 6 a frame)
		for (let n = 0; queue.length && n < 6; n++) {
			queue.shift()();
			instr += M._zw_run(8);
			ms = Math.max(0, ms - 8);
		}
		if (ms) instr += M._zw_run(ms);
		draw();
		soundFeed();
		if (t - meterAt > 1000) {
			$("speed").textContent = (instr / (t - meterAt) / 1000).toFixed(1) + " million instructions a second" +
				" (idle is waiting, not slow)";
			instr = 0; meterAt = t;
		}
		if (!M._zw_halted()) requestAnimationFrame(frame);
		else say("the machine halted (see the console)");
	}
	requestAnimationFrame(frame);
	cv.focus();
}

start().catch((e) => say("could not start: " + e.message));
