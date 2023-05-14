Import Algorithm
Import Delay
Import Interpolation
Import Gen
Import Cycle
Import Sound-File

(voc1 voc2 voc3 lattia) = Audio:Stereo-In()

Mix(sigs) {
	Algorithm:Reduce(Add sigs)
}

Lag(sig rate) {
	z = sig - sig
	st = z-1(z st + rate * (sig - st))
	st	
}

Package Rev {
	Use Algorithm[Map Reduce Concat Zip-With Take Skip]
	Split(sigs) {
		N = Arity(sigs)
		(
			Take(N / #2 sigs)
			Skip(N / #2 sigs)
		)
	}

	Hadamaard(sigs) {
		(odd even) = Split(sigs)
		ho = Hadamaard(odd)
		he = Hadamaard(even)
		
		Arity(sigs) == #1 : sigs
			Concat(
				Zip-With(Add ho he)
				Zip-With(Sub ho he) )

	}

	Max = #1000000
	Mod-Dly(sig time) {
		(buf out idx) = rcsbuf(sig - sig Max sig)
		(x-1 x0 x1 x2) = Map('Select-Wrap(buf idx - time + _) 2 1 0 -1)
		Interpolation:Hermite(Fraction(time) x-1 x0 x1 x2)
	}
}

FM(sig modulator depth) {
	mod = (1 + modulator) * depth / #2
	FM = Rev:Mod-Dly(sig Max(1 Min(depth mod)))
}

Pitch-Rate = 6

TEcho(sig dly transp) {
	Rev:Mod-Dly(Pitch(sig transp) dly)
}

Pitch(sig transp) {
	ratio = Math:Pow(2 transp / 12)
	slope1 = Gen:Osc('_ Pitch-Rate )
	slope2 = Fraction( slope1 + 0.5 )
	period = Rate-of(sig) / Pitch-Rate
	mamt   = (1 - ratio) * period
	mid    = Abs(mamt / 2)
	mod1   = Rev:Mod-Dly( sig 2 + mid + (slope1 - 0.5) * mamt )
	mod2   = Rev:Mod-Dly( sig 2 + mid + (slope2 - 0.5) * mamt )
	Pitch  = mod1 * Math:Sqrt(2 * Abs(slope2 - 0.5)) +
			 mod2 * Math:Sqrt(2 * Abs(slope1 - 0.5))
}

; mono = Audio-Cycle( fb => TEcho(in1 + fb * 0.4 20000 2.5) + TEcho(in1 + fb * 0.3 13830 -1.7) 0 )


Loop-Layers(ns path files) {
	Use Algorithm[Map Reduce Zip]

	(fsr _) = Sound-File:Data(First(files))
	asr = Audio:Rate()

	pos = Audio-Cycle((+ asr / fsr) 0)

	channels = Reduce({
		((l r) file) = arg
		url  = String:Append(String:Append(path file) ".wav")
		key  = String:append(ns file)
		(sr buffer) = Sound-File:Data(url)
		lev = Control:Param(key 0)
		sig = Sound-File:Hermite-Select(pos buffer) * lev
		pan = Control:Param(String:Append(key "-pan") 0.5)
		(
			l + sig * (1 - pan)
			r + sig * pan
		)
	} Zip(files pans))
}

ketju-files = [
	"bd"
	"mmb"
	"kilk"
	"mbr"
	"mmb5"
	"prnt"
]

ketju-path = "c:/Users/vnorilo/perinto"
