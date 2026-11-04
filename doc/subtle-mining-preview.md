# Prévia de mineração discreta no caminho móvel

Base: main `f48d54d2`. A prévia de BeamMiningTool usava a cor favorita do
personagem para substituir a iluminação dos blocos selecionados. A interface
acrescentava preenchimento e um contorno verde de alpha 220 e espessura 1,5.

Com `STAR_PLATFORM_MOBILE` (Android/iOS/Switch e desktop-launcher):

- A prévia mantém as mesmas posições/camadas, mas não substitui o light map.
- O contorno de mineração usa cinza claro (215,225,230), alpha 90 e espessura 1.
- Não há preenchimento dos blocos nessa prévia. Fora de alcance, conserva a
  indicação vermelha, agora com alpha 120, e o texto existente.
- O estilo discreto é preparado no update junto com o cache, sem consultar
  ferramentas durante o render. É reiniciado antes de cada atualização para
  não afetar a ferramenta seguinte.

Não são alterados dano, alcance, raio da área minerada, foreground/background,
Shift/bloco único, ritmo de ataque, drops, durabilidade ou a cor do feixe em si.
MaterialItem/ObjectItem continuam com a prévia colorida original. O desktop
clássico conserva também a iluminação e o contorno de mineração originais.
Esta mudança é visual fixa: não adiciona configuração nem migra saves.

Validação no Cloud Linux x64, GCC 14/C++17/O1: `StarTools.cpp` e
`StarMainInterface.cpp` compilados como objetos em desktop clássico e launcher,
quatro compilações aprovadas. Scripts/comandos e logs locais em
`build/touch-ergonomics/check-mining.py`, `check-mining-desktop.py` e arquivos
correspondentes `.log`. Headers reais SDL 3.2.28/ImGui 1.91.9 e dependências do
workspace; sem stubs. `git diff --check` passou. Não se acrescentou teste unitário
para valores cosméticos; não houve link/runtime da engine, CTest completo,
APK, validação Android/iOS/Switch nem execução no celular.

No próximo APK manter o workflow `.github/workflows/android.yml`: Release,
arm64, system_allocator=true, sem HWASan. Workflow, presets, assinatura,
identidade, compactação e VERSION não foram alterados.

Conferir no aparelho: equipar manipulador de matéria/ferramenta BeamMiningTool,
mover a mira com/sem ataque, testar bloco único e as duas camadas, fora de
alcance e cenários escuros/claros. Trocar para blocos e objetos deve restaurar
sua prévia colorida. Saves, controles e gamepad universal devem permanecer iguais.
A alteração não cobre desenhos próprios de ferramentas Lua/mods nem modifica
picaretas MiningTool que já não usam esse caminho de prévia.
