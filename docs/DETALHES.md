# RX Analyzer — detalhes técnicos (PT)

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

- **Freenove FNK0104S** (placa final): ESP32-S3R8 (8 MB PSRAM octal), 16 MB de flash, LCD de 4" 480×320 (ST7796) com touch capacitivo (FT6336U), codec ES8311, cartão SD, bateria e USB-C. Esquema em [docs/4.0inch_ESP32-S3_Display_Schematic.pdf](docs/4.0inch_ESP32-S3_Display_Schematic.pdf).
- em alternativa, **ESP32-S3 DevKitC-1 N16R8** (sem LCD, só a página web)
- entrada de áudio: **ADC interno do ESP32-S3**. Na Freenove, o codec ES8311 (I2S) fica para uma fase seguinte; ver [docs/HARDWARE.md](docs/HARDWARE.md)
- fonte de áudio de teste
- USB para alimentação/programação

### Entrada de áudio

O pino de entrada depende da placa:

- **Freenove FNK0104S:** GPIO2, no header **P3 pino 1** (IO2). O GPIO1 desta placa é o enable do amplificador do altifalante.
- **DevKitC:** GPIO1 (J3 pino 4).

| Componente | Ligação |
|---|---|
| C1 1 µF | áudio in → GPIO de entrada |
| R1 10 kΩ | 3V3 → GPIO de entrada |
| R2 10 kΩ | GPIO de entrada → GND |
| massa do áudio | GND |

Máximo ~2,8 Vpp na entrada. Esquema, a ligação do PCM1808 e os GPIOs a evitar estão em [docs/HARDWARE.md](docs/HARDWARE.md).

## Firmware atual

1. captura áudio a 48 kHz com o ADC1 interno (12 bits);
2. remove a polarização DC e decima para 12 kHz;
3. calcula FFT, ruído de fundo e picos;
4. classifica o sinal (ruído, tom, CW, FSK/RTTY, voz);
5. descodifica CW (Morse) no tom detetado e RTTY (Baudot, 45,45/50/75 baud) nos dois tons FSK detetados;
6. descodifica imagens **FAX** (WEFAX 60/90/120/240 lpm, IOC 576/288) e **SSTV** (Martin, Scottie, Robot, PD), e mensagens **FT8/FT4** (com a biblioteca [ft8_lib](components/ft8_lib/README.md));
7. escreve no monitor série o texto CW e as mudanças de sinal; com `SERIAL_REPORT 2` em `config.h`, escreve também a linha completa do waterfall em texto a cada segundo;
8. classifica o sinal também com um modelo **TinyML** (rede MLP int8, ver abaixo);
9. mostra tudo no **LCD com touch** da Freenove (ver abaixo);
10. serve uma **página web** por Wi-Fi com espectro, waterfall, classificação, texto CW/RTTY, imagens FAX/SSTV, mensagens FT8/FT4 e configuração. A página web é **opcional**.

Os descodificadores entregam o resultado a `src/ui_hub.cpp`. O LCD e a página web leem daí, cada um ao seu ritmo, e nenhum depende do outro.

## Ecrã (LCD)

Só na Freenove FNK0104S (`LCD_UI` em `config.h`). Ecrã em paisagem, com uma barra de estado em cima (tipo de sinal, SNR, Wi-Fi) e cinco separadores em baixo:

- **RX:** espectro e waterfall, e as duas últimas linhas de CW e de RTTY.
  - Tocar no espectro ou no waterfall fixa o tom do CW nesse ponto (linha branca no espectro).
  - Um toque longo volta ao tom automático.
- **Texto:** as últimas 25 linhas de CW e de RTTY, cada canal com um botão **Limpar**.
- **FT8:** o modo (Desligado / FT8 / FT4), o estado e as mensagens, com as mais recentes no topo (ficam 50).
- **Imagem:** a imagem FAX/SSTV em curso, com os botões **FAX** (iniciar agora) e **Parar**.
  - O FAX ocupa a largura toda: as linhas mais recentes ficam em baixo e as antigas saem por cima. O SSTV aparece inteiro.
  - Tocar na imagem troca entre as duas vistas (largura toda / imagem inteira).
- **⚙ Definições:**
  - idioma (**Português / English**, muda logo);
  - CW, RTTY, FAX e SSTV (as mesmas da página web);
  - brilho;
  - Wi-Fi/página web ligada ou desligada, com o botão Reiniciar;
  - informação da placa.

As definições ficam guardadas na placa (NVS).

As fontes do ecrã são a Montserrat com os caracteres acentuados, porque as do LVGL só trazem ASCII. Estão em `components/ui_fonts` e voltam a gerar-se com `tools/gen_fonts.sh`.

## Interface web

A página web é opcional:

