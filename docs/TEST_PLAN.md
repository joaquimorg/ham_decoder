# Test plan - V0.1

## Test 1 - boot

Expected:

```text
RX Analyzer - V0.2
ESP32-S3 + ADC interno
ADC started: GPIO1 (ADC1_CH0), Fs=48000 Hz, 12-bit
```

## Test 2 - no input

Com a entrada em silêncio:

- RMS baixo;
- peak baixo;
- valores relativamente estáveis.

## Test 3 - tone

Aplicar um tom de aproximadamente 700 Hz.

Expected:

- RMS sobe;
- peak sobe;
- FFT será implementada na V0.2 para confirmar a frequência.

## Test 4 - radio audio

Só depois dos testes anteriores:

1. ligar AF OUT do rádio;
2. começar com volume baixo;
3. confirmar que não existe clipping;
4. observar RMS/peak.

## Test 5 - clipping

Se o peak ficar constantemente no máximo:

- reduzir nível de entrada;
- verificar atenuador;
- não continuar a testar com nível excessivo.

# Test plan - V0.2 (espectro + classificação)

Cada segundo o monitor série mostra uma linha de waterfall em texto (0–3500 Hz,
50 Hz por coluna, 4 dB por nível acima do ruído: ` .:-=+*#%@`) seguida da classificação:

```text
      |0         500       1000      1500      2000      2500      3000      | Hz
   12 |             @                                                        | CW 702Hz 6e/s                SNR 34 rms -31 pk -18
```

| Entrada | Esperado |
|---|---|
| silêncio / ruído | `RUIDO` |
| tom contínuo 700 Hz | `TOM 700Hz` |
| CW (tom manipulado) | `CW 700Hz 20wpm  "CQ CQ DE ..."` |
| RTTY 170 Hz (ex.: 2125/2295) | `FSK 2125/2295Hz sh170 RTTY?` |
| voz / SSB | `LARGO 300-2800Hz (voz?)` |

`CLIP!` = entrada demasiado alta. `OVR` = a análise não acompanhou a captura.
`DIAG_VERBOSE 1` em `config.h` mostra ruído de fundo, picos e envelope por linha.

# Test plan - V0.3 (decoder CW)

## No PC (sem hardware)

`python tools/cw_sim.py --file cw_samples/260106_30WPM.mp3 --tone 750` descodifica a gravação real (precisa de `pip install miniaudio`).

`python tools/cw_sim.py` corre o mesmo algoritmo do `src/cw_decoder.cpp` sobre CW sintético (várias velocidades, jitter, desvio de frequência e ruído). Resultado de referência:

| caso | CER após a 1ª palavra |
|---|---|
| 12 / 20 / 30 wpm limpo | 0 % |
| jitter 15 %, desvio 8 Hz | 0 % |
| SNR 10 dB em 100 Hz | ~3 % |
| impulsos falsos de 10 ms, 2/s | ~1–7 % |
| arranque com velocidade 3x errada | recupera após ~1 palavra |
| gravação real W1AW 30 wpm (`--file`), 90 s | ~0 % |
| SNR 6 dB em 100 Hz | falha (limite atual) |
| só ruído, 10 s | sem texto |

## Na placa

1. Gerar CW com uma app de telemóvel ou um site de Morse (tom de 500–900 Hz, 15–25 wpm) e ligar a saída ao RIN.
2. Após ~1–2 s a etiqueta passa a `CW <freq>Hz <n>wpm` e o texto aparece no fim de cada linha, entre aspas.
3. A primeira letra pode perder-se enquanto o decoder sintoniza.
4. Um tom contínuo (`TOM`) não deve gerar texto.
5. Após ~5 s sem sinal o decoder dessintoniza.

### Primeiro teste real (exemplo W1AW a 30 wpm, tom 750 Hz)

A velocidade foi bem estimada (ponto de 40 ms) e a maior parte do texto saiu certa. Os erros vinham de marcas e espaços falsos de 10–15 ms. Esses impulsos partiam letras (`S` → `EEE`) e desviavam a estimativa de velocidade (para 47 ou 21 wpm). Foram corrigidos com um filtro proporcional à velocidade (`CW_DEBOUNCE_DIT`) e com uma adaptação da velocidade em passos limitados.

### Segundo e terceiro testes reais

Com o volume do telemóvel mais baixo, as harmónicas deixaram de aparecer. Não houve silenciamentos do PCM1808: o contador de sequências de zeros ficou a 0.

O erro estava na estimativa única da velocidade, que disparou de 30 para 6 wpm. Quando a estimativa subia, os traços passavam a contar como pontos e empurravam-na ainda mais para cima; o filtro e a janela, proporcionais ao ponto, apagavam os espaços. Foi corrigido de duas formas:

- pontos e traços passaram a ser seguidos como dois grupos separados;
- o filtro e a janela têm um limite máximo de 15 ms e 20 ms.

### Resultado com o ADC interno

Com a mesma gravação e o mesmo telemóvel, o texto sai **sem erros**. O ponto e o traço ficam estáveis (40/120 ms), o SNR é de ~40 dB e o espetro não tem harmónicas.

Para chegar aqui foram usados o modo de gravação (`CAPTURE_SECONDS`) e o `tools/capture_to_wav.py`. Mostraram que o sinal vindo do PCM1808 já chegava ao descodificador com um semiciclo destruído. O canal LIN do módulo estava morto, e no RIN só 1–2% das amostras eram negativas.

# Test plan - V0.5 (decoder RTTY)

