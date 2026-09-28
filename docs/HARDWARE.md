# Hardware notes

## Freenove FNK0104S (placa final)

Placa ESP32-S3 com ecrã de 4". Esquema: [4.0inch_ESP32-S3_Display_Schematic.pdf](4.0inch_ESP32-S3_Display_Schematic.pdf).
Documentação da Freenove: <https://github.com/Freenove/Freenove_ESP32_S3_Display>.

- **MCU:** ESP32-S3R8 (8 MB de PSRAM octal no chip) com 16 MB de flash quad (lida com `esptool flash_id`). A memória é igual à da N16R8, por isso as partições e a PSRAM são as mesmas.
- **USB-C:** é o USB nativo do ESP32-S3 (USB-Serial/JTAG) e não tem conversor UART. A consola vai por aí (`sdkconfig.defaults.fnk0104s`). O UART0 está no header P2.
- **Botões:** KEY1 = RESET, KEY2 = BOOT (GPIO0).

| Função | GPIO | Notas |
|---|---|---|
| LCD ST7796 (SPI) | SCK 12, MOSI 11, MISO 13, CS 10, DC 46 | 80 MHz; inversão de cores ligada; ordem BGR; o RESET do LCD está ligado ao EN do chip (só reset por software) |
| Retroiluminação | 45 | ativa alta (BSS138); PWM a 24 kHz |
| Touch FT6336U (I2C 0x38) | SDA 16, SCL 15, INT 17, RST 18 | mesmo barramento I2C do ES8311 |
| Codec ES8311 (I2C 0x18) | MCLK 4, BCLK 5, LRCK 7, dados do ADC → ESP **6**, ESP → DAC **8** | ainda não usado |
| Amplificador SC8002B | 1 (AUDIO_EN) | **ativo baixo**; o pull-up de 10 kΩ mantém-no desligado |
| Microfone MEMS | MIC1P do ES8311 | através de L3 (0 Ω) e C37 |
| Cartão SD (SDMMC 4 bits) | CLK 38, CMD 40, D0 39, D1 41, D2 48, D3 47 | ainda não usado |
| LED RGB WS2812 | 42 | ainda não usado |
| Bateria (divisor ×2) | 9 (ADC1) | ainda não usado |
| **Entrada de áudio atual** | **2** (ADC1_CH1) | header **P3 pino 1** (IO2) |
| Livres | 3, 14, 21 | header P3; I2C no P4 |

GPIO45 e GPIO46 são pinos de strapping, mas a placa já os usa (retroiluminação com pull-down e DC do LCD), por isso não é preciso nenhum cuidado. Basta não os aproveitar para outra coisa.

**Memória interna:** com o LCD, o Wi-Fi e o servidor web a RAM interna esgotava-se: a tarefa do servidor não arrancava e a placa entrava num ciclo de reinícios. Agora ficam na PSRAM o heap do LVGL (`lv_malloc_core` em `lcd_ui.cpp`) e os buffers do Wi-Fi/LWIP (`CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP`). Na RAM interna ficam os dois buffers DMA de 20 linhas do LCD.

### Próximo passo: áudio pelo ES8311

O codec tem ADC de 24 bits e PGA de 0 a 42 dB, com alimentação e massa analógicas próprias. Deve sofrer muito menos com o Wi-Fi do que o ADC interno.

Para o usar com o rádio é preciso mexer na placa:

1. Tirar o **L3 (0 Ω)** para desligar o microfone.
2. Injetar o áudio no pad do lado do **C37**, com um atenuador. O nível máximo de entrada ainda está por confirmar.

No firmware, falta um `es8311_source.cpp` com a mesma interface `audio_source_*`. Pode partir da base I2S do `pcm1808_source.cpp` e configurar o codec por I2C.

A fonte de áudio escolhe-se em `include/config.h` com `AUDIO_SOURCE`: `AUDIO_SRC_ADC` (por omissão) ou `AUDIO_SRC_PCM1808`.

## Entrada de áudio: ADC interno (atual)

