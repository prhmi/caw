// CAW hub demo - SuperCollider.
// SEND: post a line that starts with "CAWS <name> <value>" - CAW parses it
// into the shared hub. RECEIVE: the hub arrives as the dictionary ~cawr.
~cawhubroutine !? { |r| r.stop; };
~cawhubsynth !? { |s| s.release; };
~cawhubsynth = { |freq = 220| SinOsc.ar(freq.lag(0.1), 0, 0.2) }
    .play(~cawgroup, ~cawoutbus);
~cawhubroutine = fork {
    loop {
        var t = (120 + 200.rand).round(1);
        ("CAWS sc-tempo " ++ t.asString).postln;
        ~cawhubsynth !? { |s| s.set(\freq, ~cawr[\tempo] ?? { t }) };
        0.5.wait;
    }
};
"CAW hub SC clip: sends sc-tempo, plays a sine that follows the hub tempo".postln;
