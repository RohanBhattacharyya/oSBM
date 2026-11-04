# Validação da trava opcional

Base: main `d9ce0ccee45ddb2474314ed42b024e9736dba18f`. Branch: `codex/optional-target-lock`.

Os testes puros foram executados com Zig 0.16.0, C++17 e `-Wall -Wextra -Werror`:

```text
Passed 148 target lock checks
Pure input allocation audit: 20000 frames, 0 allocations
Passed 7675 touch input checks
Passed 31695 gamepad input checks
```

O teste de alocações acima pertence à lógica de toque preexistente, não é uma
medição de heap de toda a nova trava. A seleção usa estado de tamanho fixo e
só consulta candidatos em comandos explícitos. Durante acompanhamento valida
somente o alvo atual, com checagem de linha de visão no update. O render lê a
posição preparada, sem consultar entidades para desenhar o marcador.

Verificação dos quatro C++ modificados contra a base, usando NDK r26d real
arm64/API 28, Windows launcher e Windows clássico nos compartilhados:

```powershell
python scripts/validate-touch-controls.py --baseline d9ce0ccee45ddb2474314ed42b024e9736dba18f --work-dir ../validation-lock --ndk ../tooling/android-ndk-r26d --zig ../tooling/ziglang/zig.exe --sdl ../headers/SDL --imgui ../headers/imgui --glew ../headers/glew-2.3.0
```

```text
android-arm64: StarMobileControls.cpp: exit=0, warnings=6, baseline=6, new=0
windows-launcher: StarMobileControls.cpp: exit=0, warnings=6, baseline=6, new=0
android-arm64: StarMobilePlatform.cpp: exit=0, warnings=6, baseline=6, new=0
windows-launcher: StarMobilePlatform.cpp: exit=0, warnings=7, baseline=7, new=0
android-arm64: StarClientApplication.cpp: exit=0, warnings=5, baseline=5, new=0
windows-launcher: StarClientApplication.cpp: exit=0, warnings=13, baseline=13, new=0
windows-desktop: StarClientApplication.cpp: exit=0, warnings=13, baseline=13, new=0
android-arm64: StarMainInterface.cpp: exit=0, warnings=7, baseline=7, new=0
windows-launcher: StarMainInterface.cpp: exit=0, warnings=12, baseline=12, new=0
windows-desktop: StarMainInterface.cpp: exit=0, warnings=12, baseline=12, new=0
Matrix: 10 checks, failures=0
git diff --check: empty output (exit 0)
New haptic/vibration/rumble code matches: 0
```

Nenhuma regra de gameplay, save, compressão, ABI, versão, assinatura ou workflow
foi modificada. O recurso é desligado por padrão e ações antigas não foram
remapeadas. Os dois novos IDs de botão entram pelo merge existente.

Não houve build/link completo, geração de APK, execução no celular, validação
de bosses/mods reais ou compilação iOS/Switch/Linux/macOS. Detecção por barra
especial depende da convenção usada pelo mod. Instruções, limites e oito cenários
manuais estão em [optional-target-lock.md](optional-target-lock.md).
