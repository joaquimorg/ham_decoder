# Extracts a CAPTURE_BEGIN/CAPTURE_END block from a saved monitor log (the
# log2file filter writes them to logs/) and writes it as a mono WAV file.
#
#   python tools/capture_to_wav.py logs/<ficheiro>.log captura.wav
#   python tools/cw_sim.py --file captura.wav --tone 750
import base64, re, sys, wave

if len(sys.argv) != 3:
    sys.exit('uso: capture_to_wav.py <log> <saida.wav>')

rate, data, inside, bad = None, bytearray(), False, 0
with open(sys.argv[1], encoding='utf-8', errors='ignore') as f:
    for line in f:
        line = line.strip()
        m = re.search(r'CAPTURE_BEGIN rate=(\d+) samples=(\d+)', line)
        if m:
            rate, expected, data, inside = int(m.group(1)), int(m.group(2)), bytearray(), True
            continue
        if inside and 'CAPTURE_END' in line:
            inside = False
            continue
        if inside:
            i = line.find('CAP:')
            if i >= 0:
                try:
                    data += base64.b64decode(line[i + 4:], validate=True)
                except ValueError:
                    bad += 1        # cut or mangled line (e.g. monitor closed mid-dump)

if rate is None:
    sys.exit('nenhuma captura encontrada no log')
got = len(data) // 2
print(f'{got} amostras a {rate} Hz ({got / rate:.1f} s), esperadas {expected}'
      + (f', {bad} linhas invalidas ignoradas' if bad else '')
      + ('' if got >= expected else '  <- captura incompleta'))
with wave.open(sys.argv[2], 'wb') as w:
    w.setnchannels(1)
    w.setsampwidth(2)
    w.setframerate(rate)
    w.writeframes(bytes(data[:got * 2]))
print('escrito', sys.argv[2])