## No PC (sem hardware)

`python tools/rtty_sim.py` corre o mesmo algoritmo do `src/rtty_decoder.cpp` sobre RTTY sintético:

| caso | CER |
|---|---|
| 45,45 bd / 170 Hz, limpo | 0 % |
| polaridade invertida (deteção automática) | 0 % |
| SNR 10 dB em 300 Hz | 0 % |
| SNR 6 dB em 300 Hz | ~2 % |
| desvio de 10 Hz, erro de baud +1 % | 0 % |
| 50 bd / 170 Hz, 75 bd / 850 Hz | 0 % |
| só ruído, 10 s | sem texto (squelch) |
| dois CW a 850/1020 Hz (falso FSK) | ~1 carácter em 10 s (verificação de falhas de nível + qualidade dos caracteres) |

Enquanto o RTTY descodifica caracteres bem formados, tem prioridade: o CW é dessintonizado e a etiqueta fica em `RTTY`. Num boletim real da ARRL, a marca do RTTY também passava por CW e aparecia lixo no texto CW.

`python tools/rtty_sim.py --wav ficheiro.wav` gera um sinal de teste (2125/2295 Hz, 45,45 bd). O `cw_samples/rtty_45bd_2125_2295.wav` já está gerado.

## Na placa

1. Toca `cw_samples/rtty_45bd_2125_2295.wav` no telemóvel para a entrada.
2. Após 1–2 s a etiqueta passa a `FSK 2125/2295Hz sh170 RTTY?` e, com o squelch aberto, a `RTTY 2125/2295Hz 45bd`.
3. O texto aparece no painel "Texto RTTY" da página web e no monitor série: `RYRYRY CQ CQ CQ DE CT1ABC ... THE QUICK BROWN FOX ... 0123456789 ...`.
4. No espetro, as linhas azuis marcam a marca (tracejado) e o espaço (pontilhado).

# Test plan - V0.8 (FAX, SSTV, FT8/FT4)

## No PC (sem hardware)

`python tools/fax_sim.py` corre o mesmo algoritmo do `src/fm_demod.cpp` + `src/fax_decoder.cpp` sobre FAX sintético (tom de início, 60 linhas de fase, imagem de teste, tom de fim):

| caso | resultado |
|---|---|
| 120 lpm limpo, desfasamento 0,37 e 0,8 de linha | alinhado, erro médio 0,03 |
| SNR 10 dB / 5 dB (em 2,5 kHz) | erro médio 0,08 / 0,13 (ruído nos pixels) |
| desvio de +40 Hz | erro médio 0,05 |
| 60 lpm, IOC 288 | alinhado |
| só ruído, 120 s | não arranca |

`python tools/sstv_sim.py` faz o mesmo para o `src/sstv_decoder.cpp`: todos os modos (Martin, Scottie, Robot, PD) com o VIS certo e todas as linhas; relógio do emissor ±0,2 % (inclinação corrigida); SNR 10–12 dB; só ruído, 120 s: nenhuma imagem. Nos modos mais rápidos (PD50, Scottie S2) as arestas mais finas ficam esbatidas pelo filtro FM.

`tools/ftx_test/run.sh` compila o `src/ftx_core.cpp` + ft8_lib no PC e testa FT8/FT4 sintético (GFSK, ruído, vários sinais por período, início a meio de um período):

| caso | resultado |
|---|---|
| FT8, 4 sinais de −8 a −18 dB, DT −0,4 a +1,2 s | todos, SNR ±0,7 dB, DT ±0,05 s |
| FT4, 3 sinais de −8 a −12 dB | todos |
| só ruído, 4 períodos | nenhuma mensagem |

Precisa de um compilador de C/C++; sem gcc/clang: `pip install ziglang` e `CC="python -m ziglang cc" CXX="python -m ziglang c++" tools/ftx_test/run.sh`.

Sinais de teste para tocar no telemóvel:

```text
python tools/fax_sim.py --wav fax.wav
python tools/sstv_sim.py --wav sstv.wav "Martin M1"     (ou "Scottie S1", "Robot 36", "PD120", ...)
python tools/fax_sim.py --file gravacao.wav             (descodifica um WAV para .pgm)
python tools/sstv_sim.py --file gravacao.wav            (descodifica um WAV para .ppm)
```

## Na placa

1. **FAX:** toca `fax.wav`. Após o tom de início (~1,5 s) o estado passa a «a alinhar» e depois a «a receber»; a imagem de teste (rampa, barras, diagonal) aparece direita e acaba com o tom de fim. Num emissor real (ex.: DWD Pinneberg 3855/7880/13882,5 kHz, Northwood 2618,5/4610/8040/11086,5 kHz) sintoniza em USB 1,9 kHz abaixo da frequência publicada. Se a imagem sair inclinada, ajusta `FAX_CLOCK_PPM` em `config.h`.
2. **SSTV:** toca `sstv.wav`; a imagem começa sozinha com o VIS e o título indica o modo. No ar: 14,230 MHz USB; ISS em 145,800 MHz FM (PD120/PD180).
3. **FT8/FT4:** escolhe FT8 na página. A hora tem de estar certa (NTP ou browser). Sintoniza 14,074 MHz USB (FT8) ou 14,080 MHz (FT4). A cada 15 s (7,5 s) aparecem as mensagens e o tempo de descodificação; compara com o WSJT-X no mesmo áudio, se possível. Verifica também que a carga da análise (canto superior) continua abaixo de ~80 %.

