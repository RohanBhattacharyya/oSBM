# Correção do carregamento de imagens com patches Lua

A branch `codex/android-world-entry-diagnostics` corrige acessos sem proteção ao
estado Lua compartilhado em `Assets::applyImagePatches`, além de proteger os
saves contra lotes inválidos e melhorar o diagnóstico Android. O formato dos
saves, remapeamentos e configurações de APK permanecem iguais.

## Evidência do aparelho

O primeiro diagnóstico veio da main `d9ce0cc`: `Wrong key size 0` ao aplicar os
dados da nave, seguido de falhas ao carregar a nave e segfault. Isso permitiu
identificar uma falha de proteção na gravação, mas não localizar o crash.

O novo diagnóstico confirma o APK arm64 `6acb4b353561aae6d9a5af27248ef0ea24db6fab`.
Ele inclui `crashes/native-0.pb` e a pilha no log. O primeiro segfault registrado
ocorre na thread de interface em `Assets::applyImagePatches`, chamado por
`GraphicsMenu -> OptionsMenu -> MainInterface` na entrada do mundo. O menu de
gráficos usa o patch incluído no projeto:
`assets/opensb/interface/graphicsmenu/body.png.patch.lua`.

Cerca de 58 ms depois há outro segfault na thread de geração de mundo, passando
por `jsonIntern` ao ler tilesets. Isso não prova uma falha independente nessa
função: corrupção anterior de memória pode aparecer em outras threads. A
correção deve ser retestada no aparelho para confirmar se elimina ambos.

O tombstone foi lido usando os campos do
[schema oficial do Android](https://android.googlesource.com/platform/system/core/+/refs/heads/main/debuggerd/proto/tombstone.proto).
Os arquivos privados, nomes de personagens e identificadores do aparelho não
fazem parte do commit.

## Falha corrigida

applyImagePatches liberava m_luaMutex antes de destruir `newResult`, e acessava
o userdata final e destruía `result` sem a trava. LuaEngine::getUserData altera
a pilha Lua; LuaHandle::~LuaHandle chama destroyHandle, que modifica a lista
m_handleFree, e seu RefPtr também tem contagem não atômica. Ao mesmo tempo,
Assets::workerMain usa a mesma trava para collectGarbage, que retira essa lista
e modifica o estado Lua. A trava incompleta permitia operações concorrentes
sobre essas estruturas.

A função agora mantém a trava recursiva durante todo o tempo de vida dos valores
Lua, incluindo extração da imagem, destrutores temporários e saída por exceção.
A ordem de declaração garante que os valores sejam destruídos antes da trava.
Imagens sem patches já evitavam esse caminho; seu comportamento não foi mudado.
O resultado e a ordem de aplicação dos patches permanecem os mesmos. A leitura
dos pequenos scripts também fica nesse trecho serializado.

A proteção anterior da nave continua presente: valida todas as chaves antes de
abrir ou criar o arquivo e faz rollback em erro durante a aplicação. Isso impede
que um lote de chaves inválidas seja parcialmente confirmado pelo destrutor do
banco. Não é uma conversão ou recuperação automática de um save já danificado.

## Validação

Correção nova, comparada com o APK `6acb4b3`:

- StarAssets.cpp: NDK r26d / Android arm64 API 28 passou na verificação de sintaxe
  com dois avisos, os mesmos da base.
- StarAssets.cpp: compilação por Zig 0.16 no Windows launcher e desktop passou;
  sete e oito avisos respectivamente, os mesmos da base, nenhum novo.
- Novo AssetsTest.ConcurrentImagePatches compilou nas três variantes. Gera 128
  imagens PNG temporárias, alternando sem patch, retorno da imagem original,
  retorno de imagem recortada e nil; solicita quatro rodadas com dois workers e
  oito leitores. **Não foi executado**: este host não dispõe do engine completo
  compilado. O teste usa fixtures próprias; o runner game_tests ainda inicializa
  Root e precisa dos assets do proprietário.
- git diff --check passou.

Comandos executados: `python ../validate_world_entry.py --baseline 6acb4b3 ...`
e `python ../check_image_tests.py`. Comandos completos, logs e objetos locais
estão em work/validation-image-patches, fora do repositório.

A etapa anterior também compilou o armazenamento no Android arm64 e Windows,
compilou todos os fontes Java contra Android API 35 e executou 12 verificações
de leitura limitada de relatérios na JVM. Os três testes novos de armazenamento
compilaram, mas não foram executados. Esses checks não foram repetidos porque
esta correção só altera Assets.cpp e seu teste.

O ZIP novo e a execução GitHub Actions #12 demonstram que a versóo de diagnóstico
foi compilada e executada no Android do usuário, ainda com crash. Isso NÃO valida
esta correção posterior. Nenhum APK da correção nova foi executado neste host ou
no aparelho; iOS e Switch não foram compilados ou executados.

Próximo passo: gerar Release / arm64 da branch atualizada, tentar entrar no mundo
e, caso ainda feche, exportar o diagnóstico novamente. O PR permanece separado
da main até revisóo e confirmação no aparelho.
