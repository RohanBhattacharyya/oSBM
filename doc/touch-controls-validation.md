# Relatório de validação dos controles de toque

Data: 2026-11-03. Branch: `codex/import-claude-touch-controls-20261103`.
PR de revisão: [Cleitin790/oSBM#3](https://github.com/Cleitin790/oSBM/pull/3).
Base da comparação: `a8cb06177b0efd48c4d4ab827786ec61edea3bfa` (importação original).

## Resultado por tarefa

1. **Lógica pura:** header sem engine/SDL integrado ao movimento, mira, rodas,
   swipe, rejeição de borda e fade. Teste standalone registrado com `NoAssets`.
2. **Mira/ataque:** três opções no launcher, padrão manual, deadzone/curva
   ajustáveis; cursor preciso usa tempo, thresholds de gesto em dp.
3. **Mineração/construção:** Shift alternado por botão, troca de mão da mira,
   donos independentes e proteção contra cliques ao soltar movimento durante ataque.
4. **Editor:** opacidade individual, quatro presets, restauração confirmada,
   layouts nomeados, desfazer troca e avisos sobre guias do HUD. A prévia usa
   guias aproximadas; confirmar o HUD real é impossível sem executar o jogo aqui.
5. **UI:** arrasto/inércia de scroll, alvos 48 dp, itens/Shift+clique, drag/drop
   sem descarte externo, fechamento de janela, encaminhamento de Voltar e área
   do IME convertida com insets. Teclado e recortes reais aguardam aparelho.
6. **Robustez:** cancelamento de contato/foco/tamanho/insets/contexto; ataque
   de soltura não dispara ao cancelar; janela aberta no lote SDL bloqueia
   os próximos ataques de toque. Tooltips não interrompem um arrasto válido.
7. **Custo:** buffer de rótulos/filas reutilizados, métricas de texto cacheadas,
   geometria sem vetores temporários, estatísticas em buffers fixos. Teste puro:
   20 mil ciclos sem alocar. Não é perfil de GPU/heap da engine completa.
8. **Documentação:** guia de layout/opções/gestos/limites e 18 cenários manuais
   para o Redmi em [touch-controls.md](touch-controls.md); README e checklist atualizados.

## Ferramentas e nível atingido

- Host Windows; Python 3.13; Zig 0.16.0 (Clang/libc++) para os testes e objetos desktop.
- NDK r26d, `26.3.11579264`, Clang real com target `aarch64-linux-android28` e sysroot Android.
- SDL `release-3.2.28`, ImGui `v1.91.9`, GLEW 2.3.0 para o caminho GL desktop.
- Duas matrizes: headers vendorizados da engine (fmt 10.2.1/xxHash 0.8.1) e
  clones solicitados de fmt `10.2.1`/xxHash `v0.8.2` priorizados no include path.
- Android: análise sintática dos oito C++ alterados, com headers reais, sem stubs.
  Windows launcher: compilação individual dos oito em objetos. Windows clássico:
  compilação dos seis compartilhados, sem `STAR_PLATFORM_MOBILE`.
- Não foram feitos link do cliente, CTest da engine, APK, execução no aparelho,
  testes de iOS/Switch/Linux/macOS ou medições de FPS/latência no Adreno 610.

## Testes e checks — saída real

`zig` abaixo é o executável portátil `../tooling/ziglang/zig.exe`; caminhos estão
normalizados para a raiz do repositório. Os flags dos testes não suprimem warnings.

```text
$ zig c++ -std=c++17 -Wall -Wextra -Werror -I source/core source/test/touch_input_test.cpp -o ../touch_input_tests.exe
Pure input allocation audit: 20000 frames, 0 allocations
Passed 7675 touch input checks
$ zig c++ -std=c++17 -Wall -Wextra -Werror -I source/core source/test/gamepad_input_test.cpp -o ../gamepad_input_tests.exe
Passed 31695 gamepad input checks
$ git diff --check a8cb061
PASS (empty output)
New haptic/vibration/rumble code matches: 0
touchElementsFromConfig: unchanged
defaultGamepadBindings: unchanged
Gamepad adapter implementation: unchanged
Legacy default touch IDs preserved: 16/16
android.yml: YAML parsed; workflow_dispatch/build_android present; unchanged from base
Gameplay/save source changes: 0 (no source/game files changed)
```

A compatibilidade de configs foi conferida por inspeção do parser/fallbacks,
preservação dos IDs e testes do merge de layouts com elementos personalizados,
desabilitados e remapeados. O carregador original está literalmente preservado.
Isso não substitui um round-trip dentro do jogo com o arquivo real do usuário.
O scan foi feito nas linhas novas de `source/`, separando código novo de qualquer
referência anterior. Arquivos de regras de gameplay/saves não foram alterados.

## Matrizes C++ — comandos e saídas reais

Verificador reproduzível incluído em `scripts/validate-touch-controls.py` (Python
3.12+). Salva argumentos completos por compilação, stderr, objetos desktop e
`results.json` no diretório informado. Android usa `-std=c++17 -fsyntax-only
-Wall -Wextra -DFMT_HEADER_ONLY=1` mais as defines de plataforma; desktop usa
`-std=c++17 -c -Wall -Wextra`, as defines Windows/PC e, no launcher, MOBILE.
Só desktop inclui `-Wno-nullability-completeness` para o toolchain Zig.
Avisos são comparados por mensagem/contagem com a base. O define de execução
invalida cache do Zig para não perder a reemissão dos avisos do baseline.

```powershell
python scripts/validate-touch-controls.py --work-dir ../validation-touch --ndk ../tooling/android-ndk-r26d --zig ../tooling/ziglang/zig.exe --sdl ../headers/SDL --imgui ../headers/imgui --glew ../headers/glew-2.3.0
```

```text
android-arm64: StarMobileControls.cpp: exit=0, warnings=6, baseline=6, new=0
windows-launcher: StarMobileControls.cpp: exit=0, warnings=6, baseline=6, new=0
android-arm64: StarMobilePlatform.cpp: exit=0, warnings=6, baseline=6, new=0
windows-launcher: StarMobilePlatform.cpp: exit=0, warnings=7, baseline=7, new=0
android-arm64: StarClientApplication.cpp: exit=0, warnings=5, baseline=5, new=0
windows-launcher: StarClientApplication.cpp: exit=0, warnings=13, baseline=13, new=0
windows-desktop: StarClientApplication.cpp: exit=0, warnings=13, baseline=13, new=0
android-arm64: StarLogging.cpp: exit=0, warnings=0, baseline=0, new=0
windows-launcher: StarLogging.cpp: exit=0, warnings=0, baseline=0, new=0
windows-desktop: StarLogging.cpp: exit=0, warnings=0, baseline=0, new=0
android-arm64: StarActionBar.cpp: exit=0, warnings=4, baseline=4, new=0
windows-launcher: StarActionBar.cpp: exit=0, warnings=9, baseline=9, new=0
windows-desktop: StarActionBar.cpp: exit=0, warnings=9, baseline=9, new=0
android-arm64: StarMainInterface.cpp: exit=0, warnings=7, baseline=7, new=0
windows-launcher: StarMainInterface.cpp: exit=0, warnings=12, baseline=12, new=0
windows-desktop: StarMainInterface.cpp: exit=0, warnings=12, baseline=12, new=0
android-arm64: StarTitleScreen.cpp: exit=0, warnings=4, baseline=4, new=0
windows-launcher: StarTitleScreen.cpp: exit=0, warnings=9, baseline=9, new=0
windows-desktop: StarTitleScreen.cpp: exit=0, warnings=9, baseline=9, new=0
android-arm64: StarPaneManager.cpp: exit=0, warnings=6, baseline=6, new=0
windows-launcher: StarPaneManager.cpp: exit=0, warnings=6, baseline=6, new=0
windows-desktop: StarPaneManager.cpp: exit=0, warnings=4, baseline=4, new=0
Matrix: 22 checks, failures=0
```

Repetição com os tags solicitados de fmt/xxHash:

```powershell
python scripts/validate-touch-controls.py --work-dir ../validation-touch-pinned --ndk ../tooling/android-ndk-r26d --zig ../tooling/ziglang/zig.exe --sdl ../headers/SDL --imgui ../headers/imgui --glew ../headers/glew-2.3.0 --fmt ../headers/fmt --xxhash ../headers/xxh
```

```text
android-arm64: StarMobileControls.cpp: exit=0, warnings=6, baseline=6, new=0
windows-launcher: StarMobileControls.cpp: exit=0, warnings=6, baseline=6, new=0
android-arm64: StarMobilePlatform.cpp: exit=0, warnings=6, baseline=6, new=0
windows-launcher: StarMobilePlatform.cpp: exit=0, warnings=7, baseline=7, new=0
android-arm64: StarClientApplication.cpp: exit=0, warnings=5, baseline=5, new=0
windows-launcher: StarClientApplication.cpp: exit=0, warnings=13, baseline=13, new=0
windows-desktop: StarClientApplication.cpp: exit=0, warnings=13, baseline=13, new=0
android-arm64: StarLogging.cpp: exit=0, warnings=0, baseline=0, new=0
windows-launcher: StarLogging.cpp: exit=0, warnings=0, baseline=0, new=0
windows-desktop: StarLogging.cpp: exit=0, warnings=0, baseline=0, new=0
android-arm64: StarActionBar.cpp: exit=0, warnings=4, baseline=4, new=0
windows-launcher: StarActionBar.cpp: exit=0, warnings=9, baseline=9, new=0
windows-desktop: StarActionBar.cpp: exit=0, warnings=9, baseline=9, new=0
android-arm64: StarMainInterface.cpp: exit=0, warnings=7, baseline=7, new=0
windows-launcher: StarMainInterface.cpp: exit=0, warnings=12, baseline=12, new=0
windows-desktop: StarMainInterface.cpp: exit=0, warnings=12, baseline=12, new=0
android-arm64: StarTitleScreen.cpp: exit=0, warnings=4, baseline=4, new=0
windows-launcher: StarTitleScreen.cpp: exit=0, warnings=9, baseline=9, new=0
windows-desktop: StarTitleScreen.cpp: exit=0, warnings=9, baseline=9, new=0
android-arm64: StarPaneManager.cpp: exit=0, warnings=6, baseline=6, new=0
windows-launcher: StarPaneManager.cpp: exit=0, warnings=6, baseline=6, new=0
windows-desktop: StarPaneManager.cpp: exit=0, warnings=4, baseline=4, new=0
Matrix: 22 checks, failures=0
```

Resultado: **44 verificações**, nenhuma falha e nenhum aviso adicional à base.
Os avisos anteriores incluem parâmetros vazios de `StarRenderer.hpp`, inicializadores
parciais de ações existentes, `result_of_t` e o `typeid` da instrumentação de panes.
Eles não foram ocultados para declarar uma compilação inteira sem warnings.

## Tentativa real de APK

NDK oficial verificado pelo checksum do catálogo Android; CMake 3.31.6, Ninja e
vcpkg instalados no workspace. Variáveis apontadas para essas ferramentas:
`VCPKG_ROOT`, `ANDROID_NDK_HOME`, `ANDROID_SDK_ROOT` e PATH de CMake/Ninja.
Tentativa equivalente ao comando abaixo (build dir fora do repo):

```text
$ cmake --preset android-arm64-debug -B ../android-build
-- Running vcpkg install
Fetching registry information from https://github.com/microsoft/vcpkg (HEAD)...
error: in triplet x64-windows: Unable to find a valid Visual Studio instance
Could not locate a complete Visual Studio instance
-- Running vcpkg install - failed
CMake Error: vcpkg install failed.
CMake Error: CMAKE_C_COMPILER not set, after EnableLanguage
CMake Error: CMAKE_CXX_COMPILER not set, after EnableLanguage
-- Configuring incomplete, errors occurred!
configure exit=1
```

O configure falhou antes de poder construir `android_apk`. O SDK de empacotamento
e adb também não estão configurados. Não foi gerado APK nem alterada assinatura,
SDK, ABI, compressão, VERSION ou workflow. O workflow foi lido como YAML e comparado
com a base; não foi disparada uma execução de Actions nesta etapa.

## Arquivos alterados nesta continuação

- `README.md`
- `doc/touch-controls-progress.md`
- `doc/touch-controls-validation.md`
- `doc/touch-controls.md`
- `docs/agents/build-and-validation.md`
- `scripts/validate-touch-controls.py`
- `source/application/StarApplication.hpp`
- `source/application/mobile/StarMobileControls.cpp`
- `source/application/mobile/StarMobileControls.hpp`
- `source/application/mobile/StarMobilePlatform.cpp`
- `source/client/StarClientApplication.cpp`
- `source/client/StarClientApplication.hpp`
- `source/core/StarInputEvent.hpp`
- `source/core/StarLogging.cpp`
- `source/core/StarLogging.hpp`
- `source/core/StarTouchInput.hpp`
- `source/frontend/StarActionBar.cpp`
- `source/frontend/StarActionBar.hpp`
- `source/frontend/StarMainInterface.cpp`
- `source/frontend/StarTitleScreen.cpp`
- `source/test/CMakeLists.txt`
- `source/test/touch_input_test.cpp`
- `source/windowing/StarPaneManager.cpp`
- `source/windowing/StarPaneManager.hpp`


Os cenários numerados para validação de aparelho estão na última seção de
[touch-controls.md](touch-controls.md). A revisão deve considerar que estes são
resultados de lógica/compilação, não demonstração visual de execução no Redmi.
