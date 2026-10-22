# ZA Recomp

[English](README.md) | **Português (Brasil)**

Um port nativo para PC de **Pokémon Legends: Z-A** (Nintendo Switch, v1.0.0 e v2.0.0) feito por **recompilação
estática**: o código ARM64 do jogo é traduzido antecipadamente para C e compilado para x86-64, e o sistema operacional
e a GPU (NVN) do Switch são reimplementados no PC (Vulkan). Nenhum emulador roda durante o jogo.

Este é um fork do **[LAS Recomp](https://github.com/peiikai/LASRecomp)**, de **Peiikai**: o recompilador, o runtime e
a tradução NVN-para-Vulkan / de shaders Maxwell vêm todos desse projeto, feito para *The Legend of Zelda: Link's
Awakening*. Este fork o estende para rodar Pokémon Legends: Z-A.

> **Você precisa da sua própria cópia do jogo.** Nenhum arquivo, código, recurso ou fonte do jogo está incluído neste
> repositório.

## Imagens

| | |
|---|---|
| ![Cidade de Lumiose a 60 fps](docs/screenshots/lumiose.jpg) | ![Menu de configurações do port (F1)](docs/screenshots/settings.jpg) |
| Cidade de Lumiose, 60 fps | Menu de configurações do port (F1) |
| ![Menu do jogo](docs/screenshots/menu.jpg) | ![Bolsa](docs/screenshots/bag.jpg) |
| Menu do jogo (mod de tradução PT-BR) | Bolsa |

## Estado

Em desenvolvimento, jogável: inicia, passa pela introdução e pelos primeiros momentos da história na Cidade de
Lumiose, as batalhas funcionam, e roda a 60 fps estáveis numa GPU de desktop.

Funcionando:
- Renderização: personagens, animação (skinning na GPU / transform feedback), iluminação, descarte de objetos na GPU
  (com a oclusão Hi-Z do próprio jogo), texto, diálogos e interface
- **60 fps** (a versão do Switch roda a 30) na velocidade normal do jogo, ou 30 fps como no console - opção no menu
- Renderização em resolução nativa (1x = 1080p e acima), filtro anisotrópico 16x, opções de SMAA / FSR 1
- Controle (XInput / controles via SDL3), teclado e câmera pelo mouse
- Ícones de botão de controle Xbox (LB RB LT RT Menu View) ou do teclado (nomes das teclas) nos textos do jogo
- Pacotes de texturas HD: extrair as texturas do jogo, ampliá-las e carregá-las de volta (veja abaixo)
- Saves (o salvamento automático e os manuais do jogo)

Problemas conhecidos:
- Alguns ícones da interface (modo câmera, o PRESS A da tela de título) vêm dos arquivos do próprio jogo e ainda
  mostram os botões do Switch
- Tela preta rápida ocasional; listras rosas em algumas paredes à noite
- Testado até agora só no Windows com GPU NVIDIA (o código também compila no Linux e no macOS, sem testes com o Z-A)

## Como jogar

Requisitos: Windows 10/11 64 bits, uma GPU com Vulkan 1.3 e driver atualizado.

1. Compile (abaixo) ou pegue uma build pronta, e coloque os arquivos do jogo em `extracted/`:
   - `extracted/exefs/` (`main`, `main.npdm`, `rtld`, `sdk`) e `extracted/romfs/` + `extracted/romfs.bin`,
     extraídos da sua própria cópia do jogo.
2. Opcional: as fontes do sistema do seu console em `fonts/` (`tools/za/nca_font.py` as extrai de um dump do firmware
   com as suas próprias chaves), depois `python tools/za/xbox_font.py` e `python tools/za/keyboard_font.py` para os
   ícones de botão.
3. Rode o `lasrecomp.exe` (ele entra direto no jogo). O log vai para `logs/play.log`.

Controles (teclado): WASD anda, setas = direcional, Enter = A, Backspace = B, L = X, I = Y, Q / E = L / R,
1 / 3 = ZL / ZR, P = +, M = -, botão direito do mouse + mouse = câmera. F1 abre o menu do port (taxa de quadros,
contador de FPS, resolução, filtragem, controles, ícones de botão). F11 / Alt+Enter: tela cheia.

Configurações (`larecomp.ini`, também editáveis pelo menu F1):

| Configuração | Significado |
|---|---|
| `fps60 = 1` | 60 fps (0: 30 fps como no Switch) |
| `resolution = 1x` | resolução interna (`match` = tamanho da janela, `1x`..`4x` = 1080p x N) |
| `aniso16 = 1` | filtro anisotrópico 16x |
| `prompts = auto` | ícones de botão: teclado sem controle, Xbox com controle Xbox (`keyboard`, `xbox`, `nintendo` forçam um) |
| `bind_pad_a = south` ... | mapeamento do controle (pela letra: o A do controle Xbox é o A do jogo) |
| `tex_hd = 1` / `tex_dump = 0` | pacotes de texturas HD ligados / extrair as texturas do jogo |

### Texturas HD

1. Jogue com `tex_dump = 1` (ou `LARECOMP_TEX_DUMP=1`): toda textura que o jogo carrega é salva como
   `textures/dump/<hash>.png`.
2. `python tools/za/upscale_textures.py` monta `textures/hd/` a partir delas - só texturas de cor (normal maps e
   máscaras ficam as do jogo), com o [Real-ESRGAN](https://github.com/xinntao/Real-ESRGAN) (build ncnn-vulkan em
   `tools/realesrgan/`) ou, sem ele, Lanczos.
3. As texturas em `textures/hd/` são usadas automaticamente, em qualquer tamanho.

## Compilando o código (Windows)

Usa o MSYS2 (UCRT64: clang, SDL3, FFmpeg, shaderc, headers do Vulkan) e Python 3 - veja
[docs/BUILD-windows.md](docs/BUILD-windows.md) para os pacotes.

```sh
make gen                                   # converte o código do jogo (extracted/exefs) para C: generated/
make -j16 B=build/o1 GENARCH=-march=x86-64-v3 GENOPT="-O1 -DGUEST_VOLATILE"
```

Saem `build/o1/lasrecomp.exe` + `build/o1/lasgame.dll` (defina `LARECOMP_GAME_LIB` com o caminho da dll ao rodar da
pasta de build). `tools/za/` tem os scripts usados no desenvolvimento: `build-win.sh`, `play-win.sh`, `auto-win.sh`
(testes automáticos numa cópia descartável do save) e ajudantes de log / perfil.

Os patches específicos do jogo no código convertido (o gancho da taxa de quadros) ficam em `tools/patches.py`
(`ZA_PATCHES`, por build ID do jogo); todo o resto está no runtime (`runtime/`): os serviços HLE, a implementação do
NVN (`runtime/nvn/`), o backend Vulkan (`runtime/nvn/vulkan/`) e o tradutor de shaders Maxwell (`runtime/nvn/maxwell/`).

## Créditos

- **[Peiikai](https://github.com/peiikai)** - criador do [LAS Recomp](https://github.com/peiikai/LASRecomp): o
  recompilador estático, o runtime, os renderizadores NVN / Vulkan / Metal e o tradutor de shaders sobre os quais este
  port foi construído.
- **Contribuidores do LAS Recomp** - [@dvilelaf](https://github.com/dvilelaf), [@Tensorlong](https://github.com/Tensorlong),
  [@mansarip](https://github.com/mansarip), [@we11adam](https://github.com/we11adam),
  [@Maganra](https://github.com/Maganra), [@hjqcan](https://github.com/hjqcan),
  [@gitarre1234rr5](https://github.com/gitarre1234rr5), [@rkammoun](https://github.com/rkammoun) e todos que relataram
  problemas - veja [docs/LASRecomp-README.md](docs/LASRecomp-README.md#credits) para a contribuição de cada um.
- **[Hexveil404](https://github.com/Hexveil404)** - o port de Pokémon Legends: Z-A (este fork).
- **[Ryujinx](https://github.com/Ryujinx/Ryujinx)** - usado como referência de comportamento durante o port (nenhum
  código copiado).
- **[envytools](https://github.com/envytools/envytools)** e **[Switchbrew](https://switchbrew.org)** - documentação da
  GPU NVIDIA e do sistema do Switch.
- Bibliotecas: [SDL 3](https://libsdl.org), [FFmpeg](https://ffmpeg.org), [Dear ImGui](https://github.com/ocornut/imgui),
  [Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator),
  [SMAA](https://github.com/iryoku/smaa), [AMD FSR 1](https://github.com/GPUOpen-Effects/FidelityFX-FSR),
  [shaderc](https://github.com/google/shaderc), [Capstone](https://www.capstone-engine.org),
  [Unicorn](https://www.unicorn-engine.org) e as outras listadas em
  [docs/LASRecomp-README.md](docs/LASRecomp-README.md#credits), com suas licenças e fontes.

## Aviso legal

ZA Recomp é um projeto de fã, sem afiliação, apoio ou patrocínio da Nintendo, Game Freak, Creatures ou The Pokémon
Company. *Pokémon* e *Pokémon Legends: Z-A* são marcas registradas de Nintendo / Creatures / GAME FREAK / The Pokémon
Company. Nenhum arquivo, código, recurso ou fonte do jogo está incluído. Você precisa fornecer sua própria cópia do
jogo, obtida legalmente.

## Licença

O código está sob a [Licença MIT](LICENSE) (LAS Recomp © Peiikai; as adições para o Z-A sob a mesma licença).
Componentes de terceiros mantêm suas próprias licenças - veja
[docs/LASRecomp-README.md](docs/LASRecomp-README.md#license) para a lista completa e as fontes dos componentes
incluídos.
