Countdown(count) {
    Use Actions
    Countdown = 
        If(count > 0 
                { Do(
                    PrintLn(count) 
                    Countdown(count - 1)) } 
                { PrintLn("Done") })
}

Import Gen

Fractal(f dur g) {
  Use Actions

  ; time offset to next cluster
  time-offset = Math:Sqrt(dur)

  ; its duration is the remaining time
  next-dur    = dur - time-offset

  Fractal = Do(
      ; start sinusoid at frequency 'f'
      id <- Start( { Wave:Sin(f) * g } )

      ; stop it after 'dur' seconds
      After( dur  Stop( id ) )

      ; spawn two more fractals at musical intervals
      ; of 2/3 and 6/5, after time offset
      If( next-dur > 0
        { After( time-offset 
          Fractal(f * 2 / 3  next-dur g / 2)
          Fractal(f * 6 / 5  next-dur g / 2) ) } )
    )
}

Greetings() {
    Use Actions
    Greetings = Do(
            Print("What is your name? ")
            name <- ReadLine()
            PrintLn("Greetings, " name "!") 
            )
}

Import Complex

Exponential-Wave(init coef) {
    state = z-1(init state * Audio:Signal(coef))
    Exponential-Wave = state
}

MyInstr(duration pitch loudness) {
    Use Actions 
    Use Math
    coef = Complex:Polar(
        Pi * 2 * 440 * Pow(2 (pitch - 69) / 12) / Audio:Rate()
        Pow(0.01  1 / (duration * Audio:Rate())))
    init = Complex:Cons(0 loudness)

    MyInstr = Do(
        noteId <- Start( { Complex:Real(Exponential-Wave(init coef)) } )
        After(duration * 2 Stop(noteId)) ) 
}

MyScore = 
   [(0 3 60 1)
    (1 2 64 1)
    (2 1 67 1)
    (3   0.1 72 0.5)
    (3.1 0.1 71 0.4)
    (3.2 0.1 70 0.3)
    (3.3 0.1 69 0.2)
    (3.4 0.1 68 0.1)]

MyPlayer(tempo-scale score instr) {
    Use Actions
    MyPlayer = For(score 
        (time note-params) => After(time * tempo-scale 
            instr(note-params)))
}

seq = [(0 "f" 440) (1 "f" 550) (2 "f" 660) (3 "f" 0)]
synth = Wave:Sin(Control:Param("f" 0))