Fonte por omissão (`AUDIO_SOURCE AUDIO_SRC_ADC`). O pino é o **GPIO2** na Freenove (header P3) e o **GPIO1** na DevKitC; nos esquemas abaixo, GPIO1 corresponde ao pino da placa usada. Com a gravação W1AW a 30 wpm tocada num telemóvel, o espetro fica limpo (SNR ~40 dB, sem harmónicas) e o CW é descodificado sem erros.

```text
                 C1 1 µF
áudio in ────────┤├────────┬──────── GPIO1 (ADC1_CH0)
                           │
          3V3 ── R1 10 kΩ ─┤
                           │
          GND ── R2 10 kΩ ─┘
áudio GND ─────────────────────────── GND
```

- As resistências R1 e R2 centram o sinal em ~1,65 V, mais ou menos a meio da escala do ADC (atenuação de 12 dB, ~0–3,1 V).
- O condensador C1 bloqueia a componente DC da fonte. Pode ser eletrolítico; nesse caso, o **+ fica do lado do GPIO1**.
- A entrada máxima é de **~2,8 Vpp** antes de cortar. Para saídas de auscultadores de rádio, começa com o volume baixo.
- Amostragem a 48 kHz por DMA, com decimação ×4 para 12 kHz.
- Tem ~60 dB de gama dinâmica útil (12 bits). Chega para validar a ideia.
- Usar só pinos do ADC1 (GPIO1..10 no S3). O ADC2 é partilhado com o Wi-Fi.

O pino e a atenuação estão em `include/config.h` (`ADC_INPUT_GPIO`, `ADC_INPUT_ATTEN`).

**Interferência do Wi-Fi:** as rajadas de transmissão do Wi-Fi aparecem no ADC interno como faixas de ruído de banda larga e riscas espúrias. Num dos testes, essas rajadas criaram marcas e espaços falsos que baixaram a velocidade estimada do CW até 59 wpm, e um sinal real a ~1600 Hz saía como `T T TT…`. O firmware desliga a poupança de energia do Wi-Fi. A potência de transmissão (`WIFI_TX_POWER_QDBM`) voltou a 19,5 dBm: a 11 e a 15 dBm a ligação à página ficava instável. Se a interferência no áudio for um problema e o sinal Wi-Fi for forte (a página mostra o RSSI), pode-se baixar. A página só pede dados 2,5 vezes por segundo, e o descodificador de CW ignora impulsos curtos isolados. No hardware ajuda:

- um condensador de **1 nF entre o GPIO1 e o GND**, junto ao pino, como filtro de RF (com os 5 kΩ da polarização, corta acima de ~30 kHz);
- fios de entrada curtos e afastados da antena do módulo, que fica na ponta oposta aos conectores USB;
- massa do áudio ligada diretamente ao GND da placa, junto ao GPIO1.

## Entrada de áudio: PCM1808 (módulo CJMCU roxo) — desativado

**Resultado:** os dois módulos testados não servem para descodificar. O canal LIN está morto (dá zero absoluto mesmo com sinal). No RIN, os bits de topo das amostras negativas chegam como 0, e quase todas as amostras negativas passam a positivas: o contador de simetria mostra 1–2% de amostras negativas, quando devia mostrar ~50%.

Esse erro destrói um dos semiciclos do tom. Daí vêm as harmónicas, a energia abaixo dos 100 Hz e as quebras dentro das marcas de CW, que davam ~25% de caracteres errados. O firmware mantém o suporte ao PCM1808 (`AUDIO_SOURCE AUDIO_SRC_PCM1808`) e os diagnósticos no log de 5 s: correções, silenciamentos, simetria e DMA.

Antes de o dar por perdido, falta confirmar que a resistência na linha OUT é mesmo de 22 Ω, ou ligar essa linha sem resistência. Uma resistência de valor errado atrasaria a subida do sinal e daria exatamente este erro.

O ESP32 é master I2S e o PCM1808 é slave: **MD0, MD1 e FMT ficam no GND**.

