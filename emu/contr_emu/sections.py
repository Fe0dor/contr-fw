"""Section register state and fault injection; no hardware I/O."""
DEFAULTS = (128, 0, 128, 512, 0)
ELEMENTS = ('POT', 'MTX', 'POT', 'POTH', 'MTX')
LIMITS = (256, 31, 256, 1023, 7)
ADDRESSES = (0x2f, 0x27, 0x2e, 0x2c, 0x27)


class Sections:
    def __init__(self, device, error):
        self.device, self.error = device, error
        self.on = [False, False]
        self.codes = [[None] * 5 for _ in range(2)]
        self.faults = {}
        self.pwrok_present = False
        self.pwrok = [False, False]

    def inject(self, bus, address, mode, count=1):
        """mode: transient, reset, power, missing, mismatch, or None to clear."""
        if mode not in (None, 'transient', 'reset', 'power', 'missing', 'mismatch'):
            raise ValueError('Unknown section fault')
        if mode is None:
            self.faults.pop((bus, address), None)
        else:
            self.faults[bus, address] = [mode, count]

    def off(self, bus):
        self.on[bus] = False
        self.codes[bus] = [None] * 5

    def fault(self, bus, address):
        f = self.faults.get((bus, address))
        if not f:
            return False
        if f[0] == 'transient':
            if f[1] <= 0:
                del self.faults[bus, address]
                return False
            f[1] -= 1
        return True

    def power_on(self, bus):
        self.off(bus)
        if self.pwrok_present and not self.pwrok[bus]:
            self.device.advance(501)
            raise self.error('SECTION_FAIL', '2')
        self.device.advance(103)
        # The hardware initialization also checks every chip, not just the requested one.
        for address in (0x27, 0x2c, 0x2f, 0x2e):
            if self.fault(bus, address):
                raise self.error('SECTION_FAIL', '4')
        self.codes[bus] = list(DEFAULTS)
        self.on[bus] = True

    def power(self, args):
        section, action = (s.upper() for s in args)
        if section not in ('A', 'B') or action not in ('ON', 'OFF'):
            raise self.error('RANGE')
        bus = ord(section) - ord('A')
        if action == 'OFF':
            self.off(bus)
        elif not self.on[bus]:
            self.power_on(bus)
        return 'OK'

    def set(self, args):
        channel, element, raw = args
        channel, element = channel.upper(), element.upper()
        if channel not in ('R1', 'R2', 'R3', 'R4'):
            raise self.error('RANGE')
        number = int(channel[1]) - 1
        bus = number // 2
        indices = range(2, 5) if number % 2 else range(2)
        e = next((e for e in indices if ELEMENTS[e] == element), None)
        if e is None:
            raise self.error('RANGE')
        if not raw or not raw.isascii() or not raw.isdecimal() or len(raw) > 512 or int(raw) > LIMITS[e]:
            raise self.error('RANGE', f'0..{LIMITS[e]}')
        value = int(raw)
        if not self.on[bus]:
            raise self.error('SECTION_OFF')
        address = ADDRESSES[e]

        def attempt():
            if self.fault(bus, address):
                return False
            self.codes[bus][e] = value
            return True

        def log(stage):
            text = f'{channel},{element},{stage}'
            self.device.push_error('I2C_FAIL', text)
            self.device.log_event('I2C ' + text)

        if attempt():
            return 'OK'
        for _ in range(3):
            log('RETRY')
            if attempt():
                return 'OK'
        if e in (1, 3, 4):
            log('RESET')
            self.device.advance(4)
            if self.faults.get((bus, address), [None])[0] == 'reset':
                self.inject(bus, address, None)
            if attempt():
                return 'OK'
        readback = str(value ^ 1) if self.faults.get((bus, address), [None])[0] == 'mismatch' else '?'
        log('POWER')
        self.off(bus)
        self.device.generation += 1
        self.device.advance(101)
        for addr in set(ADDRESSES):
            if self.faults.get((bus, addr), [None])[0] == 'power':
                self.inject(bus, addr, None)
        try:
            self.power_on(bus)
            if attempt():
                return 'OK'
        except self.error:
            pass
        self.off(bus)
        raise self.error('I2C_FAIL', f'{channel},{element},{readback}')

    def status(self):
        fields = [f'{s}:{"ON" if self.on[b] else "OFF"}' for b, s in enumerate('AB')]
        for bus in range(2):
            for e in range(5):
                value = self.codes[bus][e]
                fields.append(f'R{bus * 2 + (1 if e < 2 else 2)},{ELEMENTS[e]},{"?" if value is None else value}')
        return ';'.join(fields)
