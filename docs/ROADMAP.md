# Roadmap

## Princípio de arquitetura

Separar o projeto em quatro camadas:

```text
Hardware
   ↓
Audio acquisition
   ↓
DSP
   ↓
Modem / TinyML
   ↓
UI
```

O TinyML será utilizado principalmente para classificação de sinais. Os demoduladores/decoders serão determinísticos sempre que possível.

## Ordem de implementação

1. Aquisição de áudio (PCM1808 por I2S; ADC interno como alternativa)
2. FFT
3. deteção de sinal
4. CW
5. ST7789
6. RTTY/FSK
7. dataset
8. TinyML
9. PSK/FT8/etc.
10. hardware portátil
