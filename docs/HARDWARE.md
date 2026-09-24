# Hardware notes

A fonte de áudio escolhe-se em `include/config.h` com `AUDIO_SOURCE`: `AUDIO_SRC_ADC` (por omissão) ou `AUDIO_SRC_PCM1808`.

## Entrada de áudio: ADC interno (atual)

Fonte por omissão (`AUDIO_SOURCE AUDIO_SRC_ADC`). Com a gravação W1AW a 30 wpm tocada num telemóvel, o espetro fica limpo (SNR ~40 dB, sem harmónicas) e o CW é descodificado sem erros.

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
- Usar só pinos do ADC1 (GPIO1..10 no S2 e no S3). O ADC2 é partilhado com o Wi-Fi.

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

## ESP32-S3 DevKitC-1 N16R8

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

## ESP32-S2 Mini

Usa os mesmos GPIOs: 8, 4, 6 e 5 para o PCM1808 e 1 (ADC1_CH0) para o ADC interno.

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
