"""DUT buttons are external events; OFF never drives a pin low."""
class Dut:
    def __init__(self, device, error):
        self.device, self.error = device, error
        self.on = set()
        self.buttons = set()
        self.manual = set()
        self.candidate = set()
        self.since = [0] * 20
        self.last_poll = device.now_ms
        self.intervention = False

    def button(self, name, pressed):
        i = self.index(name)
        (self.buttons.add if pressed else self.buttons.discard)(i)

    def index(self, name):
        names = [f'C{i}' for i in range(1, 21)]
        try: return names.index(name.upper())
        except ValueError: raise self.error('RANGE') from None

    def set(self, args):
        mode = args[-1].upper()
        if mode not in ('ON', 'OFF'): raise self.error('RANGE')
        indices = [self.index(a) for a in args[:-1]]
        if len(set(indices)) != len(indices): raise self.error('RANGE')
        for i in indices:
            if (i in self.on) == (mode == 'ON'): continue
            (self.on.add if mode == 'ON' else self.on.discard)(i)
            if i in self.manual: self.device.log_event(f'DUT C{i+1} MANUAL -> ON')
            self.manual.discard(i)
            self.candidate.discard(i)
            self.since[i] = self.device.now_ms
        return 'OK'

    def off(self):
        return self.set([f'C{i+1}' for i in range(20)] + ['OFF'])

    def poll(self):
        now = self.device.now_ms
        if now - self.last_poll < 10: return
        self.last_poll = now
        for i in range(20):
            if i in self.on: continue
            high = i in self.buttons
            if high != (i in self.candidate):
                (self.candidate.add if high else self.candidate.discard)(i)
                self.since[i] = now
            if high != (i in self.manual) and now - self.since[i] >= 30:
                (self.manual.add if high else self.manual.discard)(i)
                self.intervention = True
                self.device.generation += 1
                self.device.log_event(f"DUT C{i+1} {'MANUAL' if high else 'OFF'}")

    def status(self):
        text = ';'.join(f"C{i+1},{'ON' if i in self.on else 'MANUAL' if i in self.manual else 'OFF'}" for i in range(20))
        text += f';INTERVENTION,{int(self.intervention)};PWROK,NA,NA,NA'
        self.intervention = False
        return text
