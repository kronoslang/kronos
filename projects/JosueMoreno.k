Import Algorithm
Import Gen

map = Algorithm:Map

; record 5 seconds of ambience, prompt user
; ambient loop + live input => generator

; transient detector -> karplus strong

; resonator bandwidth = 1/40 of cutoff

; transient -> transposing echo with lookahead (500ms), unaligned read/write head

peak-day   =  900
peak-night = 1800

day   = [0 28 16]
night = [6 8  16]

base = 50
tuning = 440

midi-to-hz(note) {
	midi-to-hz = tuning * Math:Pow(2 (note - 69) / 12)
}

josue-freq(mn) {
	josue-freq = midi-to-hz(mn + base)
}

lerp(coef a b) {
	lerp = a + coef * (b - a)
}

josue-freqs(time) {
	josue-freqs = Algorithm:Zip-With(
				(d n) => josue-freq(lerp(time d n))
				day night)
}

resflt(x0 amp freq bw) {
	sample-rate = Reactive:Rate(x0)
	w = #2 * Math:Pi * freq / sample-rate 
	r = Math:Exp(Neg(Math:Pi * bw / sample-rate))
	norm = (#1 - r * r) * amp

	a1 = #-2 * r * Math:Cos(w)
	a2 = r * r

	zero = x0 - x0

	y1 = z-1(zero y0)
	y2 = z-1(zero y1)
	y0 = x0 - a1 * y1 - a2 * y2

	resflt = norm * (y0 - y2)
}

noise(seed) {
	state = z-1(seed Audio:Signal(lcg))
	lcg   = 1664525i * Coerce(Int32 state) + 1013904223i
	noise = lcg * Math:Pow(2 -32)
}	

dust(freq) {
	n1 = noise(100581i)
	n2 = noise(41180i)

	dust = n1 & (n2 > freq)
}

mod-delay(sig fb delay max-delay) {
	(buf out idx) = rcsbuf(sig - sig max-delay sig + fb(output))

	idl = idx - delay

	out-a = Select-Wrap(buf idl)
	out-b = Select-Wrap(buf idl + 1i)

	frac = idl - Floor(idl)

	output = lerp(frac out-a out-b)
	mod-delay = output
}

tone(x0 cutoff) {
	y1 = z-1(0 y0)
	y0 = lerp(cutoff y1 x0)
	tone = y0
}

exc = tone(dust(0.499) 0.3)

karplus = (e f damp gain) => mod-delay(e (s => tone(s damp) * gain) Reactive:Rate(e) / f #10000)

intervals = [4 1 1 8 0 10 5 8 1 1]
notes = Algorithm:Accumulate(Add intervals)

freq-from-loudness(loudness) {
	note = Select(notes loudness * 9)
	freq-from-loudness = 55 * Math:Pow(2 note / 13)
}

arpeggio-test = karplus(exc freq-from-loudness(0.5 + 0.5 * Wave:Saw(0.1)) 0.99 0.99)

rms(sig window) {
	zero = sig - sig
	in   = sig
	out  = rbuf(zero window sig)
	ig   = z-1(zero ig + in - out)
	rms  = Math:Sqrt(ig) / (Math:Sqrt(window) + 0.00001)
}

transient(sig) {
	transient = rms(sig #512) / rms(sig #4096)
}

slew-limiter(sig r+ r-) {
	zero  = sig - sig
	st    = z-1(zero st + delta)
	delta = Max(Min(sig - st r+) r-)
	slew-limiter = st
}

Main() {
	mic = First(Audio:Input(0 0))
	env = slew-limiter(mic 1 -1)
	Main = Wave:Sin(440) * env
}
