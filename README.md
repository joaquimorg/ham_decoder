# RX Analyzer

Projeto evolutivo para um descodificador/analisador de sinais de rádio autónomo.

## Objetivo

Criar um equipamento baseado em ESP32-S3 capaz de:

- receber áudio de um rádio;
- analisar espectro e waterfall;
- identificar automaticamente o tipo de transmissão;
- descodificar CW, RTTY, FSK, PSK e outros modos;
- apresentar informação num display;
- usar TinyML para classificação de sinais;
- funcionar de forma autónoma, sem PC.

## Hardware

- ESP32-S3 DevKitC-1 N16R8 (16 MB flash, 8 MB PSRAM octal)
- entrada de áudio: **ADC interno do ESP32-S3** (o PCM1808 está desativado, ver [docs/HARDWARE.md](docs/HARDWARE.md))
- fonte de áudio de teste
- USB para alimentação/programação

### Entrada de áudio

| Componente | Ligação |
|---|---|
| C1 1 µF | áudio in → GPIO1 (J3 pino 4) |
| R1 10 kΩ | 3V3 → GPIO1 |
| R2 10 kΩ | GPIO1 → GND |
| massa do áudio | GND |

Máximo ~2,8 Vpp na entrada. Esquema, a ligação do PCM1808 e os GPIOs a evitar estão em [docs/HARDWARE.md](docs/HARDWARE.md).

## Firmware atual

1. captura áudio a 48 kHz com o ADC1 interno (12 bits);
2. remove a polarização DC e decima para 12 kHz;
3. calcula FFT, ruído de fundo e picos;
4. classifica o sinal (ruído, tom, CW, FSK/RTTY, voz);
5. descodifica CW (Morse) no tom detetado e RTTY (Baudot, 45,45/50/75 baud) nos dois tons FSK detetados;
6. escreve no monitor série o texto CW e as mudanças de sinal; com `SERIAL_REPORT 2` em `config.h`, escreve também a linha completa do waterfall em texto a cada segundo;
7. serve uma **página web** por Wi-Fi com espectro, waterfall, classificação, texto CW e configuração.

## Interface web

1. **Primeiro arranque (sem rede configurada):** a placa cria a rede **`RX-Analyzer`**, com a password `rxanalyzer`. Liga-te a ela e abre **http://192.168.4.1/**. Em "Rede Wi-Fi", escolhe a tua rede e carrega em "Guardar e reiniciar".
2. **A partir daí** a placa liga-se à tua rede. O endereço aparece no monitor série (`WEB: ligado a "...": http://192.168.x.y/`). O nome `rx-analyzer` também é anunciado ao router, mas nem todos os routers o resolvem.
3. Se a rede guardada falhar durante 20 s, a placa volta a abrir a `RX-Analyzer` e continua a tentar a tua rede em segundo plano.

Na página:

- **Dados em direto por WebSocket (`/ws`):** a placa envia, a cada 250 ms, as linhas novas do espetro (em binário) e o estado e o texto (em JSON). Se o WebSocket falhar 3 vezes seguidas, a página passa a pedir os dados a `/api/data` (indicador "ao vivo (pedidos)").
- **Espectro e waterfall:** ~12 linhas por segundo.
- **Tom do descodificador CW:** fica em automático, ou fixa-se em manual clicando no waterfall ou escrevendo a frequência.
- **Sensibilidade (contraste mínimo):** as definições de CW e de Wi-Fi ficam guardadas na placa (NVS).
- **Cores do waterfall:** ficam guardadas no browser.
- **Texto CW:** com botões para copiar e limpar.

Em alternativa, a rede pode vir já no firmware: copia `include/wifi_secrets.h.example` para `include/wifi_secrets.h`, que é ignorado pelo git.

Ainda não há display nem TinyML. Os testes estão em [docs/TEST_PLAN.md](docs/TEST_PLAN.md).

## Teste inicial