- `WEB_UI 0` em `config.h` tira da compilação o Wi-Fi e a página.
- Com `WEB_UI 1`, liga-se e desliga-se no LCD em **⚙ → Wi-Fi e página web**, e aplica-se ao reiniciar.
- Sem Wi-Fi não há hora UTC por NTP, e o FT8/FT4 não descodifica, porque a placa não tem RTC.

0. **Na Freenove (com LCD), a rede configura-se no próprio ecrã:** **⚙ → Redes Wi-Fi...** procura as redes, escolhes uma e, se for protegida, escreves a password no teclado do ecrã (há a opção «Mostrar»). A placa liga-se logo, sem reiniciar, e diz se a password está errada ou se a rede não foi encontrada. Requer o Wi-Fi ligado (e a placa reiniciada depois de o ligar).
1. **Primeiro arranque (sem rede configurada):** a placa cria a rede **`RX-Analyzer`**, com a password `rxanalyzer`. Liga-te a ela e abre **http://192.168.4.1/**. Em "Rede Wi-Fi", escolhe a tua rede e carrega em "Guardar e reiniciar".
2. **A partir daí** a placa liga-se à tua rede. O endereço aparece no monitor série (`WEB: ligado a "...": http://192.168.x.y/`). O nome `rx-analyzer` também é anunciado ao router, mas nem todos os routers o resolvem.
3. Se a rede guardada falhar durante 20 s, a placa volta a abrir a `RX-Analyzer` e continua a tentar a tua rede em segundo plano.

Na página:

- **Dados em direto por WebSocket (`/ws`):** a placa envia, a cada 250 ms, as linhas novas do espetro (em binário) e o estado e o texto (em JSON). Se o WebSocket falhar 3 vezes seguidas, a página passa a pedir os dados a `/api/data` (indicador "ao vivo (pedidos)").
- **Espectro e waterfall:** ~12 linhas por segundo.
- **Tom do descodificador CW:** fica em automático, ou fixa-se em manual clicando no waterfall ou escrevendo a frequência.
- **Sensibilidade (contraste mínimo):** as definições de CW e de Wi-Fi ficam guardadas na placa (NVS).
- **Cores do waterfall:** ficam guardadas no browser.
- **Texto CW e RTTY:** com botões para copiar e limpar. Ficam as últimas 30 linhas.
- **FT8 / FT4:** escolhe o modo; aparece uma tabela (últimas 50, as mais recentes em cima) com hora UTC, SNR (dB em 2500 Hz, como o WSJT-X), DT, frequência e mensagem. A hora vem por NTP (`pool.ntp.org`). No ponto de acesso, sem internet, a placa usa a hora do browser.
- **Imagem (FAX / SSTV):** o SSTV começa sozinho com o cabeçalho VIS. O FAX começa com o tom de início, alinha-se pelas linhas de fase e pára com o tom de fim; «Iniciar FAX agora» apanha uma emissão a meio. «Guardar PNG» grava a imagem com a proporção certa.

Em alternativa, a rede pode vir já no firmware: copia `include/wifi_secrets.h.example` para `include/wifi_secrets.h`, que é ignorado pelo git.

Os testes estão em [docs/TEST_PLAN.md](docs/TEST_PLAN.md).

## Teste inicial

1. Montar o circuito de entrada no GPIO2 (Freenove, header P3) ou no GPIO1 (DevKitC).
2. Não ligar ainda uma saída de rádio desconhecida.
3. Usar uma fonte de áudio de baixo nível conhecida.
4. Compilar e gravar:
   - Freenove (ambiente por omissão): `pio run -t upload`
   - DevKitC: `pio run -e esp32-s3-devkitc-1 -t upload`

   Na Freenove, a consola e a gravação vão pela USB-C nativa (USB-Serial/JTAG). Se a placa não aparecer no PC (por exemplo, num ciclo de reinícios), põe-na em modo de gravação: carrega em **BOOT** (KEY2), carrega e larga **RESET** (KEY1), e larga **BOOT**.

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
- [ ] escolher o ADC definitivo (a seguir: o codec ES8311 da Freenove, por I2S)

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
- [x] interface web (espectro, waterfall, texto, configuração), agora opcional
- [x] LCD ST7796 480×320 com touch FT6336U (Freenove FNK0104S, LVGL 9)
- [x] spectrum
- [x] waterfall (toque fixa o tom do CW)
- [x] modo
- [x] SNR
- [x] texto descodificado
- [x] FT8/FT4, imagem FAX/SSTV e definições no LCD
- [x] interface em português e inglês

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
- [x] recolha de amostras (`ML_LOG_FEATURES` em `config.h` + `tools/ml/log_to_npz.py`)
- [x] dataset (sintético: `tools/ml/synth.py`, com harmónicos, reverberação, fading e ruído de SSB)
- [x] espectrogramas / features (espetro à volta do pico, bandas largas, espetro de modulação do envelope)
- [x] modelo de classificação (MLP 110 → 32 → 16 → 6)
- [x] quantização (pesos int8, uma escala por camada)
- [x] inferência no ESP32-S3 (`src/classifier.cpp`)
- [x] primeiro teste com o rádio (CW: estável em CW, onde a heurística alterna CW/TOM)
- [x] treinar com exemplos reais do rádio: CW, RTTY (FSK 50 bd, shift 446 Hz), voz e PSK31; 175/180 segundos reais certos num conjunto de teste à parte

