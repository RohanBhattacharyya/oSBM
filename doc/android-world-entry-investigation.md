# Falha ao entrar no mundo no Android

Base investigada: main `d9ce0ccee45ddb2474314ed42b024e9736dba18f`.
Branch: `codex/android-world-entry-diagnostics`.

O diagnóstico fornecido pelo usuário confirma essa revisão no APK. A trava de
alvo do PR #4 não está nesse executável. O problema persiste com o overlay
oculto. Não há mods externos carregados no registro analisado.

O registro mais antigo mostra, ao receber ClientContextUpdate, `DBException:
Wrong key size 0`. Nas sessões seguintes a nave não pode ser carregada
(`InvalidMaybeAccessException`), e o jogo registra `Segfault Encountered!`.
Um registro também contém `No such material id: 55978`. Isso identifica
falhas reais, mas não prova qual componente produziu a chave vazia nem qual
instrução provocou a falha de memória. Não há pilha utilizável no ZIP original.
A memória registrada não demonstra encerramento por falta de memória.

Correção limitada e confirmada por inspeção: applyWorldChunksUpdateToFile abria
ou criava o arquivo antes de validar todas as chaves. O destrutor de
BTreeDatabase chama close(), que pode confirmar modificações anteriores mesmo
quando uma inserção lança exceção. Agora o lote inteiro é validado antes de
abrir o arquivo, e erros durante a aplicação provocam rollback. Lotes vazios
não criam arquivos. O formato World4, remapeamentos e saves existentes não
são convertidos, apagados ou reparados por essa mudança.

Diagnóstico adicional:

- Exceções no Android capturam endereços com `_Unwind_Backtrace` e mostram
  deslocamentos relativos à biblioteca usando `dladdr`. Android liga libdl
  explicitamente. Windows, iOS e Switch mantêm seu caminho de captura.
- Export Diagnostics inclui até três registros de encerramento do próprio
  aplicativo no Android 11+. No Android 12+, tenta incluir os tombstones de
  crashes nativos como protobuf em `crashes/native-*.pb`. A indisponibilidade
  é registrada em `crashes/exits.txt`; cada relatório tem limite de 4 MiB.
  Isso usa a [API oficial ApplicationExitInfo](https://developer.android.com/reference/android/app/ApplicationExitInfo#getTraceInputStream()).
- Nenhum relatório é enviado automaticamente. O usuário continua exportando
  e compartilhando o ZIP pelo botão existente.

Validação executada neste ambiente Windows:

- NDK r26d / Android arm64 API 28: StarWorldStorage.cpp e
  StarException_unix.cpp passaram na verificação de sintaxe, sem avisos.
- Zig 0.16 / Windows desktop e launcher: StarWorldStorage.cpp compilou nas
  duas variantes; cinco avisos preexistentes em cada uma, nenhum novo.
- world_chunks_test.cpp compilou no Android arm64 e nas duas variantes
  Windows. Os três testes verificam rejeição antes de criar o arquivo,
  preservação byte a byte de um save em lote inválido, e inserção,
  substituição e remoção após serialização. **Não foram executados**: não há
  uma compilação completa do engine disponível neste host.
- javac --release 17: todos os fontes Java do aplicativo compilaram contra
  android.jar da API 35 e AndroidX core 1.13.1/documentfile 1.0.1. Apenas
  notas de APIs obsoletas preexistentes.
- CrashDiagnosticsReadTest executou na JVM local: 12 verificações passaram,
  incluindo limite de 4 MiB, leitura incompleta por erro e rejeição de excesso.
  Não exercita o serviço ActivityManager de um aparelho.
- git diff --check passou.

Os comandos e logs locais estão em work/validation-world-entry (fora do repo).
Não foram validados runtime Android, iOS ou Switch nem um APK completo local.
Esta alteração NÃO confirma resolução do crash de entrada no mundo e NÃO
recupera automaticamente a nave que já apresentou erro. O próximo passo é
gerar um APK dessa branch, reproduzir uma vez e exportar o diagnóstico novo.
O ZIP e os saves privados do usuário não fazem parte do commit.

Para repetir o teste Java, compile os fontes Java do aplicativo com o SDK e
AndroidX, acrescente source/test/CrashDiagnosticsReadTest.java à compilação e
execute io.github.openstarbound.mobile.CrashDiagnosticsReadTest com o diretório
de classes e android.jar no classpath. Os testes de armazenamento estão em
`game_tests --gtest_filter=WorldChunks.*`; o runner atual de game_tests também
inicializa Root e precisa de assets próprios, apesar de esses testes usarem
apenas arquivos temporários e operações estáticas de armazenamento.
