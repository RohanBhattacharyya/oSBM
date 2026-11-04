# Trava de alvo opcional

A função começa desligada. No launcher, em Controls/Overlay, habilite **Enable
optional target lock** e salve. No próximo lançamento, os botões **Lock** e
**Next** ficam disponíveis. O gamepad oferece as mesmas ações na lista de
remapeamento; nenhum botão existente, inclusive R3, foi substituído.

- **Lock** trava um inimigo próximo da direção da mira; pressionar novamente
  destrava. Não pressiona o gatilho nem ataca por conta própria. As opções de
  ataque por toque continuam independentes.
- **Next** percorre os outros alvos elegíveis na direção original de aquisição,
  com retorno ao início. Os comandos agem uma vez por pressionamento.
- **Prefer bosses when acquiring a target** começa ligado, mas só tem efeito
  se a função opcional estiver habilitada. A prioridade vale para a seleção
  explícita. Um alvo mantido não é trocado porque outra criatura apareceu,
  ficou mais próxima, ou porque um boss entrou na tela.
- A mira acompanha o mesmo EntityId/objeto. Um marcador laranja identifica o
  alvo. A função não aumenta dano, alcance da arma, velocidade do projétil ou
  chances de acerto; não calcula antecipação de disparos.
- O alcance de busca é configurável de 8 a 80 blocos, padrão 40. São exigidos
  alvo vivo, visível na tela, sem parede na linha até seu centro, tipo Monster
  ou Npc, time Enemy, condição agressiva (ou barra especial) e permissão de dano.
  Não seleciona aliados, pets, jogadores de PvP ou criaturas neutras.
- Perder alcance/visão/alvo, trocar os itens das mãos, abrir janela/chat,
  entrar em cinematics ou perder foco cancela. Não procura substituto sozinho.
  Mover mouse físico também cancela. Joystick continua disponível para a mira
  livre ao destravar; enquanto travado, o alvo tem precedência.
- Armas precisam da tag padrão `weapon`. Ferramentas e blocos não recebem
  assistência. Usar uma mão sem arma cancela a trava antes da ação dessa mão.
  Uma arma de mod sem a tag fica sem assistência por segurança.

## Bosses de mods

Não há lista fechada de nomes, nem palpite baseado em HP. A prioridade usa
`DamageBarEntity::damageBar() == DamageBarType::Special`, informação já exposta
pela engine e usada pelas barras especiais. Portanto um boss novo que use esse
padrão recebe prioridade sem adicionar seu nome ao código. A classificação é
uma indicação de boss, não uma garantia de que todo mod a use dessa maneira.

Bosses sem barra especial ainda podem ser travados como inimigos comuns,
desde que atendam aos filtros; use a direção da mira e **Next** para escolhê-los.
Depois de selecionados, também permanecem travados apesar dos minions. Mods que
representem um boss por projétil, objeto, criatura não agressiva/time diferente,
ou por várias entidades invisíveis precisam de adaptação específica. Troca de
fase que destrói uma entidade e cria outra exige uma nova seleção: IDs reutilizados
não transferem a trava silenciosamente.

## Validação

`target_lock_tests` registrado no CMake como NoAssets; compilação standalone:

```text
zig c++ -std=c++17 -Wall -Wextra -Werror -I source/core source/test/target_lock_test.cpp -o ../target_lock_tests.exe
Passed 148 target lock checks
```

Os testes exercitam cone de seleção, ranking por direção/distância, prioridade
opcional de boss, desempate por ID, ciclo com três alvos e as 128 combinações
de existência/vida/hostilidade/visibilidade/alcance/arma/obstáculo. A busca só
ocorre ao apertar Lock/Next; acompanhar o alvo não refaz a busca global.

Regressões executadas: 7.675 checks de toque e 31.695 de gamepad passaram.
A verificação dos quatro C++ alterados usa o NDK r26d arm64/API 28, Windows
launcher e Windows clássico nos compartilhados; resultados em
`optional-target-lock-validation.md`. Não houve execução no aparelho ou com
mods reais. Nenhum APK novo foi gerado nesta etapa; a limitação local de
Visual Studio/vcpkg descrita no relatório anterior permanece.

## Testar no jogo

1. Deixe a opção desligada: confira controles, configurações antigas e R3.
2. Habilite e salve; equipe arma com tag weapon. Aponte e aperte Lock; confirme
   marcador, acompanhamento e necessidade de apertar ataque para disparar.
3. Trave um boss vanilla; invoque/espere minions: a mira deve ficar no boss.
4. Teste boss de mod com barra especial e preferência ligada/desligada. Teste
   boss sem essa marca usando Lock/Next; registre mod, versão e comportamento.
5. Alterne Next entre pelo menos três inimigos. Destrave, mude direção e adquira
   novamente. Não deve mirar em aliado, pet, neutro ou alguém atrás de parede.
6. Mate/perca o alvo, saia de alcance/tela ou coloque parede: deve destravar,
   sem disparar nem escolher outro. Teste fases que recriam o boss/partes do corpo.
7. Equipe bloco/ferramenta, use a outra mão, abra inventário/chat/pausa, alterne
   aplicativos e reconecte gamepad. Confira mira livre e nenhuma ação presa.
8. Remapeie Lock/Next no gamepad. Repita com gamepad desabilitado usando só toque,
   com mouse físico e com as opções de ataque ao mirar já existentes.
