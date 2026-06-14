# Goose Overlay (X11 + Cairo + Miniaudio)

Um “desktop pet” interativo escrito em C puro, usando X11 e Cairo para renderização e Miniaudio para áudio.  
O projeto cria uma janela transparente sempre no topo onde um ganso animado anda, reage ao mouse e toca sons.

---

## Visão geral

Este projeto implementa:

- Janela overlay transparente (X11)
- Renderização 2D com Cairo
- Simulação de comportamento (state machine simples)
- Física básica (movimento, aceleração, direção)
- Interação com mouse (perseguição e “nabbing”)
- Áudio em memória com Miniaudio
- Animação procedural (pescoço, pés, corpo)

---

## Stack utilizada

- X11 → criação da janela, eventos e input
- Cairo → renderização vetorial 2D
- Miniaudio → reprodução de áudio direto da memória
- C (stdlib, math, time) → lógica, física e estados
- Python (embed_assets.py) → conversão de Assets/ em headers C

---

## Como compilar?

Antes de compilar o projeto, é obrigatório executar o script:

```bash
python embed_assets.py
```

Em seguida, execute:

```bash
make clean && make goose && ./goose
```