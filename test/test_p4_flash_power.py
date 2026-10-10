"""Compare production C register traces with official esptool power_on_flash."""
import argparse
import json
from pathlib import Path
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--executable', required=True, type=Path)
parser.add_argument('--esptool-root', required=True, type=Path)
parser.add_argument('--output', required=True, type=Path)
args = parser.parse_args()
sys.path.insert(0, str(args.esptool_root))
from esptool.targets import esp32p4

class Reference(esp32p4.ESP32P4ROM):
    def __init__(self, eco, xpd):
        self.secure_download_mode = False
        self.revision = {5: 300, 6: 301, 7: 302, 8: 303}[eco]
        self.events = []
        self.registers = {0x501151bc: 0xa5a50200, 0x501151b8: 0x12550122,
                          0x501153fc: 0xdeadbeef, 0x5011010c: 0x89abcde1,
                          0x5012d034: (1 << 16) if xpd else 0}

    def get_chip_revision(self):
        return self.revision

    def read_reg(self, address):
        self.events.append(['read', address])
        return self.registers[address]

    def write_reg(self, address, value):
        self.events.append(['write', address, value])
        self.registers[address] = value

def native(eco, xpd, fail=0, p4=1):
    return json.loads(subprocess.check_output([str(args.executable), str(eco), str(xpd), str(fail), str(p4)]))

report = []
for eco in (5, 6, 7, 8):
    for xpd in (0, 1):
        reference = Reference(eco, xpd)
        esp32p4.sleep = lambda seconds: reference.events.append(['delay_ms', seconds * 1000])
        reference.power_on_flash()
        result = native(eco, xpd)
        c_io = [event for event in result['events'] if event[0] in ('read', 'write')]
        reference_io = [event for event in reference.events if event[0] in ('read', 'write')]
        assert result['result'] == 0 and c_io == reference_io, (eco, xpd, c_io, reference_io)
        c_delays = [event[1] for event in result['events'] if event[0] == 'delay_ms']
        reference_delays = [event[1] for event in reference.events if event[0] == 'delay_ms']
        assert len(c_delays) == len(reference_delays)
        assert all(c >= r for c, r in zip(c_delays, reference_delays))
        for failed_call in range(1, result['calls'] + 1):
            failed = native(eco, xpd, failed_call)
            assert failed['result'] == 2 and failed['calls'] == failed_call
        report.append({'eco': eco, 'xpd': xpd, 'events': result['events'],
                       'reference_matches': True, 'failed_io_cases': result['calls']})
for eco, p4 in ((99, 1), (7, 0)):
    result = native(eco, 1, p4=p4)
    assert result['result'] == 0 and result['events'] == [['end']]
args.output.write_text(json.dumps({'reference_esptool': '5.3.1', 'cases': report,
                                 'secure_and_other_target_unchanged': True}, indent=2))
print('8 official-reference cases, all I/O failure points, secure/other-target skip: PASS')
