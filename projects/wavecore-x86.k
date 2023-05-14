Import Gen
Import IO

Package Biquad {
	Direct-II(x0 b0 b1 b2 a1 a2) {
		zero = x0 - x0

		y1 = z-1(zero y0)
		y2 = z-1(zero y1)
		y0 = x0 - y1 * a1 - y2 * a2
		Direct-II = b0 * y0 + b1 * y1 + b2 * y2
	}
}

Package Design {
	Resonator(amp freq bw sample-rate) {
		w = #2 * Math:Pi * freq / sample-rate 
		r = Math:Exp(Neg(Math:Pi * bw / sample-rate))
		norm = (#1 - r * r) * amp

		b0 = norm
		b1 = #0
		b2 = Neg(norm)
		a1 = #-2 * r * Math:Cos(w)
		a2 = r * r

		Resonator = (b0 b1 b2 a1 a2)
	}
}

Gain() {
	gain = Gen:LFO(Waveform:Tri 1)
	Gain = Algorithm:Map((* gain) Audio:Input(0 0))
}

Main() {
	lfo = Gen:LFO(Waveform:Tri 0.5)
	lfo2 = Gen:LFO(Waveform:Tri 0.55)
	(l r) = Audio:Input(0 0)
	Main  = (l * lfo  r * lfo2)	
}