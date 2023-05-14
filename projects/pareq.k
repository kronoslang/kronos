Import Lazy
Import Complex

Package PGE {
	Fsr 	 = #48000
	freqs = [#15 #20 #25 #31.50 #50 #63 #80 #100 #125 #160 #200 #250 
	         #315 #400 #500 #630 #800 #1000 #1250 #1600 #2000 #2500 #3150 
			 #4000 #5000 #6300 #8000 #10000 #12500 #16000 #20000 #24000]

	Biquad(sig b0 b1 a1 a2) {
		zero = sig - sig
		y1 = z-1(zero y0)
		y2 = z-1(zero y1)
		y0 = sig - a1 * y1 - a2 * y2
		Biquad = b0 * y0 + b1 * y1
	}

	Bandwidths(polefr) {
		Use Lazy
		Use Algorithm

		polefr-1 = 	   (#0 polefr)
		polefr+1 = Drop(#1 polefr) 
		
		Bandwidths = Force(
			Multi-Map((prev next) => (next - prev) / #2 [polefr-1 polefr+1]))	
	}

	Feedback-Coefs(samplerate freqs bws) {
		Use Lazy
		coefs 		   = (f bw) => (#-2 * Math:Exp(bw / #-2) * Math:Cos(f) Math:Exp(#-1 * bw))
		Feedback-Coefs = Force(Multi-Map(coefs [freqs bws]))
	}

}


Main() {
	Use PGE
	Use Lazy

	polefr   = Algorithm:Map(f => f * Math:Pi * #2 / Fsr PGE:freqs)
	bws 	 = Bandwidths(polefr)
	fb-coefs = Feedback-Coefs(#48000 polefr bws)

	(sig _)  = Audio:In()

	single 	= Biquad(sig External("b0" 0) External("b1" 0) -0.5 0.5)
	
	bank 	 = Algorithm:Reduce(Add External("direct" 0) * sig
				 Algorithm:Zip-With(
					(i fbc) => Biquad(sig 
									  External(("f" i "b0") 0) 
									  External(("f" i "b1") 0) 
									  fbc)
					Algorithm:Expand(Arity(fb-coefs) (+ #1) #1)
					fb-coefs))

	Main = bank
}

