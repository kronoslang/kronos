Import Control

Package MIDI {
	Brief(documentation) {
		;; Interfacing with an external MIDI device.
		nil
	}

	Raw-Input() {
		Raw-Input = IO:Source("/midi" 0i #0)
	}

	Type Event

	Input() {
		;; Receive external MIDI events and output them as `MIDI:Event`.
		 Make(Event Raw-Input())
	}

	Status/Channel/A/B(event) {
		;; Extract information from a MIDI:Event
		raw = Break(Event event)
		Status = BitShiftRight((raw & 0xf00000) 16i)
		Channel = BitShiftRight((raw & 0x0f0000) 16i)
		A = BitShiftRight((raw & 0x00ff00) 8i)
		B = (raw & 0x0000ff)
	}

	Unpack(event) {
		;; Unpack MIDI event.
		;; OUTPUTS: (status a b)
		(Status(event) | Channel(event) A(event) B(event))
	}

	Pack(status a b) {
		;; Compose MIDI event from `status` byte and two parameter bytes `a` and `b`.
		Make(Event BitShiftLeft(Coerce(0i status) & 0xff 16i) |
				   BitShiftLeft(Coerce(0i b)      & 0x7f 8i)  |
				   (Coerce(0i a) & 0x7f))
	}

	Note->Freq(note-num A4) {
		;; Convert MIDI note number to a frequency, relative to the frequency of 'A4', note 69.
		A4 * Math:Pow(2 (note-num - 69) / 12)
	}

	MIDI->Freq(midi-event A4) {

		;; Convert MIDI event stream to the frequency of the most recent note-on event
		;; when the frequency of note 69 is given by `A4`.

		s = Status(midi-event)
		a = A(midi-event)
		b = B(midi-event)

		Control:Sample-and-Hold(
			Note->Freq(a A4) 
			(b > 0i & s == 0x90))
	}

	MIDI->Gate(midi-event) {

		;; Output 1 while there is one or more keys pressed in the `midi-event` stream,
		;; by comparing count of note-ons and note-offs. Outputs 0 when no notes are 
		;; detected.

		s = Status(midi-event)
		b = B(midi-event)

		note-on = s == 0x90 & b > 0i
		note-off = s == 0x80 | (s == 0x90 & b == 0i)

		count = z-1(count) + (note-on & 1i) - (note-off & 1i)

		(count > 0i) & 1
	}
}