Classes: RUIDO, TOM, CW, RTTY (inclui FSK), PSK31, VOZ.

Treino (precisa de `numpy`):

```text
python tools/ml/train.py                         # gera include/ml_model.h
python tools/ml/classify_wav.py gravacao.wav     # testa o modelo num WAV
```

Para juntar exemplos reais: `ML_LOG_FEATURES 1` e `ML_LOG_LABEL "CW"` em `config.h`, gravar o monitor com o rádio nesse modo, e depois
`python tools/ml/log_to_npz.py logs/<ficheiro>.log -o real_cw.npz` e `python tools/ml/train.py --extra real_cw.npz`.

### V0.8 - FAX, SSTV, FT8/FT4
- [x] discriminador FM partilhado (`src/fm_demod.cpp`)
- [x] FAX: tons APT de início/fim, alinhamento pelas linhas de fase, 60/90/120/240 lpm, IOC 576/288
- [x] SSTV: cabeçalho VIS, Martin M1/M2, Scottie S1/S2/DX, Robot 36/72, PD50/90/120/160/180/240, correção de inclinação pelos sincronismos
- [x] FT8/FT4 com a ft8_lib (espectrograma na tarefa de análise, descodificação numa tarefa no core 0), hora por NTP
- [x] validação no PC: `tools/fax_sim.py`, `tools/sstv_sim.py`, `tools/ftx_test/`
- [ ] validar com sinais reais do rádio

### V0.7 - SD Card
- [ ] gravação de amostras
- [ ] configuração
- [ ] atualização de modelos

### V1.0
- [ ] caixa
- [ ] bateria
- [x] USB-C (Freenove)
- [ ] entrada de áudio protegida
- [ ] encoder
- [ ] botões
- [x] display (Freenove FNK0104S)
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
│   ├── fm_demod.h
│   ├── fax_decoder.h
│   ├── sstv_decoder.h
│   ├── ftx_core.h
│   ├── ftx_decoder.h
│   ├── settings.h
│   ├── ui_hub.h
│   ├── lcd_ui.h
│   ├── web_ui.h
│   └── web_page.inc     (página web embutida)
├── components/
│   ├── ft8_lib/         (biblioteca FT8/FT4, MIT)
│   └── ui_fonts/        (fontes do LCD com acentos)
├── src/
│   ├── idf_component.yml (LVGL, esp_lvgl_port, esp_lcd_st7796)
│   ├── main.cpp
│   ├── pcm1808_source.cpp
│   ├── adc_source.cpp
│   ├── analyzer.cpp
│   ├── cw_decoder.cpp
│   ├── rtty_decoder.cpp
│   ├── fm_demod.cpp     (discriminador FM para FAX e SSTV)
│   ├── fax_decoder.cpp
│   ├── sstv_decoder.cpp
│   ├── ftx_core.cpp     (FT8/FT4: períodos e descodificação, sem ESP-IDF)
│   ├── ftx_decoder.cpp  (FT8/FT4 na placa: hora UTC e tarefa de descodificação)
│   ├── capture.cpp
│   ├── settings.cpp     (definições em NVS)
│   ├── ui_hub.cpp       (dados para o LCD e para a página web)
│   ├── lcd_ui.cpp       (LCD + touch, LVGL)
│   └── web_ui.cpp       (Wi-Fi + servidor HTTP, opcional)
├── tools/
│   ├── cw_sim.py        (simulação do decoder CW no PC)
│   ├── rtty_sim.py      (simulação do decoder RTTY e gerador de WAV de teste)
│   ├── fax_sim.py       (simulação do decoder FAX e gerador de WAV de teste)
│   ├── sstv_sim.py      (simulação do decoder SSTV e gerador de WAV de teste)
│   ├── ftx_test/        (teste do FT8/FT4 no PC, em C++)
│   ├── gen_fonts.sh     (gera as fontes do LCD)
│   └── capture_to_wav.py
└── docs/
    ├── ROADMAP.md
    ├── HARDWARE.md
    └── TEST_PLAN.md
```
