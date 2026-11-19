CAW caws/cawr hub example
=========================

One shared message bus ("hub") with named numbers. Every clip of every
language can write a name (caws) and read it (cawr):

  Csound clip:      caws <expr>, "name"      kVar cawr "name"
  Pure Data patch:  [s $0-caws-name]         [r $0-cawr-name]
  SuperCollider:    ("CAWS name value").postln     ~cawr[\name]

What this project shows
-----------------------
Track "Csound Hub"     - Sender:  caws kSpeed, "tempo"   (the value also
                                   walks the pitch of its own tone)
                       - Receiver: kTempo cawr "tempo"   -> a tone at
                         kTempo*4 Hz follows whatever "tempo" is now.
Track "Pure Data Out"  - clip pd-hub-out.pd: [s $0-caws-tempo] sends a
                         random tempo every 0.5 s into the hub.
Track "Pure Data In"   - clip pd-hub-in.pd: [r $0-cawr-tempo] receives it
                         and plays a 0.2-amp tone that follows it.
Track "SuperCollider"  - clip sc-hub.scd: posts CAWS sc-tempo lines into
                         the hub and plays a sine that follows ~cawr[\tempo].

How to run
----------
1. Keep this .caw file, pd-hub-out.pd, pd-hub-in.pd and sc-hub.scd in the
   SAME folder (the PD/SC clips reference the files by name).
2. Open the .caw in CAW. Enable Pure Data (and SuperCollider if you want
   the SC track) in Settings > Languages, pick libpd in Settings > Programs.
3. Press "Run Csound + PD" (or the combined Run button). The console shows
   printk2 / print lines from all languages sharing one "tempo" value.
4. Mute one sender: the others keep feeding the same hub name.

Notes
-----
- Names are letters, digits, "_" and "-" only.
- caws/cawr are plain shared numbers: the last writer wins, values change
  at control rate (once per engine block). For automation of one clip use
  cawget/cawset instead (per clip, with lanes); for recording use the rec
  arm.
- Pure Data messages need the engines running; SC needs the sclang process
  (Run SuperCollider), because the values arrive as OSC to sclang.
