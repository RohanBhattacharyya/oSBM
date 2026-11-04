# Layout de toque confortável

Base: main após o PR #5 (`953c053fc498cb27bf32170c1b0c2865f9862d04`).
O objetivo é reduzir a interferência entre os controles e o HUD visto na captura
fornecida pelo usuário, preservando as configurações existentes.

## Como experimentar

No launcher, abra **Controls → Layout preset → Comfortable (recommended)**,
pressione **Apply positions and sizes**, ajuste na prévia, **Done** e **Save**.
A seleção é uma sugestão: não aplica nem grava mudanças sozinha. **Undo layout
switch** permite voltar ao estado anterior enquanto estiver no launcher;
continua sendo necessário **Save** para persistir a escolha.

- Mira passa de X=66% para 78% da área segura, mais perto do polegar direito.
- Pulo fica maior; interação e os dois botões de uso das mãos ficam próximos.
- Pausa, chat e rodas ficam abaixo do HUD superior, no lado esquerdo; bloco
  único e troca de mão ficam junto desse grupo secundário.
- O conjunto padrão de 13 controles continua disponível. O preset não desativa
  botões, muda ações, macros, rótulos, modos de pressão ou opacidade, nem move
  elementos personalizados. Configurações antigas não são migradas à força.
- Um fundo escuro discreto melhora a separação dos controles do cenário; segue
  a opacidade individual/global e o fade já existentes. O fundo do joystick
  de movimento acompanha sua origem flutuante.
- Prévia e alertas de sobreposição agora usam a mesma política de tamanho do
  jogo. Antes, a prévia usava 7,5% da área segura e subestimava o raio em telas
  pequenas, enquanto o jogo usava um mínimo de 56 unidades lógicas.

A posição confortável é uma alternativa de toque, não um perfil automático
por combate/construção/mineração. O esquema universal de gamepad e mouse não
foi alterado. Os presets antigos continuam disponíveis. Não há alterações em
saves, assinatura, identidade, compactação, SDK, ABI ou configurações do APK.
Para um próximo APK, manter `.github/workflows/android.yml`: Release, arm64,
`system_allocator=true`, sem HWASan.

## Validação no Cloud Linux x64

Comandos executados a partir da raiz do repositório:

```sh
g++ -std=c++17 -O2 -Wall -Wextra -Werror -I source/core source/test/touch_input_test.cpp -o build/touch-ergonomics/touch_input_tests
build/touch-ergonomics/touch_input_tests
g++ -std=c++17 -O2 -Wall -Wextra -Werror -I source/core source/test/gamepad_input_test.cpp -o build/touch-ergonomics/gamepad_input_tests
build/touch-ergonomics/gamepad_input_tests
g++ -std=c++17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -I source/core source/test/touch_input_test.cpp -o build/touch-ergonomics/touch_input_sanitized
build/touch-ergonomics/touch_input_sanitized
git diff --check
```

- Toque: **8.199 verificações aprovadas**, inclusive limites e separação visual
  dos 13 círculos padrão em 960×540, 1230×562 (área útil da captura), 1280×720,
  1920×1080 e 2340×1080, com tamanho global padrão. Isso não garante separação
  dos alvos expandidos de toque, controles extras ou escalas maiores.
- Preservação de ação remapeada, botão desabilitado e geometria personalizada:
  aprovada no teste da política usada pelo preset.
- Gamepad: **31.695 verificações aprovadas**.
- Toque com ASan/UBSan: aprovado, sem relatório dos sanitizers. A primeira
  execução no sandbox não pôde concluir LeakSanitizer por ptrace; a execução
  fora desse rastreamento passou com os sanitizers habilitados.
- Auditoria existente: **20.000 quadros, zero alocações na lógica pura**.
  Não é uma medição de custo do desenho do overlay ou do jogo.
- `StarMobileControls.cpp` e `StarMobilePlatform.cpp`: compilação de objetos
  Linux launcher aprovada com GCC 14, C++17/O1/NDEBUG e macros
  `STAR_PLATFORM_MOBILE`, `STAR_PLATFORM_PC`, `STAR_SYSTEM_LINUX`,
  `STAR_SYSTEM_FAMILY_UNIX`, `STAR_COMPILER_GNU`, `STAR_LITTLE_ENDIAN`.
  Usou headers reais SDL 3.2.28, ImGui 1.91.9 e dependências já instaladas;
  não substituiu callbacks/engine por stubs. Comandos e logs ficam no diretório
  ignorado `build/touch-ergonomics/` (`check-production.py`). Não foi linkado o
  aplicativo nem executada sua UI; não é prova de compilação Android.
- Workflow Android, Gradle do aplicativo, presets, CMake do cliente e VERSION:
  idênticos à base; `git diff --check` passou.

Não foram produzidos APKs ou executados testes de aparelho, iOS, Switch,
Windows/macOS, `core_tests` ou `game_tests` nesta etapa. O conjunto compartilhado
precisa da validação Android/SDL real antes de afirmar conforto ou ganho de FPS.

## Conferência no celular

1. Antes de aplicar, confira que a configuração antiga abre nas mesmas posições
   e mantém macros, remapeamentos, controles desabilitados e elementos extras.
2. Aplique Comfortable: confira os botões sobre o HUD real, incluindo missões,
   barra rápida, janelas e escala manual da GUI. Use Undo e reaplique; salve e
   reinicie para conferir persistência.
3. Mova com o polegar esquerdo e use mira, pulo, interação, L/R e tecnologia;
   confira o alcance sem ativar o vizinho. O preset não muda Aim only/ataque.
4. Confira rodas, chat, pausa, bloco único, troca de mão, mouse e gamepad.
   Perda de foco, abertura de janelas e soltura de dedos devem continuar
   liberando ações. Teste as duas orientações de paisagem e os insets.
5. Confira o fundo em cenários claros/escuros, joystick flutuante, botão
   pressionado/toggle, diferentes opacidades e fade ocioso. Ajuste tamanhos e
   posições no editor se a área segura, densidade ou HUD do aparelho exigir.

## Integração após o PR #4

Base atual: `f2087ac1`, já com a trava de alvo opcional. O Comfortable também
posiciona Lock/Next abaixo do HUD, à direita do grupo de atalhos da esquerda.
O novo passe de fundo respeita o bloqueio desses botões quando a função está
desligada, assim como o desenho e o hit-test já faziam. A configuração permanece
opt-in e nenhum remapeamento do gamepad foi alterado.

Na integração, os checks geométricos incluem os dois botões opcionais: **8.354
checks de toque**, **148 de trava**, **31.695 de gamepad** e **102 de escala da
GUI** passaram no Cloud Linux. Os dois objetos de controles/launcher foram
recompilados. Não houve APK, teste de aparelho ou validação iOS/Switch nesta
integração; o workflow Android continua intacto, com Release/arm64 e
system_allocator=true, sem HWASan para o próximo build.
