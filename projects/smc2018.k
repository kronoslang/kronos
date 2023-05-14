; setup
Import Gen

Actions:PrLn("hello from Linux!")

; simple synth

Wave:Sin(Control:Param("freq" 440)) * Control:Param("amp" 0.5)

; parameter abstraction

Pitch(name) {
	subp = (label default) => Control:Param(String:Append(name label) default)
	nn = subp("/note" 60)
	f0 = Control:Param("a4" 440) * Math:Pow(2 (nn - 69) / 12)
	vf = subp("/vfreq" 4.5)
	vd = subp("/vdepth" 0.03)

	f0 * (1 + Wave:Sin(vf) * vd)
}

Wave:Sin(Pitch("my-osc"))

; FM

; cross modulation

(af bf md) = Algorithm:Map(name => Control:Param(name 0) "af" "bf" "md")

amod = z-1(0 bout) * md
bmod = z-1(0 aout) * md

aout = Wave:Tri(af * (1 + amod))
bout = Wave:Tri(bf * (1 + bmod))

amod + bmod

; additive
