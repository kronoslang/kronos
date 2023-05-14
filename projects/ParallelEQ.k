Import Complex
Import Algorithm
Import Interpolation
Import Cepstrum
Import Linear-Algebra

Package Filter {
	Biquad(sig b0 b1 a1 a2) {
		zero = sig - sig
		y1 = z-1(zero y0)
		y2 = z-1(zero y1)
		y0 = sig - a1 * y1 - a2 * y2
		Biquad = b0 * y0 + b1 * y1
	}

	Biquad-TFII(xn b0 b1 a1 a2) {
		zero = xn - xn

		s2 = z-1(zero a2 * yn)
		s1 = z-1(zero b1 * xn - a1 * yn - s2)
		yn = s1 + b0 * xn

		Biquad-TFII = yn
	}

	Bandwidths(polefr) {
		Use Algorithm

		polefr-1 = (#0 polefr)
		polefr+1 = Rest(polefr)

		Bandwidths = Zip-With(
			(prev next) => (next - prev) / #2 
			polefr-1 polefr+1)	
	}

	Feedback-Coefs(sr f bw) {
		Use Math
		frad  =  f * Math:Pi * #2 / sr
		bwrad = bw * Math:Pi * #2 / sr 
		r     = Exp(bwrad * #-0.5)
		Feedback-Coefs = 
		  ( Coerce( Float #-2 * r * Cos(frad) )
			Coerce( Float r * r ) )
	}
}

Logarithmic-Frequencies(N f0 fn) {
	Use Algorithm
	ts = Expand(N (+ (#1 / (N - #1))) #0)
	Logarithmic-Frequencies = Map(t => f0 * Math:Pow(fn / f0 t) ts)
}

Hilbert-Transform(data) {
	Use Algorithm
	Fs = Cepstrum:FFT(data)
	N = Arity(Fs)
	j = Complex:Cons(0 1)
	H = Concat(
		(Complex:Cons(0 0) Expand((N / #2) - #1  '_  -1 * j))
		 Expand(N / #2  '_  j))
	Hilbert-Transform = Cepstrum:IFFT(Zip-With(Mul H Fs))
}

Target-Magnitude(sr freqs command-gains) {
	Use Algorithm
	splines    = Interpolation:Splines(Zip(freqs command-gains))

	dB2coef    = db => Math:Pow(#10 db / #20)
	Target-Magnitude = f => dB2coef(Interpolation:BPF(f splines))
}

Minimum-Phase-Response(magnitude) {
	Use Algorithm
	magn-mirror = Concat(magnitude Reverse(magnitude))
	Minimum-Phase-Response = 
		Zip-With(Complex:Polar
			Map(x => Neg(Complex:Real(x))
				Hilbert-Transform(Map(Math:Log magn-mirror)))
			magnitude)
}

Eigen-Least-Squares(A B) {
	Use Matrix
	out = Algorithm:Expand(Rows(A) '_ 0)
	(_ Eigen-Least-Squares _) = Foreign-Function["int32" "eigen_ls_solve" 
		"float*" out 
		"const float*" Break(Matrix A)
		"const float*" B		
		"int32" Coerce(Int32 Rows(A))
		"int32" Coerce(Int32 Cols(A)) ]	
}

Modeling-Matrix(sr control-points flt-denoms) {
	; Construct a modeling matrix containing the responses
	; of a direct path, plus the poles described by 'flt-denoms'
	; as they are and delayed by one sample

	Use Algorithm

	; unit delay on the Z plane for frequency 'f'
	ud 	  = f => Complex:Unitary(f * -2 * Math:Pi / sr)

	; compute pole response at 'frad' in radians with feedback coefficients a1 and a2 and unitary numerator
	pole-resp = (f a1 a2) => 1 / Fold((c resp) => c + resp * ud(f) 1 a1 a2)

	; the modeling matrix contains the pole responses, delayed pole responses and the direct path response
	Modeling-Matrix = Fold[ Concat
				; direct path
				[ Map('Complex:Cons(1 0) control-points) ]
				; pole responses
				Map((a1 a2) => 
					Map(f => pole-resp(f a1 a2) control-points) flt-denoms ) 
				; delayed pole responses
				Map((a1 a2) => 
					Map(f => pole-resp(f a1 a2) * ud(f)  control-points) flt-denoms )
			]
}

Flatten-Complex(cs) {
	; Turn a complex vector 'cs' into a real vector by concatenating the real
	; and imaginary parts.
	Use Algorithm
	Flatten-Complex = Concat(Map(Complex:Real cs) Map(Complex:Img cs))
}

Unbundle-Coefficients(solution) {
	Use Algorithm

	; structure the least squares solution as a set of filter coefficients
	N = (Arity(solution) - #1) / #2
	d0 = First(solution)
	b0s = Take(N Rest(solution))
	b1s = Take(N Skip(N Rest(solution)))
	Unbundle-Coefficients = ( d0 Zip( b0s b1s ))
}

Design-Numerators(sr target-response control-points flt-denoms) {
	; perform least squares optimization to match 'target-response'
	; with the set of filter poles in 'flt-denoms' for two feedforward
	; coefficients
	Use Algorithm

	; compute modeling matrix
	M = Modeling-Matrix( sr control-points flt-denoms )

	; generate real matrix Mr
	Mr = Matrix:Cons( Map(Flatten-Complex M) )

	; produce target response vector
	hr = Flatten-Complex( Map(target-response control-points) )
	
	; obtain a least squares solution to: Mr ls = hr
	ls = Eigen-Least-Squares(Mr hr)

	; unbundle direct path and filter coefficients b0 and b1
	Design-Numerators = Unbundle-Coefficients(ls)
}

Design-Numerators-Weighted(sr target-response control-points flt-denoms) {
	; perform least squares optimization to match 'target-response'
	; with the set of filter poles in 'flt-denoms' for two feedforward
	; coefficients
	Use Algorithm

	; produce target response vector
	h = Map(target-response control-points)

	; normalize by target magnitude
	norm = Map(c => #1 / Abs(c) h)

	; compute modeling matrix
	M = Modeling-Matrix( sr control-points flt-denoms )

	; generate real matrix Mr and normalize by magnitude
	Mr = Matrix:Cons( Map(row => Flatten-Complex(Zip-With(Mul row norm)) M ) )

	; generate real target hr and normalize by magnitude
	hr = Flatten-Complex(Zip-With(Mul h norm))
	
	; obtain a least squares solution to: Mr ls = hr
	ls = Eigen-Least-Squares(Mr hr)

	; unbundle direct path and filter coefficients b0 and b1
	Design-Numerators-Weighted = Unbundle-Coefficients(ls)
}

Filter-Response(sr f b0 b1 a1 a2) {
	Z-1 = Complex:Unitary(f * Math:Pi * -2 / sr)
	Zpoly = cs => Algorithm:Fold((c resp) => c + Z-1 * resp cs)
	Filter-Response = 
		Zpoly(b0 b1) / 
		Zpoly(1 a1 a2)
}

Bank-Response(sr f flts) {
	Bank-Response = Algorithm:Reduce(
		Add
		First(flts)
		Algorithm:Map(
			cs => Filter-Response(sr f cs)
			Rest(flts)))
}

N = #30

zigzag = Algorithm:Expand(N Neg 12)
unity  = Algorithm:Expand(N '_  0)
one-up = [-12 -12 -12 -12  -12 -12 -12 -12  -12 -12 -12 -12  -12 -12 -12 -12  12 -12 -12 -12  -12 -12 -12 -12  -12 -12 -12 -12  -12 -12 ]
one-dn = [12 12 12 12  12 12 12 12  -12 12 12 12  12 12 12 12  12 12 12 12  12 12 12 12  12 12 12 12  12 12 ]

Process(sr sig gains) {
	Use Algorithm

	N = Arity(gains)


	f0 = #31
	fn = #20000

	; Place 'N' frequency points logarithmically between 20 and 20000
	; including extra points at 0 and Nyquist
	fs = Concat( (#0 Logarithmic-Frequencies(N f0 fn))
				 [sr / #2] )

	; insert edge points to command gains
	gs = Concat( Reactive:Couple( (0 gains) [0] ) )

	; compute target magnitude for specified command gains
	fsf = Map( 'Coerce(Float _) fs )
	tgt = Target-Magnitude( Reactive:Couple( Coerce(Float sr) fsf gs ) )

	; number of bins: 2^15
	nb   = Math:Pow(#2 #15)

	; (positive) bin indices
	binf = sr / (nb + nb)
	bins = Expand(nb (+ binf) 0)

	; obtain an interpolating minimum phase target response 
	mpsv = Minimum-Phase-Response(Map(tgt bins))
	mps  = freq => Eval(
		Interpolation:In-Vector(#3 mpsv)
		freq / binf)

	pf-fn   = Interpolation:In-Vector(#1 fs)
	; select 2N pole frequencies 
	pole-fs = Map(pf-fn Expand(N * #2 (+ #0.5) #1)) 
	; select 4N control point frequencies 
	cp-fs   = Map(cp => Coerce(Float pf-fn(cp)) Expand(N * #4 (+ #0.25) #0.5))

	; obtain filter denominators
	flt-denoms = Map('Filter:Feedback-Coefs(sr _ _ / 5) pole-fs)

	; obtain filter numerators
	(d0 flt-nums) = Design-Numerators-Weighted(sr mps cp-fs flt-denoms)

	; process parallel filters
	parflts    = Zip-With( ((b0 b1) (a1 a2)) => Filter:Biquad(sig b0 b1 a1 a2)
						   Audio:Signal( flt-nums ) flt-denoms )

	Process = Reduce(Add d0 * sig parflts)	
}

Main() {
	sr = #48000
	gains = Control:Param("command-gains" zigzag) ;one-up
	(l r) = Audio:Input(0 0)
	Main = Process(sr l + r gains)
}

Import Gen

Test() {
	gains = Reactive:Downsample(Audio:Signal(one-up) #4410000)
	sig   = Noise:Pseudo-White(0.499d)
	Test  = ( sig Process(#44100 sig gains) )
}