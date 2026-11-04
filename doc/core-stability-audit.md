# Revisão preventiva de estabilidade

Base desta etapa: `685c006bd47100d7f895d52e007a699f13cd18b9`.
Branch: `codex/android-world-entry-diagnostics`, PR #5. O usuário esclareceu
que não tinha outros sintomas específicos: pediu correções de erros adicionais
encontrados na revisão. Não há issues abertas no fork na consulta realizada.

## Defeitos corrigidos

| Arquivos | Defeito e resultado |
|---|---|
| source/core/StarByteArray.cpp | sub(begin, length) usava begin+length como quantidade de bytes. Isso devolvia dados além do recorte e podia ler além do buffer. Agora limita a quantidade ao restante do buffer, sem soma sujeita a overflow; início fora do buffer ou tamanho zero devolvem vazio. left/right passam a respeitar o recorte correto. |
| source/core/StarByteArray.hpp | append no próprio objeto podia realocar e depois copiar de um ponteiro invalidado. Agora preserva a origem numa cópia antes de crescer. Acrescentar zero bytes retorna sem acessar ponteiros nulos. |
| source/core/StarFlatHashMap.hpp e StarFlatHashSet.hpp | Autoatribuição por cópia esvaziava a origem, que era o próprio destino. Agora é uma operação sem alteração de dados. |
| source/core/StarOrderedMap.hpp | take usava o iterador da tabela depois de apagá-lo; uma colisão de hash podia fazer a remoção atingir outro item da lista. Agora copia o valor antes da alteração e usa o iterador estável da lista para remover a entrada certa. |
| source/base/StarAssets.cpp | Dois avisos de patches de imagem tinham placeholders sem argumentos. Com avisos ativos, um retorno não Image do script podia provocar erro de formatação em vez de ser ignorado. Agora os avisos recebem caminho do patch e da imagem. |

Os testes de regressão foram incluídos nos arquivos existentes:
source/test/byte_array_test.cpp, flat_hash_test.cpp, ordered_map_test.cpp e
assets_test.cpp. A extração ordenada inclui um hash que força colisões; o teste
de patch inválido instala temporariamente um coletor de avisos para realmente
exercitar a formatação mesmo quando o runner geral só registra erros.

Não foram modificados controles, perfis, formato de save, assinatura, versão,
ABIs, compactação ou configuração do APK. Estes defeitos adicionais não foram
atribuídos ao crash original sem evidência no aparelho.

## Verificações executadas

Uma reprodução nativa pequena antes/depois confirmou a perda por autoatribuição:

```text
antes: map=0 set=0
agora: map=2 set=3
```

Outra reprodução com os fontes reais de ByteArray e os headers das coleções
confirmou recorte incorreto e remoção da entrada errada sob colisão:

```text
before exit 1 slice=4 take=20 remaining keys: 1 2
after  exit 0 slice=2 take=20 remaining keys: 1 3
```

Um programa isolado executou 3.783.160 verificações de recortes, limites,
autoappend, autoatribuição e extração ordenada sem falhas. Este programa usa
StarByteArray.cpp/StarMemory.cpp e os headers reais. Para conseguir ligar sem o
engine completo, substitui somente apresentação de exceções e impressão de
strings/hex por suporte de teste; as funções de impressão substituídas lançam
se forem chamadas inesperadamente. Não substitui operações de bytes ou
coleções. Isso valida essas operações na execução local Windows, não o jogo
completo nem captura de erros no Android.

Comandos locais: python ../check_before_after.py e
python ../run_functional_probe.py. Fontes dos programas e suporte de teste
ficam em work/, fora do repo.

A matriz de compilação final passou nas seis combinações:

| Implementação | Android arm64 / NDK r26d | Windows launcher / Zig 0.16 | Windows desktop / Zig 0.16 |
|---|---|---|---|
| StarByteArray.cpp | 0 avisos | 0 avisos | 0 avisos |
| StarAssets.cpp | 2 avisos preexistentes | 7 avisos preexistentes | 8 avisos preexistentes |

Nenhum aviso novo em relação à base. Comando:
python ../validate_world_entry.py --baseline 685c006 --work-dir
../validation-core-audit-final (com os mesmos caminhos de toolchain da etapa
anterior). Logs e comandos completos em work/validation-core-audit-final.

Os quatro arquivos de testes compilaram nas três variantes, totalizando 12
combinações. Após reforçar os casos de colisão e captura do aviso, os dois
arquivos correspondentes foram recompilados nas três variantes com sucesso.
Comandos: python ../check_core_audit_tests.py e
python ../check_core_audit_final_tests.py. git diff --check passou.

Os runners completos core_tests/game_tests não foram ligados ou executados
neste host, e o novo teste de assets não foi executado. O build completo local
continua indisponível por dependências/toolchain de host; tentativas isoladas de
ligação sem suporte de diagnóstico não substituem esse build. Não foi gerado
um APK desta etapa. Android só recebeu verificação de sintaxe; iOS e Switch
não foram compilados nem executados.

Esta revisão conclui os defeitos confirmados acima. A confirmação da entrada
no mundo e do comportamento completo do jogo ainda requer gerar um APK da
branch e testar no aparelho. Não significa que todo o engine e todos os mods
estejam livres de defeitos. As alterações continuam separadas da main.