| Pino J1 | ESP32-S3 | Módulo PCM1808 | Função |
|---|---|---|---|
| 12 | GPIO 8 | SCK | MCLK 12,288 MHz (256 × fs) |
| 4 | GPIO 4 | BCK | Bit clock 3,072 MHz (64 × fs) |
| 6 | GPIO 6 | LRC | Word select 48 kHz |
| 5 | GPIO 5 | OUT | Dados áudio |
| 21 | 5V | +5V | Alimentação analógica |
| 1 | 3V3 | 3.3V | Alimentação digital |
| 22 | GND | GND, AGND, FMT, MD0, MD1 | Massa, I2S, modo slave |

**Obrigatório:** uma resistência de **22 Ω em série** em cada uma das quatro linhas (SCK, BCK, LRC, OUT), soldada **junto ao ESP32**. O fio do MCLK (GPIO8) deve ficar afastado dos outros três.

Sem estas resistências, os flancos dos clocks oscilam e o PCM1808 perde a sincronização: a saída fica a zero absoluto, com apenas alguns impulsos quando o sinal é forte. Isto aconteceu com os dois módulos testados, tanto em slave como em master. Antes das resistências, parecia que os módulos estavam avariados.

- Áudio no **RIN** (`PCM_CHANNEL 1`) ou no **LIN** (`PCM_CHANNEL 0`), com a massa no AGND. Máximo ~3 Vpp.
- Uma entrada sem nada ligado dá zero absoluto nesse canal.
- **Defeito conhecido dos módulos testados:** cerca de 2% das palavras chegam com o **byte do topo** (bits 23..16, os primeiros depois da transição do LRCK) errado, enquanto os 16 bits de baixo estão certos. Por exemplo, `0CA244` chega como `FCA244`.
- **O que já foi excluído:** a posição do pino do DOUT (GPIO5 → GPIO15), a frequência de amostragem (48 kHz → 24 kHz), a inversão do BCK e o MCLK com divisor inteiro. O alinhamento das palavras também está certo: o padding está sempre a 0 e o canal livre também.
- **Suspeita:** o próprio chip, por exemplo um PCM1808 falsificado. Para qualidade final, usar um ADC de origem fiável.
- **Contorno no firmware:** com `PCM_GLITCH_FIX 1`, as amostras que saltam mais de 2²⁰ em relação às duas vizinhas são corrigidas. O firmware mantém os 16 bits de baixo e escolhe o byte do topo que encaixa no sinal. A cada 5 s, o monitor mostra a contagem e alguns exemplos.

## ESP32-S3 DevKitC-1 N16R8 (alternativa, sem LCD)

Ambiente `esp32-s3-devkitc-1`: `pio run -e esp32-s3-devkitc-1 -t upload`.

Com as portas USB viradas para baixo, o PCM1808 liga ao header **J1** (lado esquerdo) e o GPIO1 do ADC interno fica no header **J3** (lado direito): G, TX, RX, **1**, 2, ...

**Atenção (clones N16R8, ex. YD-ESP32-S3):** o pino 5V vem isolado do USB por um díodo e fica a 0 V. Para ter 5 V à saída (para alimentar módulos externos) é preciso fazer uma ponte de solda no jumper **IN-OUT** junto ao pino 5V.

### GPIOs a evitar no S3 N16R8

| GPIO | Motivo |
|---|---|
| 0, 3, 45, 46 | Strapping (arranque) |
| 19, 20 | USB nativo (D−/D+) |
| 26..32 | Flash SPI interna |
| 33..37 | PSRAM octal (OPI), usados pelo N16R8 |
| 43, 44 | UART0 (consola, porta "UART") |
| 38 ou 48 | LED RGB da placa (38 na v1.1, 48 na v1.0) |

Livres e seguros para expansão futura (display, botões, etc.): 2, 7, 9..18, 21, 39..42, 47. Ocupados pela entrada de áudio: 4, 5, 6 e 8 (PCM1808) e 1 (ADC interno).

## Entrada de áudio no equipamento final

Para o primeiro teste usar uma fonte controlada.

No equipamento final:

```text
RADIO AF OUT
     |
     v
proteção
     |
     v
atenuador/ganho
     |
     v
filtro anti-alias
     |
     v
ADC
```

Não ligar diretamente uma saída de rádio desconhecida sem verificar o nível.