1. Montar o circuito de entrada no GPIO1.
2. Não ligar ainda uma saída de rádio desconhecida.
3. Usar uma fonte de áudio de baixo nível conhecida.
4. Compilar e gravar:

   `pio run -t upload`

5. Abrir:

   `pio device monitor`

6. Procurar `ADC started` e depois as linhas do waterfall.

Com um tom, o tom aparece no waterfall.

## Roadmap

### V0.1 - Aquisição de áudio
- [x] ESP32-S3
- [x] ADC interno em contínuo
- [ ] PCM1808 por I2S (módulos testados corrompem as amostras)
- [x] RMS/peak
- [ ] escolher o ADC definitivo

### V0.2 - DSP
- [x] buffer de áudio
- [x] DC removal
- [ ] filtros (só decimação boxcar por agora)
- [x] FFT
- [x] frequência dominante
- [ ] S-meter (há rms/pico em dBFS)
- [x] deteção de sinal

### V0.3 - CW
- [x] detector de tom (sintonia a partir do espetro, mistura em quadratura)
- [x] envelope (janela ~ponto/3, níveis por percentis)
- [x] DIT/DAH
- [x] timing adaptativo (marcas e espaços)
- [x] decoder Morse
- [x] estimativa WPM
- [x] validar com gravação real (W1AW 30 wpm, sem erros com o ADC interno)
- [ ] validar com o rádio

### V0.4 - Display
- [x] interface web (espectro, waterfall, texto, configuração) enquanto não há LCD
- [ ] ST7789
- [ ] spectrum
- [ ] waterfall
- [ ] modo
- [ ] SNR
- [ ] texto descodificado

### V0.5 - RTTY/FSK
- [x] FSK detector (dois picos no espetro, desvios de 170 a 850 Hz)
- [x] AFC (segue os tons detetados, ressintoniza acima de 15 Hz)
- [x] sincronização pelo bit de arranque (45,45 / 50 / 75 / 100 baud)
- [x] ITA2/Baudot (LTRS/FIGS, unshift on space)
- [x] RTTY 45.45 baud
- [x] RTTY 170 Hz shift
- [x] polaridade automática e squelch
- [ ] validar com sinais reais do rádio

### V0.6 - TinyML
- [ ] recolha de amostras
- [ ] dataset
- [ ] espectrogramas
- [ ] modelo de classificação
- [ ] quantização
- [ ] inferência no ESP32-S3

Classes iniciais previstas:

- NOISE
- VOICE
- CW
- RTTY
- FSK
- PSK31

### V0.7 - SD Card
- [ ] gravação de amostras
- [ ] configuração
- [ ] atualização de modelos

### V1.0
- [ ] caixa
- [ ] bateria
- [ ] USB-C
- [ ] entrada de áudio protegida
- [ ] encoder
- [ ] botões
- [ ] display
- [ ] firmware integrado

## Estrutura

```text
rx_analyzer/
├── platformio.ini
├── README.md
├── include/
│   ├── config.h
│   ├── audio_source.h
│   ├── analyzer.h
│   ├── cw_decoder.h
│   ├── rtty_decoder.h
│   ├── settings.h
│   ├── web_ui.h
│   └── web_page.inc     (página web embutida)
├── src/
│   ├── main.cpp
│   ├── pcm1808_source.cpp
│   ├── adc_source.cpp
│   ├── analyzer.cpp
│   ├── cw_decoder.cpp
│   ├── rtty_decoder.cpp
│   ├── capture.cpp
│   ├── settings.cpp     (definições em NVS)
│   └── web_ui.cpp       (Wi-Fi + servidor HTTP)
├── tools/
│   ├── cw_sim.py        (simulação do decoder CW no PC)
│   ├── rtty_sim.py      (simulação do decoder RTTY e gerador de WAV de teste)
│   └── capture_to_wav.py
└── docs/
    ├── ROADMAP.md
    ├── HARDWARE.md
    └── TEST_PLAN.md
```
