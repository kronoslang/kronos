
Import Math
Import Algorithm
Import Gen

Use Algorithm

Cycle(func init) {
	;; Route the output of 'func' back to its argument through a unit delay. Initialize the delay to 'init'.
	st = z-1(init func(st))
	Cycle = st
}

Noise(initial) {
	scrambler = x => Audio:Signal(x * 3.9999d * (1d - x))
	rng = Cycle(scrambler initial)
	Noise = 2 * Coerce(Float rng - 0.5d)
}

noise = Noise(0.499d)

Conv(sig coefs) {
    Conv = Nil?(coefs) : #0 sig * coefs

    z = sig - sig    
    (c cs) = coefs
    Conv = sig * c + Conv(z-1(z sig) cs)
}

Remove-Bass(sig) {
	Remove-Bass = Conv(sig 1 -1)
}

Biquad(sig b1 b2 a0 a1 a2) {
	fir    = 			 Conv( sig a0 a1 a2 )
	Biquad = Cycle( y => Conv( fir + y b1 b2 ) 0 )	
}

Reson(sig freq bw) {
    w = #2 * Math:Pi * freq / Audio:Rate() 
    r = Math:Exp(Neg(Math:Pi * bw / Audio:Rate()))
    norm = (#1 - r * r)
    Reson = Biquad(sig 
    	2 * r * Math:Cos(w)
    	Neg(r * r)
        norm #0 Neg(norm))
}

Lag(sig slow) {
	Lag = Cycle(old => old + slow * (sig - old) 0) 
}

slow-noise = 200 * Lag(noise 0.00003)
Reson(noise 330 + slow-noise * 80 3) * Lag(slow-noise 0.00005)

Algorithm:Map('Reson(noise _ 5) 440 550 660)

Note-to-Freq(note) {
	Note-to-Freq = 440 * Math:Pow(2 (note - 69) / 12)
}

Analyzer(sig) {
	Use Algorithm
	pitches = Expand(#32 (+ 4) 2)
	freqs   = Map(Note-to-Freq pitches)
	Analyzer = Map(f => Reson(sig f f / 20) freqs) 
}

Countdown() {
	url = "/users/vnorilo/countdown.wav"
	(sr sample) = External-Data("PAFGetDataSource" "audio-file" url)
	i = Cycle(i => Audio:Signal(i + 1i) 0i)
	Countdown = Select(sample i) * 0.3
}


Chord-Notes = [165 220 247.5 185.625]
left = Reduce(Add Map(Wave:Saw Chord-Notes)) / 16
right = Reduce(Add Map('Wave:Saw(_ + 2) Chord-Notes)) / 16

Env(sig) {
	Env = Lag(Abs(sig) 0.003)
}

envs = Map(Env Analyzer(Countdown()))
lbands = Analyzer(left)
rbands = Analyzer(right)

sibilance = Remove-Bass(Remove-Bass(Countdown())) * 0.2

Vocoder(carrier) {
	bands = Analyzer(carrier)
	Vocoder = Reduce(Add 
		Zip-With(Mul envs bands)) * 32 
		+ sibilance
}

(Vocoder(left) Vocoder(right))

